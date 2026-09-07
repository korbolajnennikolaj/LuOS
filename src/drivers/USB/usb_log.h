#ifndef USB_LOG_H
#define USB_LOG_H

#include <stdint.h>

typedef enum usb_log_level {
    USB_LOG_TRACE = 0,
    USB_LOG_INFO = 1,
    USB_LOG_EVENT = 2,

    USB_LOG_WARN = 3,
    USB_LOG_ERROR = 4,
} usb_log_level;

typedef enum usb_log_tag {
    USB_LOG_CORE = 0,
    USB_LOG_HUB,
    USB_LOG_XHCI,
    USB_LOG_EHCI,
    USB_LOG_OHCI,
    USB_LOG_UHCI,
    USB_LOG_TAG_COUNT
} usb_log_tag;

void usb_log_init(void);

usb_log_level usb_log_set_level(usb_log_level lvl);
usb_log_level usb_log_get_level(void);

void usb_log(usb_log_tag tag, usb_log_level lvl, const char *msg);

void usb_log_hex(usb_log_tag tag, usb_log_level lvl, const char *msg, uint32_t val);
void usb_log_hex64(usb_log_tag tag, usb_log_level lvl, const char *msg, uint64_t val);
void usb_log_dec(usb_log_tag tag, usb_log_level lvl, const char *msg, uint32_t val);

void usb_logrow_begin(usb_log_tag tag, usb_log_level lvl);
void usb_logrow_str(const char *s);
void usb_logrow_hex32(uint32_t v);
void usb_logrow_hex64(uint64_t v);
void usb_logrow_dec(int32_t v);

void usb_logrow_pad(uint16_t col);
void usb_logrow_end(void);

void usb_log_port_event(usb_log_tag tag, int ctrl_idx, uint8_t port,
                         const char *event, uint32_t portsc,
                         uint8_t speed_id);

void usb_log_port_table_header(void);

#endif
