// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef KERNEL_LKPI_H
#define KERNEL_LKPI_H


void lkpi_boot(void);

void lkpi_status(void (*emit)(const char *line));

int lkpi_gpu_reinit(void);
int lkpi_gpu_restart(int *unbound, int *bound);

void lkpi_set_drm_debug(unsigned long mask);

void lkpi_console_hold_connector(unsigned int primary_devt, unsigned int connector_id, int hold);

void lkpi_console_restore(unsigned int primary_devt);


struct lkpi_gpu_devinfo {
    char pci_name[16];
    unsigned short vendor;
    unsigned short device;
    unsigned int class_code;
    unsigned char revision;
    int boot_vga;
    char driver[24];
    char card[12];
    char render[16];
    int primary_minor;
    int render_minor;
    unsigned int primary_devt;
    unsigned int render_devt;
    unsigned long vram_bytes;
    int num_crtcs;
    int num_connectors;
    int modeset;
    int render_avail;
};

int lkpi_gpu_enumerate(struct lkpi_gpu_devinfo *out, int max);

int lkpi_gpu_driver_list(const char *names, int rows, int row_len);

int lkpi_gpu_bind_one(const char *pci_name);

int lkpi_gpu_unbind_one(const char *pci_name);

int lkpi_gpu_open_dev(unsigned int devt, unsigned int flags, void **handle);
void lkpi_gpu_close_dev(void *handle);

long lkpi_gpu_ioctl(void *handle, unsigned int cmd, void *karg);

long lkpi_gpu_drm_version(void *handle, int *major, int *minor, int *patch,
                          char *name, int name_cap, char *date, int date_cap,
                          char *desc, int desc_cap);

long lkpi_gpu_gem_vmap(void *handle, unsigned int gem_handle, void **map_token,
                       unsigned long *kaddr, unsigned long long *size);
long lkpi_gpu_gem_vunmap(void *map_token);
void lkpi_file_put(void *lx);
long lkpi_gpu_gem_share(void *dst_handle, void *src_file, unsigned int src_gem, unsigned int *dst_gem);

int lkpi_backlight_count(void);
int lkpi_backlight_get(int idx, char *name, int name_cap, unsigned int *cur, unsigned int *max);
int lkpi_backlight_set(int idx, unsigned int value);
#endif

