/* xhci.h - M33: xHCI 与 HID boot 键盘的接口 */

#ifndef ZH_OS_XHCI_H
#define ZH_OS_XHCI_H

#include <stdint.h>

/* 找控制器 -> 复位端口 -> Address Device -> 枚举配置 -> 配 HID 端点。
 * 成功返回 0；返回负数表示某一步失败（见 xhci_last_error）。 */
int xhci_init(void);

/* 键盘起来了没有 */
int usb_kbd_ready(void);

/* 轮询事件环与中断端点，把按下的键喂给 keyboard 驱动。非阻塞，可频繁调用。 */
void usb_kbd_poll(void);

/* 诊断信息（shell 的 usb 命令用） */
int      xhci_last_error(void);
uint32_t xhci_bar_low(void);
uint32_t xhci_max_ports(void);
uint32_t xhci_max_slots(void);
uint32_t xhci_port_speed(void);
uint32_t xhci_slot_id(void);
uint32_t xhci_hid_ep_addr(void);
uint32_t xhci_reports(void);
uint32_t xhci_last_key(void);
void     xhci_intr_state(uint32_t *enq, uint32_t *out, uint32_t *evt_deq);

#endif /* ZH_OS_XHCI_H */
