#ifndef USB_CONTROLLER_H
#define USB_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

struct pci_device;

enum USB_CONTROLLER_TYPE{
    USB_TYPE_UHCI = 0,
    USB_TYPE_OHCI = 1,
    USB_TYPE_EHCI = 2,
    USB_TYPE_XHCI = 3
};

typedef struct usb_controller {
    enum USB_CONTROLLER_TYPE type;
    uint64_t base_addr;
    bool is_io_space;
    struct pci_device* pci;
} usb_controller;

#include "uhci.h"
#include "ohci.h"
#include "ehci.h"
#include "xhci.h"

#endif
