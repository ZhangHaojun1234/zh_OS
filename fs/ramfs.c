/* ============================================================================
 * zh_OS 1.0 (x86_64)
 * File:    fs/ramfs.c
 * Version: 1.0
 * Brief:   ramfs 内存态文件系统实现（节点管理、路径解析、权限判断）。
 *
 * B 组第 1 个功能，纯增量「只添加，不修改」：本文件与 ramfs.h 都是新文件，
 * 已有模块只追加 include 与初始化调用 / 命令注册，不改动任何已有行。
 * ============================================================================ */

#include "ramfs.h"

#include <stdbool.h>

#include "../users/users.h"   /* user_current / user_is_root：权限判断 */
#include "../mm/heap.h"       /* kmalloc / kfree：节点与文件内容分配 */

/* ------------------------------------------------------------------ */
/* 本地常量与工具                                                     */
/* ------------------------------------------------------------------ */

#define RAMFS_MAX_COMP  16u    /* 路径最多 16 段（含 ~ 展开后） */
#define RAMFS_COMP_SZ   64u    /* 单段名字长度（与 name[64] 对齐） */
#define RAMFS_PATH_MAX  256u   /* 绝对路径缓冲上限 */

/* freestanding 无 libc，自行实现最小字符串工具 */
static int ramfs_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void ramfs_strcpy(char *dst, const char *src)
{
    uint32_t i = 0;
    while (src[i] != '\0') {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = '\0';
}

/* 全局根与当前目录（仅本文件可见） */
static ramfs_node_t *g_root = 0;
static ramfs_node_t *g_cwd  = 0;

/* ------------------------------------------------------------------ */
/* 节点创建与链表操作                                                 */
/* ------------------------------------------------------------------ */

static ramfs_node_t *ramfs_node_new(const char *name, ramfs_type_t type)
{
    ramfs_node_t *n = (ramfs_node_t *)kmalloc(sizeof(ramfs_node_t));
    if (n == 0) {
        return 0;
    }
    ramfs_strcpy(n->name, name);
    n->type    = type;
    n->size    = 0;
    n->data    = 0;
    n->parent  = 0;
    n->children = 0;
    n->next    = 0;
    return n;
}

/* 把 child 头插到 parent 的子节点链表（不查重，调用方保证唯一） */
static void node_attach(ramfs_node_t *parent, ramfs_node_t *child)
{
    child->parent = parent;
    child->next   = parent->children;
    parent->children = child;
}

static ramfs_node_t *node_find_child(ramfs_node_t *dir, const char *name)
{
    ramfs_node_t *c = dir->children;
    while (c != 0) {
        if (ramfs_strcmp(c->name, name) == 0) {
            return c;
        }
        c = c->next;
    }
    return 0;
}

/* 从 parent 的子链表中摘除 child（不释放内存） */
static void node_detach(ramfs_node_t *parent, ramfs_node_t *child)
{
    ramfs_node_t *prev = 0;
    ramfs_node_t *c = parent->children;
    while (c != 0) {
        if (c == child) {
            if (prev == 0) {
                parent->children = c->next;
            } else {
                prev->next = c->next;
            }
            return;
        }
        prev = c;
        c = c->next;
    }
}

/* ------------------------------------------------------------------ */
/* 权限：当前用户 home 路径与 home 子树判定                            */
/* ------------------------------------------------------------------ */

/* 当前用户的 home 绝对路径；init 阶段（未登录）或 home 为空时返回 "/" */
static const char *home_path(void)
{
    const zh_user_t *u = user_current();
    if (u == 0 || u->home[0] == '\0') {
        return "/";
    }
    return u->home;
}

/* 计算节点绝对路径写入 buf（如 "/home/zh"）；根节点为 "/" */
static void node_abs_path(ramfs_node_t *node, char *buf, uint32_t cap)
{
    if (node == 0) {
        if (cap != 0) {
            buf[0] = '\0';
        }
        return;
    }

    /* names[0]=node … names[n-1]=root，再由 root 向 node 拼接 */
    char names[RAMFS_MAX_COMP][RAMFS_COMP_SZ];
    int n = 0;
    ramfs_node_t *c = node;
    while (c != 0 && n < (int)RAMFS_MAX_COMP) {
        if (c == g_root) {
            ramfs_strcpy(names[n], "/");   /* 根节点名字固定为 "/" */
        } else {
            ramfs_strcpy(names[n], c->name);
        }
        ++n;
        c = c->parent;
    }

    uint32_t w = 0;
    for (int i = n - 1; i >= 0; --i) {
        if (names[i][0] == '/') {
            /* 根：仅在最开头写一个前导 '/' */
            if (w == 0) {
                if (w + 1 < cap) {
                    buf[w++] = '/';
                }
            }
        } else {
            /* 非根组件：若上一个字符不是 '/' 则补分隔符 */
            if (w > 0 && buf[w - 1] != '/') {
                if (w + 1 < cap) {
                    buf[w++] = '/';
                }
            }
            uint32_t k = 0;
            while (names[i][k] != '\0' && w + 1 < cap) {
                buf[w++] = names[i][k++];
            }
        }
    }
    if (w == 0) {   /* 极端情况下至少输出 "/" */
        buf[0] = '/';
        w = 1;
    }
    buf[w] = '\0';
}

/* node 是否位于当前用户 home 子树内（含 home 本身） */
static bool in_home(ramfs_node_t *node)
{
    char ap[RAMFS_PATH_MAX];
    node_abs_path(node, ap, sizeof(ap));
    const char *home = home_path();
    uint32_t hl = 0;
    while (home[hl] != '\0') {
        ++hl;
    }
    if (hl == 0) {
        return false;
    }
    /* 等于 home，或以 home + '/' 为前缀 */
    if (ramfs_strcmp(ap, home) == 0) {
        return true;
    }
    if (hl < RAMFS_PATH_MAX) {
        uint32_t i = 0;
        while (i < hl && ap[i] == home[i]) {
            ++i;
        }
        if (i == hl && ap[hl] == '/') {
            return true;
        }
    }
    return false;
}

/* 写权限：init 阶段（user_current()==NULL）与 root 任意写；普通用户限 home 子树 */
static bool can_write(ramfs_node_t *dir)
{
    if (user_current() == 0) {
        return true;
    }
    if (user_is_root()) {
        return true;
    }
    return in_home(dir);
}

/* [ADD] M8：`/bin` 是"程序目录"，按 Linux 惯例对所有人可读（相当于 0755）。
 * ramfs 没有 mode 位，所以这里按路径判断：根节点下的 bin 子树一律可读可进入。
 * 有了它，普通用户才能 exec("/bin/xxx.elf") —— 就像 Linux 里谁都能执行 /bin/sh。 */
static bool in_bin_dir(ramfs_node_t *node)
{
    ramfs_node_t *n = node;
    const char *want = "bin";

    while (n != 0 && n->parent != 0) {
        if (n->parent->parent == 0) {
            int i = 0;
            while (want[i] != '\0' && n->name[i] == want[i]) {
                ++i;
            }
            return want[i] == '\0' && n->name[i] == '\0';
        }
        n = n->parent;
    }
    return false;
}

/* 读/进入权限：同写权限的归属规则（读文件、cd 目录均受此约束） */
static bool can_read(ramfs_node_t *dir)
{
    if (user_current() == 0) {
        return true;
    }
    if (user_is_root()) {
        return true;
    }
    return in_home(dir) || in_bin_dir(dir);
}

/* ------------------------------------------------------------------ */
/* 路径解析                                                           */
/* ------------------------------------------------------------------ */

/*
 * 把 path 切成段，处理：
 *   - 绝对路径（以 '/' 开头）或 ~ 前缀（展开为 home 的绝对段）
 *   - 重复 '/' 视为单分隔符
 * 返回 absolute（是否绝对/以~开头），段写入 comps，*ncomp 为段数。
 * home 在 init 阶段为 "/"，故 ~ 展开为空集，等价于回到根。
 */
static int tokenize(const char *path, bool *absolute,
                    char comps[RAMFS_MAX_COMP][RAMFS_COMP_SZ], int *ncomp)
{
    *ncomp = 0;
    *absolute = (path[0] == '/' || path[0] == '~');
    const char *p = path;

    if (path[0] == '~') {
        /* ~ 展开为 home，作为绝对基址（与 '/' 前缀同样对待） */
        const char *home = home_path();
        const char *h = home;
        while (h[0] != '\0') {
            while (h[0] == '/') {
                ++h;
            }
            if (h[0] == '\0') {
                break;
            }
            char hc[RAMFS_COMP_SZ];
            uint32_t j = 0;
            while (h[0] != '\0' && h[0] != '/' && j + 1 < RAMFS_COMP_SZ) {
                hc[j++] = h[0];
                ++h;
            }
            hc[j] = '\0';
            if (*ncomp >= (int)RAMFS_MAX_COMP) {
                return RAMFS_ERR_INVAL;
            }
            ramfs_strcpy(comps[*ncomp], hc);
            ++(*ncomp);
        }
        ++p;   /* 跳过 '~' 自身，后续交给主循环处理 "/" 与剩余段 */
    }

    while (p[0] != '\0') {
        while (p[0] == '/') {
            ++p;
        }
        if (p[0] == '\0') {
            break;
        }
        char tok[RAMFS_COMP_SZ];
        uint32_t i = 0;
        while (p[0] != '\0' && p[0] != '/' && i + 1 < RAMFS_COMP_SZ) {
            tok[i++] = p[0];
            ++p;
        }
        tok[i] = '\0';
        /* 仅首段才可能是 ~（已在上面处理），其余 ~ 当作普通名字忽略 */
        if (*ncomp >= (int)RAMFS_MAX_COMP) {
            return RAMFS_ERR_INVAL;
        }
        ramfs_strcpy(comps[*ncomp], tok);
        ++(*ncomp);
    }
    return RAMFS_OK;
}

/* 规整段列表：丢弃 "."，遇到 ".." 回退一段（到根不再上移） */
static int normalize(char comps[RAMFS_MAX_COMP][RAMFS_COMP_SZ], int *ncomp)
{
    int out = 0;
    for (int k = 0; k < *ncomp; ++k) {
        if (ramfs_strcmp(comps[k], ".") == 0) {
            continue;
        }
        if (ramfs_strcmp(comps[k], "..") == 0) {
            if (out > 0) {
                --out;
            }
            continue;
        }
        if (out != k) {
            ramfs_strcpy(comps[out], comps[k]);
        }
        ++out;
    }
    if (out > (int)RAMFS_MAX_COMP) {
        return RAMFS_ERR_INVAL;
    }
    *ncomp = out;
    return RAMFS_OK;
}

/* 把相对路径先拼到 cwd 的绝对段之后，再统一规整，得到从根出发的绝对段集合。
 * 注意：用独立的 tmp 数组做拼接，避免 path 段与 cwd 段在同一 comps 上互相覆盖。 */
static int build_abs_components(const char *path,
                                char comps[RAMFS_MAX_COMP][RAMFS_COMP_SZ], int *n)
{
    bool absolute;
    int rc = tokenize(path, &absolute, comps, n);
    if (rc != RAMFS_OK) {
        return rc;
    }
    if (!absolute) {
        /* 相对路径：把 cwd 的绝对段作为前缀，再追加 path 的段 */
        char cwdap[RAMFS_PATH_MAX];
        node_abs_path(g_cwd, cwdap, sizeof(cwdap));
        bool ign;
        char cwdcomp[RAMFS_MAX_COMP][RAMFS_COMP_SZ];
        int cwd_count = 0;
        tokenize(cwdap, &ign, cwdcomp, &cwd_count);   /* cwd 自己的绝对段 */

        char tmp[RAMFS_MAX_COMP][RAMFS_COMP_SZ];
        int m = 0;
        for (int k = 0; k < cwd_count && m < (int)RAMFS_MAX_COMP; ++k) {
            ramfs_strcpy(tmp[m], cwdcomp[k]);
            ++m;
        }
        for (int k = 0; k < *n && m < (int)RAMFS_MAX_COMP; ++k) {
            ramfs_strcpy(tmp[m], comps[k]);
            ++m;
        }
        for (int k = 0; k < m; ++k) {
            ramfs_strcpy(comps[k], tmp[k]);
        }
        *n = m;
    }
    return normalize(comps, n);
}

/* 从根按段下行；任一段缺失返回 NULL 并置 *err=NOENT，中途非目录置 NOTDIR */
static ramfs_node_t *descend(ramfs_node_t *base,
                             char comps[RAMFS_MAX_COMP][RAMFS_COMP_SZ],
                             int n, int *err)
{
    ramfs_node_t *cur = base;
    for (int k = 0; k < n; ++k) {
        if (cur->type != RAMFS_DIR) {
            *err = RAMFS_ERR_NOTDIR;
            return 0;
        }
        ramfs_node_t *child = node_find_child(cur, comps[k]);
        if (child == 0) {
            *err = RAMFS_ERR_NOENT;
            return 0;
        }
        cur = child;
    }
    return cur;
}

/* 解析到最终节点（用于 lookup / chdir / read） */
static ramfs_node_t *resolve_node(const char *path, int *err)
{
    char comps[RAMFS_MAX_COMP][RAMFS_COMP_SZ];
    int n = 0;
    int rc = build_abs_components(path, comps, &n);
    if (rc != RAMFS_OK) {
        *err = rc;
        return 0;
    }
    return descend(g_root, comps, n, err);
}

/* 解析到父目录并取出最后一段名字（用于 mkdir / touch / rm / write） */
static ramfs_node_t *resolve_parent(const char *path, char *out_name, int *err)
{
    char comps[RAMFS_MAX_COMP][RAMFS_COMP_SZ];
    int n = 0;
    int rc = build_abs_components(path, comps, &n);
    if (rc != RAMFS_OK) {
        *err = rc;
        return 0;
    }
    if (n == 0) {
        *err = RAMFS_ERR_INVAL;   /* 路径即根 "/"，没有可操作的父/名 */
        return 0;
    }
    ramfs_strcpy(out_name, comps[n - 1]);
    return descend(g_root, comps, n - 1, err);
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

void ramfs_init(void)
{
    g_root = ramfs_node_new("/", RAMFS_DIR);
    g_cwd  = g_root;
    if (g_root == 0) {
        return;   /* 极端 OOM：静默，后续命令会因找不到节点而报错 */
    }
    /* init 阶段 user_current()==NULL，权限放行，可建系统目录 */
    ramfs_mkdir("/root");
    ramfs_mkdir("/home");
    ramfs_mkdir("/home/zh");
}

ramfs_node_t *ramfs_lookup(const char *path)
{
    int err = RAMFS_OK;
    return resolve_node(path, &err);
}

/* mkdir 与 touch 共用：创建空目录或空文件，parent 须可写、目标不存在 */
static int ramfs_create(const char *path, ramfs_type_t type)
{
    int err = RAMFS_OK;
    char name[RAMFS_COMP_SZ];
    ramfs_node_t *parent = resolve_parent(path, name, &err);
    if (parent == 0) {
        return err;
    }
    if (!can_write(parent)) {
        return RAMFS_ERR_PERM;
    }
    if (node_find_child(parent, name) != 0) {
        return RAMFS_ERR_EXIST;
    }
    ramfs_node_t *nd = ramfs_node_new(name, type);
    if (nd == 0) {
        return RAMFS_ERR_NOMEM;
    }
    node_attach(parent, nd);
    return RAMFS_OK;
}

int ramfs_mkdir(const char *path)
{
    return ramfs_create(path, RAMFS_DIR);
}

int ramfs_touch(const char *path)
{
    return ramfs_create(path, RAMFS_FILE);
}

int ramfs_rm(const char *path)
{
    int err = RAMFS_OK;
    char name[RAMFS_COMP_SZ];
    ramfs_node_t *parent = resolve_parent(path, name, &err);
    if (parent == 0) {
        return err;
    }
    if (!can_write(parent)) {
        return RAMFS_ERR_PERM;
    }
    ramfs_node_t *child = node_find_child(parent, name);
    if (child == 0) {
        return RAMFS_ERR_NOENT;
    }
    if (child->type == RAMFS_DIR && child->children != 0) {
        return RAMFS_ERR_NOTEMPTY;   /* 只删空目录 */
    }
    node_detach(parent, child);
    if (child->data != 0) {
        kfree(child->data);
    }
    kfree(child);
    return RAMFS_OK;
}

int ramfs_write_file(const char *path, const uint8_t *buf, uint64_t len)
{
    int err = RAMFS_OK;
    char name[RAMFS_COMP_SZ];
    ramfs_node_t *parent = resolve_parent(path, name, &err);
    if (parent == 0) {
        return err;
    }
    if (!can_write(parent)) {
        return RAMFS_ERR_PERM;
    }
    ramfs_node_t *child = node_find_child(parent, name);
    if (child == 0) {
        child = ramfs_node_new(name, RAMFS_FILE);
        if (child == 0) {
            return RAMFS_ERR_NOMEM;
        }
        node_attach(parent, child);
    } else if (child->type == RAMFS_DIR) {
        return RAMFS_ERR_ISDIR;
    }
    uint8_t *nd = (uint8_t *)kmalloc(len != 0 ? len : 1u);
    if (nd == 0) {
        return RAMFS_ERR_NOMEM;
    }
    for (uint64_t i = 0; i < len; ++i) {
        nd[i] = buf[i];
    }
    if (child->data != 0) {
        kfree(child->data);
    }
    child->data = nd;
    child->size = len;
    return RAMFS_OK;
}

int ramfs_read_file(const char *path, uint8_t *buf, uint64_t maxlen, uint64_t *outlen)
{
    int err = RAMFS_OK;
    ramfs_node_t *node = resolve_node(path, &err);
    if (node == 0) {
        return err;
    }
    if (node->type != RAMFS_FILE) {
        return RAMFS_ERR_ISDIR;
    }
    if (!can_read(node)) {
        return RAMFS_ERR_PERM;
    }
    uint64_t n = node->size;
    if (n > maxlen) {
        n = maxlen;
    }
    for (uint64_t i = 0; i < n; ++i) {
        buf[i] = node->data[i];
    }
    *outlen = n;
    return RAMFS_OK;
}

int ramfs_chdir(const char *path)
{
    int err = RAMFS_OK;
    ramfs_node_t *node = resolve_node(path, &err);
    if (node == 0) {
        return err;
    }
    if (node->type != RAMFS_DIR) {
        return RAMFS_ERR_NOTDIR;
    }
    if (!can_read(node)) {
        return RAMFS_ERR_PERM;   /* 普通用户不能 cd 出 home 子树 */
    }
    g_cwd = node;
    return RAMFS_OK;
}

const char *ramfs_get_cwd(void)
{
    static char disp[RAMFS_PATH_MAX];
    char ap[RAMFS_PATH_MAX];
    node_abs_path(g_cwd, ap, sizeof(ap));

    if (!user_is_root()) {
        /* 普通用户：位于自己 home 时显示 "~"，在 home 子树下显示 "~/xxx" */
        const char *home = home_path();
        uint32_t hl = 0;
        while (home[hl] != '\0') {
            ++hl;
        }
        if (hl != 0) {
            if (ramfs_strcmp(ap, home) == 0) {
                disp[0] = '~';
                disp[1] = '\0';
                return disp;
            }
            uint32_t i = 0;
            while (i < hl && ap[i] == home[i]) {
                ++i;
            }
            if (i == hl && ap[hl] == '/') {
                uint32_t w = 0;
                disp[w++] = '~';
                uint32_t k = hl;
                while (ap[k] != '\0' && w + 1 < RAMFS_PATH_MAX) {
                    disp[w++] = ap[k++];
                }
                disp[w] = '\0';
                return disp;
            }
        }
    }
    /* root 或越出 home：返回绝对路径 */
    uint32_t w = 0;
    while (ap[w] != '\0' && w + 1 < RAMFS_PATH_MAX) {
        disp[w] = ap[w];
        ++w;
    }
    disp[w] = '\0';
    return disp;
}

const char *ramfs_strerror(int rc)
{
    switch (rc) {
    case RAMFS_ERR_NOENT:    return "no such file or directory";
    case RAMFS_ERR_EXIST:    return "file exists";
    case RAMFS_ERR_NOTDIR:   return "not a directory";
    case RAMFS_ERR_PERM:     return "permission denied";
    case RAMFS_ERR_NOTEMPTY: return "directory not empty";
    case RAMFS_ERR_ISDIR:    return "is a directory";
    case RAMFS_ERR_INVAL:    return "invalid argument";
    case RAMFS_ERR_NOMEM:    return "out of memory";
    default:                 return "error";
    }
}

/* ------------------------------------------------------------------ */
/* [ADD] M9：给 fd 表用的三个接口                                     */
/* ------------------------------------------------------------------ */

/* 打开前的检查：路径存在 + 是文件 + 当前用户可读。成功回填节点指针。 */
int ramfs_open_check(const char *path, ramfs_node_t **out)
{
    int err = RAMFS_OK;
    ramfs_node_t *node = resolve_node(path, &err);

    if (node == 0) {
        return err;
    }
    if (node->type != RAMFS_FILE) {
        return RAMFS_ERR_ISDIR;
    }
    if (!can_read(node)) {
        return RAMFS_ERR_PERM;
    }
    *out = node;
    return RAMFS_OK;
}

/*
 * [M14] readdir：按序号枚举目录条目（Linux 的 getdents 的雏形）。
 *   path  : 目录路径（NULL 或空串 = 当前目录）
 *   index : 从 0 开始的序号
 *   name  : 输出缓冲，写节点名（不含 '/'）
 *   cap   : 输出缓冲容量
 *   type  : 回填 RAMFS_FILE / RAMFS_DIR
 * 返回：RAMFS_OK 成功；RAMFS_ERR_NOENT 没有这个序号了（枚举结束）；
 *       RAMFS_ERR_NOTDIR 不是目录；RAMFS_ERR_PERM 越权。
 *
 * 为什么不返回一个"目录句柄"？因为本版没有 fd 之外的进程资源概念，
 * 用"序号"足以让 ls 一页页翻完一个目录 —— 和 Linux 的 readdir(fd) 效果一样，
 * 只是少了那个 fd（真正省事的地方在于调用方不需要 close）。
 */
int ramfs_readdir(const char *path, uint32_t index, char *name, uint32_t cap,
                  ramfs_type_t *type)
{
    int err = RAMFS_OK;
    ramfs_node_t *dir;
    ramfs_node_t *child;
    uint32_t n = 0u;

    if (name == (char *)0 || cap == 0u) {
        return RAMFS_ERR_INVAL;
    }
    if (path == (const char *)0 || path[0] == '\0') {
        dir = ramfs_lookup(".");            /* 当前目录 */
    } else {
        dir = resolve_node(path, &err);
        if (dir == (ramfs_node_t *)0) {
            return err;
        }
    }
    if (dir->type != RAMFS_DIR) {
        return RAMFS_ERR_NOTDIR;
    }
    if (!can_read(dir)) {
        return RAMFS_ERR_PERM;
    }

    for (child = dir->children; child != (ramfs_node_t *)0; child = child->next) {
        if (n == index) {
            uint32_t i = 0u;

            while (child->name[i] != '\0' && i + 1u < cap) {
                name[i] = child->name[i];
                ++i;
            }
            name[i] = '\0';
            if (type != (ramfs_type_t *)0) {
                *type = child->type;
            }
            return RAMFS_OK;
        }
        ++n;
    }
    return RAMFS_ERR_NOENT;
}
uint64_t ramfs_node_size(const ramfs_node_t *node)
{
    return (node == 0) ? 0u : node->size;
}

/*
 * 从 node 的 off 处读最多 maxlen 字节。这里不做权限检查 —— open 时已经查过，
 * 与 Linux 一致（read 只认 fd，不再回头查 inode 权限）。
 */
int ramfs_node_read_at(ramfs_node_t *node, uint64_t off, uint8_t *buf,
                       uint64_t maxlen, uint64_t *outlen)
{
    uint64_t n = 0u;

    if (outlen == (uint64_t *)0) {
        return RAMFS_ERR_INVAL;
    }
    *outlen = 0u;
    if (node == 0 || buf == 0) {
        return RAMFS_ERR_INVAL;
    }
    if (node->type != RAMFS_FILE) {
        return RAMFS_ERR_ISDIR;
    }
    if (off >= node->size || node->data == 0) {
        return RAMFS_OK;   /* 越过文件尾：读 0 字节，就是 EOF */
    }
    n = node->size - off;
    if (n > maxlen) {
        n = maxlen;
    }
    for (uint64_t i = 0; i < n; ++i) {
        buf[i] = node->data[off + i];
    }
    *outlen = n;
    return RAMFS_OK;
}

/* ------------------------------------------------------------------ */
/* [ADD] M10：可写文件（creat / append）                                */
/* ------------------------------------------------------------------ */

/*
 * creat：父目录可写才允许；目标不存在就新建，存在且是文件就清空（O_TRUNC）。
 * 与 Linux 的 creat(path, 0666) 对应，只是这里没有 mode 位。
 */
int ramfs_create_file(const char *path, ramfs_node_t **out)
{
    int err = RAMFS_OK;
    char name[RAMFS_COMP_SZ];
    ramfs_node_t *parent = resolve_parent(path, name, &err);
    ramfs_node_t *child;

    if (parent == 0) {
        return err;
    }
    if (!can_write(parent)) {
        return RAMFS_ERR_PERM;
    }
    child = node_find_child(parent, name);
    if (child == 0) {
        child = ramfs_node_new(name, RAMFS_FILE);
        if (child == 0) {
            return RAMFS_ERR_NOMEM;
        }
        node_attach(parent, child);
    } else if (child->type == RAMFS_DIR) {
        return RAMFS_ERR_ISDIR;
    }
    if (child->data != 0) {
        kfree(child->data);
        child->data = 0;
    }
    child->size = 0u;
    *out = child;
    return RAMFS_OK;
}

/*
 * 追加写：重新申请一块更大的内存、拷旧内容、接上新内容、释放旧的。
 * 这就是 Linux 里"文件数据在页缓存/块设备上增长"的最小版本 —— 内容真的变了，
 * 不是往某个缓冲区里一塞就算数。
 */
int ramfs_node_append(ramfs_node_t *node, const uint8_t *buf, uint64_t len)
{
    uint8_t *nd;
    uint64_t old;
    uint64_t i;

    if (node == 0 || buf == 0) {
        return RAMFS_ERR_INVAL;
    }
    if (node->type != RAMFS_FILE) {
        return RAMFS_ERR_ISDIR;
    }
    if (len == 0u) {
        return RAMFS_OK;
    }
    old = node->size;
    nd = (uint8_t *)kmalloc(old + len);
    if (nd == 0) {
        return RAMFS_ERR_NOMEM;
    }
    for (i = 0u; i < old; ++i) {
        nd[i] = node->data[i];
    }
    for (i = 0u; i < len; ++i) {
        nd[old + i] = buf[i];
    }
    if (node->data != 0) {
        kfree(node->data);
    }
    node->data = nd;
    node->size = old + len;
    return RAMFS_OK;
}