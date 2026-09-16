#ifndef SERVICE_USB_HOTPLUG_H
#define SERVICE_USB_HOTPLUG_H

#include "programs.h"

#include <stdbool.h>
#include <stdint.h>

int usb_hotplug_device_count(void);

uint64_t usb_hotplug_connect_count(void);
uint64_t usb_hotplug_disconnect_count(void);
uint64_t usb_hotplug_poll_count(void);
uint64_t usb_hotplug_rescan_count(void);

bool usb_hotplug_busy(void);
void usb_hotplug_request_rescan(void);

service_t *get_usb_hotplug_service(void);

#endif
