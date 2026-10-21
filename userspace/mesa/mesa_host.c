// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define MESA_FRAME_MAGIC 0x4C584D46u
#define MESA_ACLS_INT 0
#define MESA_ACLS_FLT 1
#define MESA_ACLS_DBL 2
#define MESA_ACLS_PTR 3
#define MESA_FIX_BLOB 0
#define MESA_FIX_STROFF 1
#define MESA_CALL_FN 0
#define MESA_CALL_PIX 1
#define MESA_CALL_TRACK_PIX 2
#define MESA_CALL_TRACK_POKE 3
#define MESA_CALL_FNPTR 4
#define MESA_CALL_ARGS 20
#define MESA_PIX_CHUNK (28 * 1024)
#define MESA_PIX_CAP (32 * 1024 * 1024)
#define MESA_TRACKS 8

/* Unsigned on purpose: with a plain int, (2 << 30) overflows to a negative value that
 * ioctl()'s unsigned long parameter sign-extends to 0xffffffff8xxxxxxx, and every _IOR
 * request (HOSTINFO, API) missed the kernel's switch with ENOTTY. */
#define GPU_IOC(dir, type, nr, size) \
    ((unsigned long)(((unsigned)(dir) << 30) | ((unsigned)(type) << 8) | ((unsigned)(nr) << 0) | \
                     ((unsigned)(size) << 16)))
#define GPU_IOW(nr, type) GPU_IOC(1, 'G', nr, sizeof(type))

struct gpu_pix_xfer {
    uint32_t seq;
    uint32_t off;
    uint32_t len;
    uint32_t rsv;
    uint8_t data[MESA_PIX_CHUNK];
};

#define GPU_IOC_PIX_GPU GPU_IOW(0x10, struct gpu_pix_xfer)

struct gpu_pix_ptr {
    uint32_t seq;
    uint32_t off;
    uint32_t len;
    uint32_t rsv;
    uint64_t ptr;
} __attribute__((packed));

#define GPU_IOC_PIX_PTR_GPU GPU_IOW(0x12, struct gpu_pix_ptr)
#define GPU_IOC_ATTACH_GPU GPU_IOC(1, 'G', 0x00, 4)
#define GPU_IOC_API_GPU GPU_IOC(2, 'G', 0x04, 256)

struct mesa_host_info_gpu {
    uint32_t version;
    uint32_t pid;
    uint32_t gl_ok;
    uint32_t vk_ok;
    uint32_t frame_bytes;
    char renderer[160];
    char env[160];
    uint32_t prime_gen;
};
#define GPU_IOC_HOSTINFO_GPU GPU_IOC(2, 'G', 0x11, sizeof(struct mesa_host_info_gpu))

static uint32_t my_prime_gen = 0;
static int gpu_fd = -1;

static int prime_changed(void) {
    struct mesa_host_info_gpu hi;
    memset(&hi, 0, sizeof(hi));
    if (ioctl(gpu_fd, GPU_IOC_HOSTINFO_GPU, &hi) < 0) return 0;
    return hi.prime_gen != my_prime_gen;
}

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

struct gpu_api_info {
    uint32_t gl_commands;
    uint32_t vk_commands;
    uint32_t drm_commands;
    uint32_t batch_bytes;
    uint32_t ring_slots;
    uint32_t protocol;
    char drv_name[48];
    char drv_desc[96];
    int drm_version[3];
};

static uint8_t frame_buf[64 * 1024];
static uint8_t done_buf[64 * 1024];
static uint8_t *pix_stage;
static void *track_tab[MESA_TRACKS];

typedef unsigned long (*fn_t)(void);

static void *h_egl, *h_gl, *h_vk, *h_gbm;
static void *(*p_eglGetProcAddress)(const char *);

static uint8_t *tramp_page;
static size_t tramp_used;

static void die(const char *msg) {
    dprintf(2, "luos-mesa-host: %s\n", msg);
    _exit(1);
}

static void tramp_init(void) {
    tramp_page = mmap(NULL, 256 * 1024, PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp_page == MAP_FAILED) die("no trampoline page");
}

static uint8_t *tramp_emit(uint8_t *p, const void *src, size_t n) {
    memcpy(p, src, n);
    return p + n;
}

static fn_t tramp_build(void *fn, const uint64_t *args, int argc, uint32_t fmask) {
    if (argc > MESA_CALL_ARGS) return NULL;
    uint8_t *start = (uint8_t *)tramp_page;
    uint8_t *p = start;
    static const uint8_t gpr[6] = {7, 6, 2, 1, 8, 9};
    int gp = 0, fp = 0;
    uint64_t stackv[MESA_CALL_ARGS];
    int nstack = 0;

    for (int i = 0; i < argc; i++) {
        uint32_t cls = (fmask >> (2 * i)) & 3;
        if (cls == MESA_ACLS_PTR) cls = MESA_ACLS_INT;
        if (cls == MESA_ACLS_INT) {
            if (gp < 6) {
                uint8_t buf[10];
                uint8_t reg = gpr[gp];
                buf[0] = (uint8_t)(0x48 | (reg >> 3));
                buf[1] = (uint8_t)(0xB8 + (reg & 7));
                uint64_t v = args[i];
                memcpy(buf + 2, &v, 8);
                p = tramp_emit(p, buf, 10);
                gp++;
            } else {
                stackv[nstack++] = args[i];
            }
        } else if (cls == MESA_ACLS_FLT || cls == MESA_ACLS_DBL) {
            if (fp < 8) {
                uint8_t buf[14];
                if (cls == MESA_ACLS_FLT) {
                    uint32_t v32 = (uint32_t)args[i];
                    buf[0] = 0xB8;
                    memcpy(buf + 1, &v32, 4);
                    p = tramp_emit(p, buf, 5);
                } else {
                    uint64_t v = args[i];
                    buf[0] = 0x48;
                    buf[1] = 0xB8;
                    memcpy(buf + 2, &v, 8);
                    p = tramp_emit(p, buf, 10);
                }
                uint8_t movq[5] = {0x66, 0x48, 0x0F, 0x6E};
                movq[4] = (uint8_t)(0xC0 | (fp << 3));
                p = tramp_emit(p, movq, 5);
                fp++;
            }
        }
    }

    int pad = (nstack % 2 == 0) ? 1 : 0;
    if (pad) {
        uint8_t push0[2] = {0x6A, 0x00};
        p = tramp_emit(p, push0, 2);
    }
    for (int j = nstack - 1; j >= 0; j--) {
        uint8_t buf[11] = {0x48, 0xB8};
        memcpy(buf + 2, &stackv[j], 8);
        buf[10] = 0x50;
        p = tramp_emit(p, buf, 11);
    }

    uint8_t mov_rax[10] = {0x48, 0xB8};
    memcpy(mov_rax + 2, &fn, 8);
    p = tramp_emit(p, mov_rax, 10);
    uint8_t call_rax[2] = {0xFF, 0xD0};
    p = tramp_emit(p, call_rax, 2);
    if (nstack + pad) {
        uint8_t add[7] = {0x48, 0x81, 0xC4};
        uint32_t amt = (uint32_t)(8 * (nstack + pad));
        memcpy(add + 3, &amt, 4);
        p = tramp_emit(p, add, 7);
    }
    uint8_t ret[1] = {0xC3};
    p = tramp_emit(p, ret, 1);
    if ((size_t)(p - start) > 256 * 1024) return NULL;
    tramp_used = (size_t)(p - (uint8_t *)tramp_page);
    return (fn_t)start;
}

static void open_libs(void) {
    h_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    h_gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    h_gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
    h_vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (h_egl) p_eglGetProcAddress = dlsym(h_egl, "eglGetProcAddress");
}

static void *resolve(const char *name) {
    void *fn = NULL;
    if (p_eglGetProcAddress && (name[0] == 'g' && name[1] == 'l')) {
        fn = p_eglGetProcAddress(name);
        if (fn) return fn;
    }
    if (p_eglGetProcAddress && strncmp(name, "egl", 3) == 0) {
        fn = p_eglGetProcAddress(name);
        if (fn) return fn;
    }
    if (h_egl) { fn = dlsym(h_egl, name); if (fn) return fn; }
    if (h_gl) { fn = dlsym(h_gl, name); if (fn) return fn; }
    if (h_gbm) { fn = dlsym(h_gbm, name); if (fn) return fn; }
    if (h_vk) { fn = dlsym(h_vk, name); if (fn) return fn; }
    if (!h_vk && strncmp(name, "vk", 2) == 0) {
        h_vk = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (h_vk) fn = dlsym(h_vk, name);
        if (fn) return fn;
    }
    fn = dlsym(RTLD_DEFAULT, name);
    return fn;
}

static int pix_ptr_ok = 1;

static int pix_send(uint32_t seq, uint32_t off, const uint8_t *data, uint32_t len) {
    if (pix_ptr_ok) {
        struct gpu_pix_ptr p = {.seq = seq, .off = off, .len = len, .rsv = 0, .ptr = (uint64_t)(uintptr_t)data};
        if (ioctl(gpu_fd, GPU_IOC_PIX_PTR_GPU, &p) == 0) return 0;
        if (errno != ENOTTY && errno != EINVAL) return -1;
        pix_ptr_ok = 0;
    }
    while (len) {
        uint32_t chunk = len > MESA_PIX_CHUNK ? MESA_PIX_CHUNK : len;
        struct gpu_pix_xfer x;
        x.seq = seq;
        x.off = off;
        x.len = chunk;
        x.rsv = 0;
        memcpy(x.data, data, chunk);
        if (ioctl(gpu_fd, GPU_IOC_PIX_GPU, &x) < 0) return -1;
        data += chunk;
        off += chunk;
        len -= chunk;
    }
    return 0;
}

static int argc_ge(struct mesa_call *c, int n) { return c->argc >= n; }

static int mesa_trace_all = 0;
static int mesa_trace_basic = 0;

int main(void) {
    gpu_fd = open("/dev/gpu", O_RDWR);
    if (gpu_fd < 0) die("cannot open /dev/gpu");
    mesa_trace_all = getenv("MESA_TRACE_ALL") != NULL;
    mesa_trace_basic = getenv("LUOS_MESA_TRACE") != NULL;

    int dev = 0;
    if (ioctl(gpu_fd, GPU_IOC_ATTACH_GPU, &dev) < 0) die("ATTACH failed");
    {
        struct mesa_host_info_gpu hi;
        memset(&hi, 0, sizeof(hi));
        if (ioctl(gpu_fd, GPU_IOC_HOSTINFO_GPU, &hi) == 0) my_prime_gen = hi.prime_gen;
    }

    struct gpu_api_info info;
    memset(&info, 0, sizeof(info));
    ioctl(gpu_fd, GPU_IOC_API_GPU, &info);

    pix_stage = malloc(MESA_PIX_CAP);
    if (!pix_stage) die("no pixel staging");
    {
        const char *vkdf = getenv("VK_DRIVER_FILES");
        dprintf(2, "mesa-env VK_DRIVER_FILES=%s LIBGL_ALWAYS_SOFTWARE=%s\n",
                vkdf ? vkdf : "(unset)",
                getenv("LIBGL_ALWAYS_SOFTWARE") ? getenv("LIBGL_ALWAYS_SOFTWARE") : "?");
    }
    tramp_init();
    open_libs();

    for (;;) {
        struct pollfd pfd = {.fd = gpu_fd, .events = POLLIN};
        int pr = poll(&pfd, 1, 500);
        if (pr == 0) {
            if (prime_changed()) {
                dprintf(2, "mesa: PRIME selection changed - exiting for a restart\n");
                _exit(0);
            }
            continue;
        }
        if (pr < 0) continue;
        ssize_t n = read(gpu_fd, frame_buf, sizeof(frame_buf));
        if (n <= 0) continue;
        if ((size_t)n < sizeof(struct mesa_frame_hdr)) continue;
        struct mesa_frame_hdr *hdr = (struct mesa_frame_hdr *)frame_buf;
        if (hdr->magic != MESA_FRAME_MAGIC) continue;

        uint8_t *blob = frame_buf + hdr->blob_off;
        struct mesa_fixup *fix = (struct mesa_fixup *)(frame_buf + hdr->fix_off);
        struct mesa_out *outs = (struct mesa_out *)(frame_buf + hdr->out_off);
        struct mesa_call *calls = (struct mesa_call *)(frame_buf + sizeof(struct mesa_frame_hdr));

        struct mesa_done_hdr *dh = (struct mesa_done_hdr *)done_buf;
        uint32_t fixbase = 0;
        memset(dh, 0, sizeof(*dh));
        dh->device = hdr->device;
        dh->slot_seq = hdr->slot_seq;
        dh->seq = hdr->seq;
        uint64_t *rets = (uint64_t *)(done_buf + sizeof(*dh));
        uint8_t *outbuf = NULL;
        uint32_t outpos = 0;
        uint32_t retcount = 0;
        uint32_t fail = 0;

        for (int ci = 0; ci < hdr->calls; ci++) {
            struct mesa_call *c = &calls[ci];
            uint64_t argv[MESA_CALL_ARGS];
            for (int i = 0; i < MESA_CALL_ARGS; i++) argv[i] = i < c->argc ? c->args[i] : 0;

            if (c->kind == MESA_CALL_PIX) {
                uint32_t len = (uint32_t)c->args[0];
                if (len > MESA_PIX_CAP) len = MESA_PIX_CAP;
                if (pix_send(hdr->pix_seq, 0, pix_stage, len) == 0) rets[retcount++] = len;
                else {
                    rets[retcount++] = 0;
                    dprintf(2, "mesa: pixel upload rejected (%u bytes)\n", len);
                    fail = 1;
                }
                continue;
            }
            if (c->kind == MESA_CALL_TRACK_POKE) {
                uint32_t slot = (uint32_t)c->args[0];
                uint64_t off = c->args[1];
                uint32_t len = (uint32_t)c->args[2];
                uint8_t *src = blob + (uint32_t)c->args[3];
                if (slot >= MESA_TRACKS || !track_tab[slot]) {
                    rets[retcount++] = (uint64_t)-1;
                    fail = 1;
                    continue;
                }
                memcpy((uint8_t *)track_tab[slot] + off, src, len);
                rets[retcount++] = len;
                continue;
            }
            if (c->kind == MESA_CALL_TRACK_PIX) {
                uint32_t slot = (uint32_t)c->args[0];
                uint64_t off = c->args[1];
                uint32_t len = (uint32_t)c->args[2];
                if (slot >= MESA_TRACKS || !track_tab[slot]) {
                    rets[retcount++] = (uint64_t)-1;
                    fail = 1;
                    continue;
                }
                uint8_t *src = (uint8_t *)track_tab[slot] + off;
                if (pix_send(hdr->pix_seq, 0, src, len) == 0) rets[retcount++] = len;
                else {
                    rets[retcount++] = 0;
                    dprintf(2, "mesa: tracked pixel upload rejected (%u bytes)\n", len);
                    fail = 1;
                }
                continue;
            }

            const char *name = c->kind == MESA_CALL_FNPTR ? "(fnptr)" : (const char *)(blob + c->name_off);
            if (mesa_trace_all)
                dprintf(2, "mesa-pre %s argc=%u fmask=%08x kind=%u plen=%u\n",
                         name, c->argc, c->fmask, c->kind, hdr->pix_len);
            void *fn = c->kind == MESA_CALL_FNPTR ? (void *)1 : resolve(name);
            if (!fn) {
                dprintf(2, "mesa-trace %s -> UNRESOLVED\n", name);
                rets[retcount++] = (uint64_t)-1;
                fail = 1;
                fixbase += c->nfix;
                continue;
            }

            for (int i = 0; i < c->argc; i++) {
                if ((c->rsv >> i) & 1u) {
                    uint32_t src = (uint32_t)argv[i];
                    if (src < retcount) argv[i] = rets[src];
                }
                if (((c->fmask >> (2 * i)) & 3) == MESA_ACLS_PTR) {
                    if (argv[i] == 0xFFFF) argv[i] = (uint64_t)pix_stage;
                    else if (argv[i] == 0xFFFE) argv[i] = 0;
                    else argv[i] = (uint64_t)(blob + argv[i]);
                }
            }

            void *fxheap[16];
            uint32_t fxheapn = 0;
            for (uint16_t f = 0; f < c->nfix; f++) {
                struct mesa_fixup *fx = &fix[fixbase + f];
                if (mesa_trace_all && f < 12)
                    dprintf(2, "mesa-fix[%u] where=%u target=%08x kind=%u\n",
                            f, fx->where, fx->target, fx->target >> 31);
                if (fx->target >> 31) {
                    uint32_t count = fx->target & 0x7FFFFFFFu;
                    if (count > 4096) count = 4096;
                    uint64_t *arr = malloc((size_t)count * 8 + 8);
                    if (!arr) continue;
                    if (fxheapn < 16) fxheap[fxheapn++] = arr;
                    for (uint32_t k = 0; k < count; k++) {
                        uint32_t off32;
                        memcpy(&off32, blob + fx->where + 4 * k, 4);
                        arr[k] = (uint64_t)(blob + fx->where + off32);
                    }
                    for (int ai = 0; ai < c->argc; ai++)
                        if (argv[ai] == (uint64_t)(blob + fx->where))
                            argv[ai] = (uint64_t)arr;
                } else {
                    uint64_t target = (uint64_t)(blob + fx->target);
                    memcpy(blob + fx->where, &target, 8);
                }
            }
            fixbase += c->nfix;

            fn_t call;
            if (c->kind == MESA_CALL_FNPTR) {
                fn = (void *)(uintptr_t)argv[0];
                call = fn ? tramp_build(fn, argv + 1, c->argc - 1, c->fmask >> 2) : NULL;
            } else {
                call = tramp_build(fn, argv, c->argc, c->fmask);
            }
            if (!call) {
                dprintf(2, "mesa-trace %s -> TRAMP-FAIL argc=%u fmask=%08x\n",
                        name, c->argc, c->fmask);
                rets[retcount++] = (uint64_t)-1;
                fail = 1;
                continue;
            }
            if (mesa_trace_all && name[0] == 'v' && name[1] == 'k') {
                dprintf(2, "mesa-vk-args %s", name);
                for (int i = 0; i < c->argc; i++)
                    dprintf(2, " a%d=%llx%s", i, (unsigned long long)argv[i],
                            ((c->fmask >> (2 * i)) & 3) == MESA_ACLS_PTR ? "(p)" : "");
                dprintf(2, "\n");
                if (strstr(name, "Enumerate") && argc_ge(c, 2) && argv[1])
                    dprintf(2, "mesa-vk-count pre *a1=%llu\n",
                            (unsigned long long)*(uint32_t *)(uintptr_t)argv[1]);
                if (strstr(name, "AllocateCommandBuffers") && argv[1])
                    dprintf(2, "mesa-vk-cai st=%llx next=%llx pool=%llx lvl=%u cnt=%u\n",
                            (unsigned long long)*(uint64_t *)(uintptr_t)(argv[1] + 0),
                            (unsigned long long)*(uint64_t *)(uintptr_t)(argv[1] + 8),
                            (unsigned long long)*(uint64_t *)(uintptr_t)(argv[1] + 16),
                            *(uint32_t *)(uintptr_t)(argv[1] + 24),
                            *(uint32_t *)(uintptr_t)(argv[1] + 28));
            }
            unsigned long r = call();
            if (mesa_trace_all && strstr(name, "Enumerate") && argc_ge(c, 2) && argv[1])
                dprintf(2, "mesa-vk-count post *a1=%llu\n",
                        (unsigned long long)*(uint32_t *)(uintptr_t)argv[1]);
            if (c->kind != MESA_CALL_FNPTR && name[0] == 'o' && name[1] == 'p' && name[2] == 'e' && name[3] == 'n' &&
                name[4] == '\0' && (long)r >= 0 && (long)r < 4096)
                ioctl((int)r, 0x641F);
            rets[retcount++] = (uint64_t)r;
            if (mesa_trace_basic)
            dprintf(2, "mesa-trace %s -> 0x%lx (nfix=%u)\n", name, r, c->nfix);
            for (uint32_t hi = 0; hi < fxheapn; hi++) free(fxheap[hi]);

            if (c->rsv & 0x80000000u) {
                uint32_t area = c->rsv & 0x7FFFFFFFu;
                if (r && r != (uint64_t)-1) {
                    strncpy((char *)(blob + area), (const char *)r, 255);
                    blob[area + 255] = 0;
                } else {
                    blob[area] = 0;
                }
            }
        }

        uint16_t outn = 0;
        outbuf = done_buf + sizeof(*dh) + (size_t)retcount * 8;
        size_t outcap = sizeof(done_buf) - (size_t)(outbuf - done_buf);
        for (int ci = 0; ci < hdr->calls && outn < 192; ci++) {
            struct mesa_call *c = &calls[ci];
            for (uint16_t k = 0; k < c->nout && outn < 192; k++) {
                struct mesa_out *o = &outs[outn];
                uint8_t *src = blob + o->off;
                if (mesa_trace_all && outn < 4) {
                    uint64_t v8 = 0;
                    memcpy(&v8, src, o->len < 8 ? o->len : 8);
                    dprintf(2, "mesa-out[%u] off=%u len=%u first8=0x%llx\n",
                            outn, o->off, o->len, (unsigned long long)v8);
                }
                if ((size_t)o->len > outcap - outpos) {
                    dprintf(2, "mesa-trace out overflow (%u + %u > %zu) - result truncated\n",
                            (unsigned)outpos, o->len, outcap);
                    fail = 1;
                    outn++;
                    continue;
                }
                memcpy(outbuf + outpos, src, o->len);
                outpos += o->len;
                if (o->track != 0xFFFF && o->track < MESA_TRACKS && o->len >= 8) {
                    uint64_t ptr;
                    memcpy(&ptr, src, 8);
                    track_tab[o->track] = (void *)ptr;
                }
                outn++;
            }
        }

        dh->rets = retcount;
        dh->bytes = outpos;
        dh->status = fail;
        size_t total = sizeof(*dh) + (size_t)retcount * 8 + outpos;
        if (write(gpu_fd, done_buf, total) < 0) die("write completion failed");
        if (prime_changed()) {
            dprintf(2, "mesa: PRIME selection changed - exiting for a restart\n");
            _exit(0);
        }
    }
}
