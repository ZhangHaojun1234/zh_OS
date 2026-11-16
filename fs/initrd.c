/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    fs/initrd.c
 * Version: 0.2beta
 * Brief:   M8 initrd：把嵌入的 ELF 程序写成 ramfs 里的文件（/bin 下的 .elf）。
 * ============================================================================ */

#include "initrd.h"

#include "ramfs.h"
#include "../shell/shell.h"

/*
 * build.sh 用 `ld -r -b binary xxx.elf` 把每个用户程序原样嵌进内核，
 * 链接器生成的三符号就是下面这些名字。
 */
extern const uint8_t _binary_hello_elf_start[];
extern const uint8_t _binary_hello_elf_end[];
extern const uint8_t _binary_forkdemo_elf_start[];
extern const uint8_t _binary_forkdemo_elf_end[];
extern const uint8_t _binary_execdemo_elf_start[];
extern const uint8_t _binary_execdemo_elf_end[];
extern const uint8_t _binary_fddemo_elf_start[];   /* [ADD] M9 */
extern const uint8_t _binary_fddemo_elf_end[];
extern const uint8_t _binary_wrdemo_elf_start[];    /* [ADD] M10 */
extern const uint8_t _binary_wrdemo_elf_end[];
extern const uint8_t _binary_spawndemo_elf_start[]; /* [ADD] M10 */
extern const uint8_t _binary_spawndemo_elf_end[];
extern const uint8_t _binary_argvdemo_elf_start[];  /* [ADD] M11 */
extern const uint8_t _binary_argvdemo_elf_end[];
extern const uint8_t _binary_reexec_elf_start[];   /* [ADD] M12 */
extern const uint8_t _binary_reexec_elf_end[];
extern const uint8_t _binary_zhsh3_elf_start[];    /* [ADD] M13 */
extern const uint8_t _binary_zhsh3_elf_end[];
extern const uint8_t _binary_ls_elf_start[];       /* [ADD] M14 */
extern const uint8_t _binary_ls_elf_end[];
extern const uint8_t _binary_cat_elf_start[];
extern const uint8_t _binary_cat_elf_end[];
extern const uint8_t _binary_cowdemo_elf_start[];   /* [ADD] M15 */
extern const uint8_t _binary_cowdemo_elf_end[];
extern const uint8_t _binary_demanddemo_elf_start[]; /* [ADD] M16 */
extern const uint8_t _binary_demanddemo_elf_end[];
extern const uint8_t _binary_spin_elf_start[];      /* [ADD] M17 */
extern const uint8_t _binary_spin_elf_end[];
extern const uint8_t _binary_sigdemo_elf_start[];   /* [ADD] M17 */
extern const uint8_t _binary_sigdemo_elf_end[];
extern const uint8_t _binary_sigcatch_elf_start[];  /* [ADD] M18 */
extern const uint8_t _binary_sigcatch_elf_end[];
extern const uint8_t _binary_jobdemo_elf_start[];   /* [ADD] M18 */
extern const uint8_t _binary_jobdemo_elf_end[];

static void install(const char *path, const uint8_t *start, const uint8_t *end)
{
    const uint64_t len = (uint64_t)(uintptr_t)(end - start);
    const int rc = ramfs_write_file(path, start, len);

    if (rc != RAMFS_OK) {
        sh_print(SH_ERR, "[initrd] ");
        sh_print(SH_ERR, path);
        sh_print(SH_ERR, ": ");
        sh_println(SH_ERR, ramfs_strerror(rc));
        return;
    }

    sh_print(SH_PLAIN, "[initrd] ");
    sh_print(SH_PLAIN, path);
    sh_print(SH_PLAIN, "  ");
    sh_print_u32(SH_PLAIN, (uint32_t)len);
    sh_println(SH_PLAIN, " bytes (ELF64, ready for exec)");
}

void initrd_init(void)
{
    int rc = ramfs_mkdir("/bin");

    if (rc != RAMFS_OK && ramfs_lookup("/bin") == (ramfs_node_t *)0) {
        err("[initrd] cannot create /bin");
        return;
    }

    install("/bin/hello.elf", _binary_hello_elf_start, _binary_hello_elf_end);
    install("/bin/forkdemo.elf", _binary_forkdemo_elf_start,
            _binary_forkdemo_elf_end);
    install("/bin/execdemo.elf", _binary_execdemo_elf_start,
            _binary_execdemo_elf_end);
    install("/bin/fddemo.elf", _binary_fddemo_elf_start, _binary_fddemo_elf_end);
    install("/bin/wrdemo.elf", _binary_wrdemo_elf_start, _binary_wrdemo_elf_end);
    install("/bin/spawndemo.elf", _binary_spawndemo_elf_start,
            _binary_spawndemo_elf_end);
    install("/bin/argvdemo.elf", _binary_argvdemo_elf_start,
            _binary_argvdemo_elf_end);
    install("/bin/reexec.elf", _binary_reexec_elf_start,
            _binary_reexec_elf_end);
    install("/bin/zhsh3.elf", _binary_zhsh3_elf_start,
            _binary_zhsh3_elf_end);
    install("/bin/ls.elf", _binary_ls_elf_start, _binary_ls_elf_end);
    install("/bin/cat.elf", _binary_cat_elf_start, _binary_cat_elf_end);
    install("/bin/cowdemo.elf", _binary_cowdemo_elf_start, _binary_cowdemo_elf_end);
    install("/bin/demanddemo.elf", _binary_demanddemo_elf_start, _binary_demanddemo_elf_end);
    install("/bin/spin.elf", _binary_spin_elf_start, _binary_spin_elf_end);          /* [ADD] M17 */
    install("/bin/sigdemo.elf", _binary_sigdemo_elf_start, _binary_sigdemo_elf_end); /* [ADD] M17 */
    install("/bin/sigcatch.elf", _binary_sigcatch_elf_start, _binary_sigcatch_elf_end); /* [ADD] M18 */
    install("/bin/jobdemo.elf", _binary_jobdemo_elf_start, _binary_jobdemo_elf_end);   /* [ADD] M18 */
}
