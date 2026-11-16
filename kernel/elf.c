/* elf.c - ELF64 装载器：校验头、按 PT_LOAD 逐段映射拷贝、拿 e_entry */

#include "elf.h"

#include "paging.h"
#include "vmm.h"   /* [ADD] M16：vmm_space_want() / VMM_WANT_WRITE */
#include "../mm/phys.h"
#include "../shell/shell.h"

/*
 * ELF64 的两个结构，按 System V ABI 的磁盘布局原样定义（不要压实、不要加 packed：
 * 这个布局天然是对齐的，sizeof 应当正好是 64 / 56 字节，下面有编译期断言）。
 */
typedef struct {
    uint8_t  e_ident[16];   /* 0x7f 'E' 'L' 'F' / class / data / version */
    uint16_t e_type;        /* 2 = ET_EXEC */
    uint16_t e_machine;     /* 0x3E = EM_X86_64 */
    uint32_t e_version;
    uint64_t e_entry;       /* 入口虚拟地址 */
    uint64_t e_phoff;       /* 程序头表在文件里的偏移 */
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;   /* 必须是 56 */
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;        /* 1 = PT_LOAD */
    uint32_t p_flags;       /* bit2 = R, bit1 = W, bit0 = X */
    uint64_t p_offset;      /* 文件内偏移 */
    uint64_t p_vaddr;       /* 目标虚拟地址（本内核不用 p_paddr） */
    uint64_t p_paddr;
    uint64_t p_filesz;      /* 文件里实际有多少字节要拷 */
    uint64_t p_memsz;       /* 内存里占多少（> filesz 的部分是 .bss，要清零） */
    uint64_t p_align;
} elf64_phdr_t;

/* 布局不符就编译失败 ， 这类错在运行时表现为"读出来的字段全是垃圾"，极难查。 */
typedef char elf_ehdr_size_check[(sizeof(elf64_ehdr_t) == 64u) ? 1 : -1];
typedef char elf_phdr_size_check[(sizeof(elf64_phdr_t) == 56u) ? 1 : -1];

#define PT_LOAD     1u
#define ET_EXEC     2u
#define EM_X86_64   0x3Eu
#define ELFCLASS64  2u
#define ELFDATA2LSB 1u

#define PAGE_MASK_4K 0xFFFULL

static const char *g_elf_err;

const char *elf_last_error(void)
{
    return (g_elf_err != (const char *)0) ? g_elf_err : "ok";
}

static void fail(const char *why)
{
    g_elf_err = why;
}

/* freestanding：不能调 memcpy/memset，自己来（-nostdlib，编译器也可能不内联）。 */
static void copy_bytes(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;

    for (i = 0u; i < n; ++i) {
        dst[i] = src[i];
    }
}

static void zero_bytes(uint8_t *dst, uint64_t n)
{
    uint64_t i;

    for (i = 0u; i < n; ++i) {
        dst[i] = 0u;
    }
}

/* 紧凑十六进制：0x40000000 而不是 0x0000000040000000 ， VGA 只有 80 列，
 * 满宽 18 个字符会把后面的字段挤到折行，反而看不清。 */
static void show_hex(uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    char rev[16];
    char buf[19];
    uint32_t n = 0u;
    uint32_t i;

    if (value == 0u) {
        sh_print(SH_PLAIN, "0x0");
        return;
    }
    while (value != 0u && n < 16u) {
        rev[n] = digits[value & 0xFu];
        value >>= 4;
        ++n;
    }
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0u; i < n; ++i) {
        buf[2u + i] = rev[n - 1u - i];
    }
    buf[2u + n] = '\0';
    sh_print(SH_PLAIN, buf);
}

void elf_report(const uint8_t *blob, uint64_t size)
{
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)(const void *)blob;
    uint32_t i;

    if (size < sizeof(elf64_ehdr_t)) {
        sh_println(SH_ERR, "  elf: blob too small to even hold an ELF header");
        return;
    }

    sh_print(SH_PLAIN, "  magic     : ");
    sh_print(SH_PLAIN, "7f 45 4c 46 (\\x7fELF)   class=");
    sh_print_u32(SH_PLAIN, (uint32_t)eh->e_ident[4]);
    sh_println(SH_PLAIN, " (2=ELF64)");
    sh_print(SH_PLAIN, "  data      : ");
    sh_print_u32(SH_PLAIN, (uint32_t)eh->e_ident[5]);
    sh_print(SH_PLAIN, " (1=little endian)    version=");
    sh_print_u32(SH_PLAIN, (uint32_t)eh->e_version);
    sh_newline();
    sh_print(SH_PLAIN, "  type      : ");
    sh_print_u32(SH_PLAIN, (uint32_t)eh->e_type);
    sh_print(SH_PLAIN, " (2=ET_EXEC)  machine=");
    sh_print_u32(SH_PLAIN, (uint32_t)eh->e_machine);
    sh_println(SH_PLAIN, " (0x3e=EM_X86_64)");
    sh_print(SH_PLAIN, "  entry     : ");
    show_hex(eh->e_entry);
    sh_newline();
    sh_print(SH_PLAIN, "  phoff     : ");
    show_hex(eh->e_phoff);
    sh_print(SH_PLAIN, "   phnum=");
    sh_print_u32(SH_PLAIN, (uint32_t)eh->e_phnum);
    sh_print(SH_PLAIN, "   phentsize=");
    sh_print_u32(SH_PLAIN, (uint32_t)eh->e_phentsize);
    sh_newline();

    for (i = 0u; i < (uint32_t)eh->e_phnum && i < ELF_MAX_PHDRS; ++i) {
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)(const void *)(blob + eh->e_phoff
                                                 + (uint64_t)i * sizeof(elf64_phdr_t));

        sh_print(SH_PLAIN, "  phdr ");
        sh_print_u32(SH_PLAIN, i);
        sh_print(SH_PLAIN, "     : type=");
        if (ph->p_type != PT_LOAD) {
            show_hex(ph->p_type);
            sh_print(SH_PLAIN, (ph->p_type == 0x6474E551u)
                                   ? " PT_GNU_STACK (ignored)"
                                   : " (not PT_LOAD, ignored)");
            sh_newline();
            continue;
        }
        sh_print(SH_PLAIN, "1 PT_LOAD  flags=");
        sh_print_u32(SH_PLAIN, ph->p_flags & 7u);
        sh_print(SH_PLAIN, " (");
        sh_print(SH_PLAIN, ((ph->p_flags & 4u) != 0u) ? "R" : "-");
        sh_print(SH_PLAIN, ((ph->p_flags & 2u) != 0u) ? "W" : "-");
        sh_print(SH_PLAIN, ((ph->p_flags & 1u) != 0u) ? "X" : "-");
        sh_println(SH_PLAIN, ")");
        sh_print(SH_PLAIN, "            vaddr=");
        show_hex(ph->p_vaddr);
        sh_print(SH_PLAIN, "  filesz=");
        show_hex(ph->p_filesz);
        sh_print(SH_PLAIN, "  memsz=");
        show_hex(ph->p_memsz);
        sh_newline();
    }
}

int elf_load_into(vmm_space_t *space, const uint8_t *blob, uint64_t size,
                  elf_image_t *img)
{
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)(const void *)blob;
    /* 512 字节，标记"这一次装载里哪些页已经被清过"，避免同一页被两个段重复清。 */
    uint8_t touched[VMM_ENTRIES];
    uint32_t i;

    if (space == (vmm_space_t *)0 || blob == (const uint8_t *)0 || img == (elf_image_t *)0) {
        fail("bad argument");
        return -1;
    }
    if (size < sizeof(elf64_ehdr_t)) {
        fail("file shorter than an ELF header");
        return -1;
    }
    if (eh->e_ident[0] != 0x7Fu || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') {
        fail("not an ELF file (bad magic)");
        return -1;
    }
    if (eh->e_ident[4] != ELFCLASS64) {
        fail("not ELFCLASS64 (32-bit or corrupt)");
        return -1;
    }
    if (eh->e_ident[5] != ELFDATA2LSB) {
        fail("not little endian");
        return -1;
    }
    if (eh->e_type != ET_EXEC) {
        fail("not ET_EXEC (no PIE/dynamic support yet)");
        return -1;
    }
    if (eh->e_machine != EM_X86_64) {
        fail("not EM_X86_64");
        return -1;
    }
    if (eh->e_phentsize != sizeof(elf64_phdr_t)) {
        fail("unexpected e_phentsize");
        return -1;
    }
    if (eh->e_phnum == 0u || eh->e_phnum > ELF_MAX_PHDRS) {
        fail("e_phnum out of range");
        return -1;
    }
    if (eh->e_phoff + (uint64_t)eh->e_phnum * sizeof(elf64_phdr_t) > size) {
        fail("program header table outside the file");
        return -1;
    }
    if (eh->e_entry < VMM_USER_BASE ||
        eh->e_entry >= VMM_USER_BASE + VMM_USER_SPAN) {
        fail("entry point outside the user window");
        return -1;
    }

    zero_bytes((uint8_t *)(void *)touched, (uint64_t)sizeof(touched));

    img->entry = eh->e_entry;
    img->segments = 0u;
    img->pages = 0u;
    img->file_bytes = 0u;
    img->mem_bytes = 0u;
    img->has_bss = 0u;
    img->demand_pages = 0u;   /* [M16] */

    for (i = 0u; i < (uint32_t)eh->e_phnum; ++i) {
        const elf64_phdr_t *ph =
            (const elf64_phdr_t *)(const void *)(blob + eh->e_phoff
                                                 + (uint64_t)i * sizeof(elf64_phdr_t));
        uint64_t seg_start;
        uint64_t seg_end;
        uint64_t va;

        if (ph->p_type != PT_LOAD || ph->p_memsz == 0u) {
            continue;   /* PT_GNU_STACK / PT_NOTE 之类，本版不处理 */
        }
        if (ph->p_offset + ph->p_filesz > size) {
            fail("PT_LOAD extends past end of file");
            return -1;
        }
        if (ph->p_vaddr < VMM_USER_BASE ||
            ph->p_vaddr + ph->p_memsz > VMM_USER_BASE + VMM_USER_SPAN) {
            fail("PT_LOAD outside the user window");
            return -1;
        }

        /* 按页覆盖 [vaddr, vaddr+memsz)，第一页和最后一页都要向下/向上取整。 */
        seg_start = ph->p_vaddr & ~PAGE_MASK_4K;
        seg_end = (ph->p_vaddr + ph->p_memsz + PAGE_MASK_4K) & ~PAGE_MASK_4K;

        for (va = seg_start; va < seg_end; va += VMM_PAGE_SIZE) {
            const uint32_t index = (uint32_t)((va - VMM_USER_BASE) >> 12);
            uint64_t pa = vmm_probe(space, va);
            uint8_t *page;
            uint64_t copy_from;
            uint64_t copy_to;
            uint64_t zero_from;
            uint64_t zero_to;
            uint64_t have_from;
            uint64_t have_to;

            /* [ADD] M16：这一页有没有来自文件的内容？
             * have_from/have_to 是"本页 ∩ [p_vaddr, p_vaddr+p_filesz)" 的区间，
             * 空区间（have_to <= have_from）就是纯 .bss 页 ， 它的内容只有"零"。 */
            have_from = (va > ph->p_vaddr) ? va : ph->p_vaddr;
            have_to = va + VMM_PAGE_SIZE;
            if (have_to > ph->p_vaddr + ph->p_filesz) {
                have_to = ph->p_vaddr + ph->p_filesz;
            }

            /*
             * [M16] 纯 .bss 页走"按需"这条路：不分配帧、不清零，只在
             * desire[] 里记一笔"这一段将来要可写"。用户第一次读写它时才会
             * #PF，vmm_page_fault() 那一刻才给一帧（分配器返回的帧本身就是零）。
             * 这就是为什么装载 64KB 的 .bss 程序不用真的付 64KB 物理内存。
             */
            if (pa == 0u && touched[index] == 0u && have_to <= have_from) {
                if (vmm_space_want(space, va, VMM_PAGE_SIZE, VMM_WANT_WRITE) == 0) {
                    /* 注意：不加 img->pages ， 这一页并没有被映射，
                     * 它只是"将来会存在"。mapped 与 on-demand 是两个账本。 */
                    if (img->demand_pages == 0u) {
                        img->has_bss = 1u;
                    }
                    ++img->demand_pages;
                    continue;   /* 这一页到此为止：不分配、不拷贝 */
                }
            }

            if (pa == 0u) {
                /* 首次访问这一页：向页帧分配器要一帧并映射（这就是"按页装入"）。 */
                pa = (uint64_t)(uintptr_t)phys_alloc_frame();
                if (pa == 0u) {
                    fail("out of physical frames while loading");
                    return -1;
                }
                if (vmm_map_4k(space, va, pa, 1) != 0) {
                    fail("vmm_map_4k failed (address outside the window?)");
                    return -1;
                }
            }
            /* 计数按"这段镜像覆盖了几页"算，而不是"这次新申请了几帧"：
             * 重复装载（复用同一个地址空间）时，页早就映射好了，但镜像依然覆盖它。 */
            ++img->pages;

            page = (uint8_t *)(uintptr_t)pa;   /* 恒等映射：物理地址可直接写 */

            /* 同一页可能被相邻段共享：只在本次装载里第一次碰到它时清零，
             * 否则会把前一个段刚拷进去的内容抹掉。 */
            if (touched[index] == 0u) {
                zero_bytes(page, VMM_PAGE_SIZE);
                touched[index] = 1u;
            }

            /* 1) 把文件里的 p_filesz 字节按页切段拷进去 */
            copy_from = (va > ph->p_vaddr) ? va : ph->p_vaddr;
            copy_to = va + VMM_PAGE_SIZE;
            if (copy_to > ph->p_vaddr + ph->p_filesz) {
                copy_to = ph->p_vaddr + ph->p_filesz;
            }
            if (copy_to > copy_from) {
                copy_bytes(page + (copy_from - va),
                           blob + ph->p_offset + (copy_from - ph->p_vaddr),
                           copy_to - copy_from);
            }

            /* 2) p_memsz 超出 p_filesz 的部分（.bss）清零 */
            zero_from = va;
            if (zero_from < ph->p_vaddr + ph->p_filesz) {
                zero_from = ph->p_vaddr + ph->p_filesz;
            }
            zero_to = va + VMM_PAGE_SIZE;
            if (zero_to > ph->p_vaddr + ph->p_memsz) {
                zero_to = ph->p_vaddr + ph->p_memsz;
            }
            if (zero_to > zero_from) {
                zero_bytes(page + (zero_from - va), zero_to - zero_from);
            }
        }

        ++img->segments;
        img->file_bytes += (uint32_t)ph->p_filesz;
        img->mem_bytes += (uint32_t)ph->p_memsz;
        if (ph->p_memsz > ph->p_filesz) {
            img->has_bss = 1u;
        }
    }

    if (img->segments == 0u) {
        fail("no PT_LOAD segment");
        return -1;
    }

    g_elf_err = "ok";
    return 0;
}
