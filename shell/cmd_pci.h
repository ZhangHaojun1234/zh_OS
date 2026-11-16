/* cmd_pci.h - pci 和 ahci 命令的声明 */

#ifndef ZH_OS_CMD_PCI_H
#define ZH_OS_CMD_PCI_H

int cmd_pci(int argc, char **argv);
int cmd_ahci(int argc, char **argv);
int cmd_nvme(int argc, char **argv);
int cmd_usb(int argc, char **argv);

#endif /* ZH_OS_CMD_PCI_H */
