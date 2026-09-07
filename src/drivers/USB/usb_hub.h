#ifndef USB_HUB_H
#define USB_HUB_H

#include "drivers/USB/usb_core.h"

#include <stdint.h>

#define USB_HUB_MAX_PORTS 15

#define USB_HUB_FEAT_PORT_CONNECTION 0
#define USB_HUB_FEAT_PORT_ENABLE 1
#define USB_HUB_FEAT_PORT_SUSPEND 2
#define USB_HUB_FEAT_PORT_OVER_CURRENT 3
#define USB_HUB_FEAT_PORT_RESET 4
#define USB_HUB_FEAT_PORT_POWER 8
#define USB_HUB_FEAT_PORT_LOW_SPEED 9
#define USB_HUB_FEAT_C_PORT_CONNECTION 16
#define USB_HUB_FEAT_C_PORT_ENABLE 17
#define USB_HUB_FEAT_C_PORT_SUSPEND 18
#define USB_HUB_FEAT_C_PORT_OVER_CURRENT 19
#define USB_HUB_FEAT_C_PORT_RESET 20

#define USB_HUB_PORTSTS_CONNECTION 0x0001u
#define USB_HUB_PORTSTS_ENABLE 0x0002u
#define USB_HUB_PORTSTS_SUSPEND 0x0004u
#define USB_HUB_PORTSTS_OVER_CURRENT 0x0008u
#define USB_HUB_PORTSTS_RESET 0x0010u
#define USB_HUB_PORTSTS_POWER 0x0100u
#define USB_HUB_PORTSTS_LOW_SPEED 0x0200u
#define USB_HUB_PORTSTS_HIGH_SPEED 0x0400u

void usb_hub_attach(struct usb_device *hub_dev, int hub_slot);

void usb_hub_poll(void);

void usb_hub_reset_state(void);

void usb_hub_detach(int slot);

#endif
