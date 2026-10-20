// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static const char *connector_type_name(uint32_t type) {
    switch (type) {
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
        default: return "Unknown";
    }
}

static void draw(uint32_t *px, uint32_t w, uint32_t h, uint32_t pitch, uint32_t frame) {
    static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
    for (uint32_t y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)px + (size_t)y * pitch);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c;
            if (y < h * 2 / 3) {
                c = bars[(x * 8 / w) & 7];
            } else {
                uint32_t g = ((x + frame * 4) * 255 / w) & 0xff;
                c = (g << 16) | ((255 - g) << 8) | ((y * 255 / h) & 0xff);
            }
            uint32_t bx = (frame * 7) % (w > 64 ? w - 64 : 1), by = h / 3 - 32;
            if (x >= bx && x < bx + 64 && y >= by && y < by + 64) c = 0x202020;
            row[x] = c;
        }
    }
}

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    const char *path = "/dev/dri/card0";
    uint32_t want_w = 0, want_h = 0;
    int secs = 5;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-m") && i + 1 < argc) sscanf(argv[++i], "%ux%u", &want_w, &want_h);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atoi(argv[++i]);
        else path = argv[i];
    }

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("kmstest: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    drmVersionPtr ver = drmGetVersion(fd);
    if (!ver) {
        printf("kmstest: DRM_IOCTL_VERSION failed: %s\n", strerror(errno));
        return 1;
    }
    printf("kmstest: %s %d.%d.%d (%s)\n", ver->name, ver->version_major, ver->version_minor,
           ver->version_patchlevel, ver->desc);
    drmFreeVersion(ver);

    uint64_t dumb = 0;
    drmGetCap(fd, DRM_CAP_DUMB_BUFFER, &dumb);
    drmModeResPtr res = drmModeGetResources(fd);
    if (!res) {
        printf("kmstest: GETRESOURCES failed: %s\n", strerror(errno));
        return 1;
    }
    printf("kmstest: dumb buffers %s, %d crtc(s), %d encoder(s), %d connector(s), fb %ux%u..%ux%u\n",
           dumb ? "yes" : "no", res->count_crtcs, res->count_encoders, res->count_connectors,
           res->min_width, res->min_height, res->max_width, res->max_height);

    drmModeConnectorPtr conn = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnectorPtr c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        printf("kmstest: connector %u %s-%u: %s, %d mode(s)\n", c->connector_id, connector_type_name(c->connector_type),
               c->connector_type_id, c->connection == DRM_MODE_CONNECTED ? "connected" : "disconnected",
               c->count_modes);
        for (int m = 0; m < c->count_modes && m < 8; m++)
            printf("kmstest:   %s %uHz%s\n", c->modes[m].name, c->modes[m].vrefresh,
                   (c->modes[m].type & DRM_MODE_TYPE_PREFERRED) ? " preferred" : "");
        if (!conn && c->connection == DRM_MODE_CONNECTED && c->count_modes) conn = c;
        else drmModeFreeConnector(c);
    }
    if (!conn) {
        printf("kmstest: no connected connector\n");
        return 1;
    }

    drmModeModeInfo *mode = &conn->modes[0];
    for (int m = 0; m < conn->count_modes; m++) {
        drmModeModeInfo *mi = &conn->modes[m];
        if (want_w && mi->hdisplay == want_w && mi->vdisplay == want_h) {
            mode = mi;
            break;
        }
        if (!want_w && (mi->type & DRM_MODE_TYPE_PREFERRED)) mode = mi;
    }

    uint32_t crtc_id = 0;
    drmModeEncoderPtr enc = conn->encoder_id ? drmModeGetEncoder(fd, conn->encoder_id) : NULL;
    if (enc && enc->crtc_id) crtc_id = enc->crtc_id;
    if (!crtc_id) {
        for (int e = 0; e < conn->count_encoders && !crtc_id; e++) {
            drmModeEncoderPtr en = drmModeGetEncoder(fd, conn->encoders[e]);
            if (!en) continue;
            for (int c = 0; c < res->count_crtcs; c++)
                if (en->possible_crtcs & (1u << c)) {
                    crtc_id = res->crtcs[c];
                    break;
                }
            drmModeFreeEncoder(en);
        }
    }
    if (enc) drmModeFreeEncoder(enc);
    if (!crtc_id) {
        printf("kmstest: no usable CRTC\n");
        return 1;
    }

    struct drm_mode_create_dumb creq = { .width = mode->hdisplay, .height = mode->vdisplay, .bpp = 32 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq)) {
        printf("kmstest: CREATE_DUMB failed: %s\n", strerror(errno));
        return 1;
    }
    uint32_t fb_id = 0;
    if (drmModeAddFB(fd, creq.width, creq.height, 24, 32, creq.pitch, creq.handle, &fb_id)) {
        printf("kmstest: ADDFB failed: %s\n", strerror(errno));
        return 1;
    }
    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq)) {
        printf("kmstest: MAP_DUMB failed: %s\n", strerror(errno));
        return 1;
    }
    uint32_t *px = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)mreq.offset);
    if (px == MAP_FAILED) {
        printf("kmstest: mmap failed: %s\n", strerror(errno));
        return 1;
    }
    printf("kmstest: buffer %ux%u pitch %u size %llu handle %u fb %u mapped at %p\n", creq.width, creq.height,
           creq.pitch, (unsigned long long)creq.size, creq.handle, fb_id, (void *)px);

    draw(px, creq.width, creq.height, creq.pitch, 0);
    if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn->connector_id, 1, mode)) {
        printf("kmstest: SETCRTC failed: %s\n", strerror(errno));
        return 1;
    }
    printf("kmstest: mode %s@%u set on crtc %u\n", mode->name, mode->vrefresh, crtc_id);

    int frames = 0;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms >= secs * 1000L) break;
        draw(px, creq.width, creq.height, creq.pitch, (uint32_t)frames);
        drmModeDirtyFB(fd, fb_id, NULL, 0);
        frames++;
        sleep_ms(16);
    }
    printf("kmstest: %d frames in %d s\n", frames, secs);

    munmap(px, creq.size);
    if (drmModeCloseFB(fd, fb_id)) drmModeRmFB(fd, fb_id);
    struct drm_mode_destroy_dumb dreq = { .handle = creq.handle };
    drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);
    close(fd);
    printf("kmstest: OK\n");
    return 0;
}
