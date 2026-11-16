/* cmd_pci.c - M30/M31 的 pci 和 ahci 两个命令：前者枚举配置空间，后者看控制器和端口状态 */

#include <stdint.h>

#include "../drivers/ahci.h"
#include "../drivers/nvme.h"
#include "../drivers/xhci.h"
#include "../drivers/pci.h"
#include "shell.h"

static void put_hex(uint32_t v, uint32_t digits, enum sh_level lv)
{
    char b[12];
    uint32_t i;
    const char *hex = "0123456789abcdef";

    if (digits > 8u) {
        digits = 8u;
    }
    b[0] = '0';
    b[1] = 'x';
    for (i = 0u; i < digits; ++i) {
        b[2u + i] = hex[(v >> (4u * (digits - 1u - i))) & 0x0Fu];
    }
    b[2u + digits] = '\0';
    sh_print(lv, b);
}

int cmd_pci(int argc, char **argv)
{
    pci_dev_t list[24];
    uint32_t n, i;
    (void)argc;
    (void)argv;

    n = pci_scan(list, 24u);
    sh_print(SH_OK, "pci: ");
    sh_print_u32(SH_OK, n);
    sh_print(SH_OK, " device(s)");
    sh_newline();

    for (i = 0u; i < n; ++i) {
        sh_print(SH_PLAIN, "  ");
        sh_print_u32(SH_PLAIN, list[i].bus);
        sh_print(SH_PLAIN, ":");
        sh_print_u32(SH_PLAIN, list[i].slot);
        sh_print(SH_PLAIN, ".");
        sh_print_u32(SH_PLAIN, list[i].func);
        sh_print(SH_PLAIN, "  ");
        put_hex(list[i].vendor, 4u, SH_PLAIN);
        sh_print(SH_PLAIN, ":");
        put_hex(list[i].device, 4u, SH_PLAIN);
        sh_print(SH_PLAIN, "  class ");
        put_hex(list[i].class_code, 2u, SH_PLAIN);
        sh_print(SH_PLAIN, "/");
        put_hex(list[i].subclass, 2u, SH_PLAIN);
        if (list[i].class_code == PCI_CLASS_STORAGE
            && list[i].subclass == PCI_SUBCLASS_SATA) {
            sh_print(SH_OK, "  [AHCI]");
        } else if (list[i].class_code == PCI_CLASS_STORAGE
                   && list[i].subclass == PCI_SUBCLASS_NVME) {
            sh_print(SH_OK, "  [NVME]");
        } else if (list[i].class_code == PCI_CLASS_STORAGE
                   && list[i].subclass == PCI_SUBCLASS_IDE) {
            sh_print(SH_INFO, "  [IDE]");
        }
        sh_newline();
    }
    return 0;
}

int cmd_usb(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    if (xhci_max_ports() == 0u && !usb_kbd_ready()) {
        err("usb: no xHCI controller");
        return 0;
    }
    sh_print(SH_OK, "usb: BAR0 ");
    put_hex(xhci_bar_low(), 8u, SH_OK);
    sh_print(SH_OK, "  ports ");
    sh_print_u32(SH_OK, xhci_max_ports());
    sh_print(SH_OK, "  slots ");
    sh_print_u32(SH_OK, xhci_max_slots());
    sh_newline();

    if (!usb_kbd_ready()) {
        err("usb: no HID keyboard");
        sh_print(SH_PLAIN, "  last error code ");
        sh_print_u32(SH_PLAIN, (uint32_t)(-xhci_last_error()));
        sh_newline();
        return 0;
    }

    sh_print(SH_OK, "  port speed ");
    sh_print_u32(SH_OK, xhci_port_speed());
    sh_print(SH_OK, "  slot ");
    sh_print_u32(SH_OK, xhci_slot_id());
    sh_print(SH_OK, "  HID ep ");
    put_hex(xhci_hid_ep_addr(), 2u, SH_OK);
    sh_newline();

    sh_print(SH_PLAIN, "  reports ");
    sh_print_u32(SH_PLAIN, xhci_reports());
    sh_print(SH_PLAIN, "  last key usage ");
    put_hex(xhci_last_key(), 2u, SH_PLAIN);
    sh_newline();
    {
        uint32_t enq = 0u, out = 0u, deq = 0u;

        xhci_intr_state(&enq, &out, &deq);
        sh_print(SH_PLAIN, "  ring enq ");
        sh_print_u32(SH_PLAIN, enq);
        sh_print(SH_PLAIN, "  pending ");
        sh_print_u32(SH_PLAIN, out);
        sh_print(SH_PLAIN, "  evt deq ");
        sh_print_u32(SH_PLAIN, deq);
        sh_newline();
    }
    return 0;
}

int cmd_nvme(int argc, char **argv)
{
    uint8_t sec[512];
    uint32_t i;
    uint32_t magic;
    (void)argc;
    (void)argv;

    if (!nvme_ready()) {
        err("nvme: no usable NVMe namespace");
        sh_print(SH_PLAIN, "  BAR0 ");
        put_hex(nvme_bar_low(), 8u, SH_PLAIN);
        sh_newline();
        return 0;
    }

    sh_print(SH_OK, "nvme: BAR0 ");
    put_hex(nvme_bar_low(), 8u, SH_OK);
    sh_print(SH_OK, "  VS ");
    put_hex(nvme_version(), 8u, SH_OK);
    sh_newline();

    sh_print(SH_OK, "  model ");
    sh_print(SH_OK, nvme_model());
    sh_newline();

    sh_print(SH_OK, "  nsid ");
    sh_print_u32(SH_OK, nvme_namespace());
    sh_print(SH_OK, "  block ");
    sh_print_u32(SH_OK, nvme_block_size());
    sh_print(SH_OK, "  sectors(512) ");
    sh_print_u32(SH_OK, nvme_sector_count());
    sh_newline();

    /* 读 LBA2 ， ext2 超级块所在扇区；magic（偏移 56）应为 0xEF53 */
    if (nvme_read_sector(2u, sec) != 0) {
        err("nvme: sector 2 read failed");
        return 0;
    }
    magic = (uint32_t)sec[56] | ((uint32_t)sec[57] << 8);
    sh_print(SH_PLAIN, "  lba2 head = ");
    for (i = 0u; i < 4u; ++i) {
        put_hex(sec[i], 2u, SH_PLAIN);
        sh_print(SH_PLAIN, " ");
    }
    sh_print(SH_PLAIN, " magic ");
    put_hex(magic, 4u, SH_PLAIN);
    if (magic == 0xEF53u) {
        sh_print(SH_OK, "  (ext2 OK)");
    } else {
        sh_print(SH_WARN, "  (not ext2)");
    }
    sh_newline();
    return 0;
}

int cmd_ahci(int argc, char **argv)
{
    uint8_t sec[512];
    uint32_t i;
    uint32_t magic;
    (void)argc;
    (void)argv;

    if (!ahci_ready()) {
        err("ahci: no usable SATA port");
        sh_print(SH_PLAIN, "  controller found: ");
        sh_print_u32(SH_PLAIN, ahci_map_status());
        sh_print(SH_PLAIN, "  ABAR ");
        put_hex(ahci_abar(), 8u, SH_PLAIN);
        sh_print(SH_PLAIN, "  CAP ");
        put_hex(ahci_cap(), 8u, SH_PLAIN);
        sh_print(SH_PLAIN, "  PI ");
        put_hex(ahci_ports_implemented(), 8u, SH_PLAIN);
        sh_newline();
        for (i = 0u; i < 32u; ++i) {
            if ((ahci_ports_implemented() & (1u << i)) == 0u) {
                continue;
            }
            sh_print(SH_PLAIN, "  port ");
            sh_print_u32(SH_PLAIN, i);
            sh_print(SH_PLAIN, "  SIG ");
            put_hex(ahci_port_sig_of(i), 8u, SH_PLAIN);
            sh_print(SH_PLAIN, "  SSTS ");
            put_hex(ahci_port_ssts_of(i), 8u, SH_PLAIN);
            sh_print(SH_PLAIN, "  CMD ");
            put_hex(ahci_port_cmd_of(i), 8u, SH_PLAIN);
            sh_newline();
        }
        return 0;
    }

    sh_print(SH_OK, "ahci: ABAR ");
    put_hex(ahci_abar(), 8u, SH_OK);
    sh_print(SH_OK, "  CAP ");
    put_hex(ahci_cap(), 8u, SH_OK);
    sh_print(SH_OK, "  PI ");
    put_hex(ahci_ports_implemented(), 8u, SH_OK);
    sh_newline();

    sh_print(SH_OK, "  active port ");
    sh_print_u32(SH_OK, ahci_port_number());
    sh_print(SH_OK, "  signature ");
    put_hex(ahci_port_signature(), 8u, SH_OK);
    sh_newline();

    /* 读 LBA2 ， ext2 超级块所在扇区；magic（偏移 56）应为 0xEF53 */
    if (ahci_read_sector(2u, sec) != 0) {
        err("ahci: sector 2 read failed");
        return 0;
    }
    magic = (uint32_t)sec[56] | ((uint32_t)sec[57] << 8);
    sh_print(SH_PLAIN, "  lba2 head = ");
    for (i = 0u; i < 4u; ++i) {
        put_hex(sec[i], 2u, SH_PLAIN);
        sh_print(SH_PLAIN, " ");
    }
    sh_print(SH_PLAIN, " magic ");
    put_hex(magic, 4u, SH_PLAIN);
    if (magic == 0xEF53u) {
        sh_print(SH_OK, "  (ext2 OK)");
    } else {
        sh_print(SH_WARN, "  (not ext2)");
    }
    sh_newline();
    return 0;
}
