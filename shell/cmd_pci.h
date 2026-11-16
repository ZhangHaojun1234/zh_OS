/* ============================================================================
 * zh_OS 0.2beta (x86_64)
 * File:    shell/cmd_pci.h
 * Version: 0.2beta
 * Brief:   [ADD] M30/M31：pci 与 ahci 两个命令的声明。
 * ============================================================================ */

#ifndef ZH_OS_CMD_PCI_H
#define ZH_OS_CMD_PCI_H

int cmd_pci(int argc, char **argv);
int cmd_ahci(int argc, char **argv);
int cmd_nvme(int argc, char **argv);
int cmd_usb(int argc, char **argv);

#endif /* ZH_OS_CMD_PCI_H */
