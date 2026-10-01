#ifndef USB_CORE_INTERNAL_H
#define USB_CORE_INTERNAL_H

#include "components/logger.h"
#include "drivers/USB/usb_core.h"

#include <stdbool.h>
#include <stdint.h>

int usb_control_transfer(struct usb_device *dev, uint8_t type, uint8_t req,
                          uint16_t val, uint16_t idx, uint16_t len, void *data);

int usb_interrupt_transfer(struct usb_device *dev, uint8_t endpoint,
                            void *data, uint16_t len, uint8_t direction);

void usb_init_device(void *ctrl_ptr, uint8_t port, bool is_xhci);

void usb_init_device_topo(void *ctrl_ptr, uint8_t port, bool is_xhci,
                           uint8_t root_port, uint8_t hub_depth,
                           uint32_t route_string, uint8_t parent_hub_slot,
                           uint8_t speed_id);

void delay_ms(uint64_t ms);

enum usb_port_log_source {
    USB_PORT_LOG_HUB,
    USB_PORT_LOG_XHCI,
    USB_PORT_LOG_EHCI,
    USB_PORT_LOG_OHCI,
    USB_PORT_LOG_UHCI,
    USB_PORT_LOG_SOURCE_COUNT
};

void usb_log_port_event(enum usb_port_log_source source, enum logger_level_t level,
                        int ctrl_idx, uint8_t port, const char *event,
                        uint32_t portsc, uint8_t speed_id);

#endif
