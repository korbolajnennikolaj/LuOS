// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#ifndef MESA_H
#define MESA_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/GPU/gpu_core.h"

#define MESA_FRAME_MAGIC 0x4C584D46u
#define MESA_FRAME_MAX (60 * 1024)
#define MESA_CALL_ARGS 20
#define MESA_HOST_BIN "/usr/bin/luos-mesa-host"
#define MESA_PIX_CHUNK (28 * 1024)
#define MESA_PIX_MAX (32 * 1024 * 1024)
#define MESA_PRIME_LLVM 0
#define MESA_PRIME_AUTO (-1)
#define MESA_HOST_BOOT_MS 120000
#define MESA_HOST_ENV_MAX 512
#define MESA_MAX_FRAMES 4
#define MESA_MAX_TRACK 8

#define MESA_ACLS_INT 0
#define MESA_ACLS_FLT 1
#define MESA_ACLS_DBL 2
#define MESA_ACLS_PTR 3

#define MESA_FIX_BLOB 0
#define MESA_FIX_STROFF 1

#define MESA_MAX_CALLS 96

enum mesa_call_kind {
    MESA_CALL_FN = 0,
    MESA_CALL_PIX = 1,
    MESA_CALL_TRACK_PIX = 2,
    MESA_CALL_TRACK_POKE = 3,
    MESA_CALL_FNPTR = 4,
};

#define MESA_DIRECT_SEQ 0xFFFFFFFDu
#define MESA_DC_OUTS_MAX 8192

struct mesa_fixup {
    uint32_t where;
    uint32_t target;
};

struct mesa_out {
    uint32_t off;
    uint32_t len;
    uint16_t track;
    uint16_t rsv;
};

struct mesa_call {
    uint16_t kind;
    uint16_t argc;
    uint16_t nfix;
    uint16_t nout;
    uint32_t name_off;
    uint32_t fmask;
    uint32_t rsv;
    uint64_t args[MESA_CALL_ARGS];
} __attribute__((packed));

struct mesa_frame_hdr {
    uint32_t magic;
    uint32_t seq;
    uint16_t calls;
    uint16_t rsv0;
    uint16_t fix_off;
    uint16_t out_off;
    uint32_t blob_off;
    uint32_t frame_len;
    uint32_t slot_seq;
    uint32_t device;
    uint32_t api;
    uint32_t first_cmd;
    uint32_t cmd_count;
    uint32_t pix_seq;
    uint32_t pix_len;
} __attribute__((packed));

struct mesa_done_hdr {
    uint32_t device;
    uint32_t slot_seq;
    uint32_t seq;
    uint32_t status;
    uint32_t rets;
    uint32_t bytes;
} __attribute__((packed));

struct mesa_host_info {
    uint32_t version;
    uint32_t pid;
    uint32_t gl_ok;
    uint32_t vk_ok;
    uint32_t frame_bytes;
    char renderer[160];
    char env[160];
    uint32_t prime_gen;
};

#define GPU_IOC_PIX GPU_IOW(0x10, struct gpu_pix_xfer)
#define GPU_IOC_HOSTINFO GPU_IOR(0x11, struct mesa_host_info)

struct gpu_pix_ptr {
    uint32_t seq;
    uint32_t off;
    uint32_t len;
    uint32_t rsv;
    uint64_t ptr;
} __attribute__((packed));

#define GPU_IOC_PIX_PTR GPU_IOW(0x12, struct gpu_pix_ptr)

struct gpu_pix_xfer {
    uint32_t seq;
    uint32_t off;
    uint32_t len;
    uint32_t rsv;
    uint8_t data[MESA_PIX_CHUNK];
} __attribute__((packed));

int mesa_start(void);
bool mesa_host_pid_get(void);
void mesa_host_attached(void);
uint32_t mesa_epoch_get(void);
void mesa_on_flush(int dev_idx, struct gpu_ring_slot *slot);
int mesa_init(void);
int mesa_start_async(void);
bool mesa_ready(void);
bool mesa_failed(void);
int mesa_prime_set(int sel);
int mesa_prime_get(void);
const char *mesa_prime_name(int sel);
int mesa_prime_count(void);
const char *mesa_prime_card(int idx);
void mesa_prime_refresh(void);

bool mesa_frame_pending(int dev_idx);
int mesa_frame_drain(int dev_idx, uint8_t *buf, uint32_t cap, uint32_t *out_len);
int mesa_frame_complete(int dev_idx, uint32_t slot_seq, uint32_t seq, const uint8_t *data, uint32_t len);
int mesa_pix_write(uint32_t pix_seq, uint32_t off, const void *data, uint32_t len);
uint8_t *mesa_pix_target(uint32_t pix_seq, uint32_t off, uint32_t len);
void mesa_pix_account(uint32_t len);
int mesa_host_info(struct mesa_host_info *out);
uint32_t mesa_pix_seq(void);
uint8_t *mesa_pix_data(void);
uint32_t mesa_pix_len(void);
bool mesa_boot_ok(void);

struct mesa_dc;
struct mesa_dc *mesa_dc_new(void);
void mesa_dc_free(struct mesa_dc *d);
int mesa_dc_call(struct mesa_dc *d, const char *name, uint8_t argc, const uint64_t *args, uint32_t ptr_mask,
                 uint32_t ret_mask);
int mesa_dc_fnptr(struct mesa_dc *d, uint8_t argc, const uint64_t *args, uint32_t ptr_mask, uint32_t ret_mask);
uint32_t mesa_dc_in(struct mesa_dc *d, const void *data, uint32_t len);
uint32_t mesa_dc_out(struct mesa_dc *d, uint32_t len);
void mesa_dc_fix(struct mesa_dc *d, uint32_t where, uint32_t target);
int mesa_dc_run(struct mesa_dc *d, uint64_t *rets, int max_rets, void *outs, uint32_t outs_cap,
                uint32_t timeout_ms);

struct mesa_card_ctx {
    int dev_idx;
    int drm_fd;
    uint64_t gbm;
    uint64_t dpy;
    uint64_t ctx;
};
bool mesa_card_context(struct mesa_card_ctx *out);
void *mesa_host_drm_file(int fd);
const char *mesa_renderer(void);

#endif
