// SPDX-License-Identifier: MIT
// Copyright (c) 2026 korbolajnennikolaj

#include "limine_video_driver.h"

#include <components/drivers.h>
#include "gop_font.h"
#include "components/logger.h"
#include "kernel/limine.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern struct limine_framebuffer_request* get_framebuffer_request();

#ifndef SCROLL_CACHE_MAX_WIDTH
# define SCROLL_CACHE_MAX_WIDTH 3840
#endif
#ifndef SCROLL_CACHE_MAX_HEIGHT
# define SCROLL_CACHE_MAX_HEIGHT 2160
#endif

#define SCROLL_CACHE_MAX_BYTES \
    ((uint64_t)SCROLL_CACHE_MAX_WIDTH * SCROLL_CACHE_MAX_HEIGHT * 4)

static uint8_t scroll_cache_storage[SCROLL_CACHE_MAX_BYTES];

static uint8_t* framebuffer_base = NULL;
static uint64_t phys_width = 0;
static uint64_t phys_height = 0;
static uint64_t phys_pitch = 0;

static uint8_t* backbuffer = NULL;
static uint8_t* scroll_cache = NULL;
static uint64_t screen_width = 0;
static uint64_t screen_height = 0;

static uint32_t cursor_x = 0;
static uint32_t cursor_y = 0;
static uint32_t max_chars_x = 0;
static uint32_t max_chars_y = 0;
static uint32_t current_bg_color = 0x00000000;
static bool cursor_enabled = true;

static uint8_t* firmware_base = NULL;
static uint64_t firmware_width = 0;
static uint64_t firmware_height = 0;
static uint64_t firmware_pitch = 0;

static uint32_t damage_x1 = 0;
static uint32_t damage_y1 = 0;
static uint32_t damage_x2 = 0;
static uint32_t damage_y2 = 0;

static spinlock_t video_lock = SPINLOCK_INIT;

static inline void sse2_memcpy(void* dst, const void* src, size_t bytes)
{
    size_t n16 = bytes >> 4;
    __asm__ volatile (
        "test %0, %0\n\t"
        "jz 2f\n\t"
        "1:\n\t"
        "movdqu (%1), %%xmm0\n\t"
        "movdqu %%xmm0, (%2)\n\t"
        "add $16, %1\n\t"
        "add $16, %2\n\t"
        "dec %0\n\t"
        "jnz 1b\n\t"
        "2:\n\t"
        : "+r"(n16), "+r"(src), "+r"(dst)
        :
        : "xmm0", "memory"
    );

    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < (bytes & 15); i++) d[i] = s[i];
}

static inline void sse2_memcpy_nt(void* dst, const void* src, size_t bytes)
{
    size_t n16 = bytes >> 4;
    __asm__ volatile (
        "test %0, %0\n\t"
        "jz 2f\n\t"
        "1:\n\t"
        "movdqu (%1), %%xmm0\n\t"
        "movntdq %%xmm0, (%2)\n\t"
        "add $16, %1\n\t"
        "add $16, %2\n\t"
        "dec %0\n\t"
        "jnz 1b\n\t"
        "2:\n\t"
        "sfence\n\t"
        : "+r"(n16), "+r"(src), "+r"(dst)
        :
        : "xmm0", "memory"
    );

    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (size_t i = 0; i < (bytes & 15); i++) d[i] = s[i];
}

static inline void sse2_memset32_nt(void* dst, uint32_t val, size_t count)
{

    uint64_t v64 = ((uint64_t)val << 32) | val;

    size_t n16 = count >> 2;
    size_t rem = count & 3;

    __asm__ volatile (
        "movq   %3, %%xmm0\n\t"
        "movlhps %%xmm0, %%xmm0\n\t"
        "test   %0, %0\n\t"
        "jz     2f\n\t"
        "1:\n\t"
        "movntdq %%xmm0, (%1)\n\t"
        "add    $16, %1\n\t"
        "dec    %0\n\t"
        "jnz    1b\n\t"
        "2:\n\t"
        "sfence\n\t"
        : "+r"(n16), "+r"(dst)
        : "r"(v64), "r"(v64)
        : "xmm0", "memory"
    );

    uint32_t* p = (uint32_t*)dst;
    for (size_t i = 0; i < rem; i++) p[i] = val;
}

static inline void fast_memset32(void* dst, uint32_t val, size_t count)
{
    uint64_t v64 = ((uint64_t)val << 32) | val;
    uint64_t* p = (uint64_t*)dst;
    size_t n = count >> 1;
    for (size_t i = 0; i < n; i++) p[i] = v64;
    if (count & 1) ((uint32_t*)dst)[count - 1] = val;
}

static void mark_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (x >= screen_width || y >= screen_height || !w || !h) return;
    uint32_t x2 = x + w > screen_width ? (uint32_t)screen_width : x + w;
    uint32_t y2 = y + h > screen_height ? (uint32_t)screen_height : y + h;
    if (damage_x2 == 0) {
        damage_x1 = x;
        damage_y1 = y;
        damage_x2 = x2;
        damage_y2 = y2;
        return;
    }
    if (x < damage_x1) damage_x1 = x;
    if (y < damage_y1) damage_y1 = y;
    if (x2 > damage_x2) damage_x2 = x2;
    if (y2 > damage_y2) damage_y2 = y2;
}

static void scroll_cache_init(void)
{
    uint64_t needed = screen_height * phys_pitch;

    if (needed > SCROLL_CACHE_MAX_BYTES) {
        scroll_cache = NULL;
        return;
    }

    scroll_cache = scroll_cache_storage;

    uint64_t row_bytes = FONT_HEIGHT * phys_pitch;

    sse2_memcpy(scroll_cache,
            backbuffer,
            needed);

    fast_memset32(scroll_cache + (screen_height - FONT_HEIGHT) * phys_pitch,
                  current_bg_color,
                  (uint32_t)(FONT_HEIGHT * screen_width));
}

static void rect_fill_buf(uint8_t* buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (!buf) return;
    if (x >= screen_width || y >= screen_height) return;
    if (x + w > screen_width) w = (uint32_t)(screen_width - x);
    if (y + h > screen_height) h = (uint32_t)(screen_height - y);
    for (uint32_t i = 0; i < h; i++) {
        void* row = buf + (y + i) * phys_pitch + x * 4;
        fast_memset32(row, color, w);
    }
}

static void fast_rect_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    mark_damage(x, y, w, h);
    rect_fill_buf(backbuffer, x, y, w, h, color);
    if (scroll_cache && y >= FONT_HEIGHT)
        rect_fill_buf(scroll_cache, x, y - FONT_HEIGHT, w, h, color);
}

static void draw_char_buf(uint8_t* buf, char c, uint32_t color, uint16_t cx, uint16_t cy)
{
    if (!buf) return;
    uint32_t px = (uint32_t)cx * FONT_WIDTH;
    uint32_t py = (uint32_t)cy * FONT_HEIGHT;
    if (px >= screen_width || py >= screen_height) return;

    uint8_t idx = (uint8_t)c & 0x7F;
    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint32_t* line = (uint32_t*)(buf + (py + row) * phys_pitch + px * 4);
        uint8_t bits = gop_font_bitmap[idx][row];
        for (int col = 0; col < FONT_WIDTH; col++)
            line[col] = ((bits >> (7 - col)) & 1) ? color : current_bg_color;
    }
}

static void limine_video_char_at_raw(char c, uint32_t color, uint16_t cx, uint16_t cy)
{
    mark_damage((uint32_t)cx * FONT_WIDTH, (uint32_t)cy * FONT_HEIGHT, FONT_WIDTH, FONT_HEIGHT);
    draw_char_buf(backbuffer, c, color, cx, cy);
    if (scroll_cache)
        draw_char_buf(scroll_cache, c, color, cx, cy);
}

static void limine_video_char_at(char c, uint32_t color, uint16_t cx, uint16_t cy)
{
    uint64_t flags = spin_lock_irqsave(&video_lock);
    limine_video_char_at_raw(c, color, cx, cy);
    spin_unlock_irqrestore(&video_lock, flags);
}

static void draw_cursor_shape(uint32_t color) {
    if (!cursor_enabled) return;
    fast_rect_fill(cursor_x * FONT_WIDTH,
                   cursor_y * FONT_HEIGHT + (FONT_HEIGHT - 2),
                   FONT_WIDTH, 2, color);
}

static void limine_video_clear(uint32_t color)
{
    uint64_t flags = spin_lock_irqsave(&video_lock);

    current_bg_color = color;
    size_t total_px = (size_t)(screen_height * phys_pitch) >> 2;

    sse2_memset32_nt(backbuffer, color, total_px);

    if (scroll_cache) {

        fast_memset32(scroll_cache, color, total_px);
    }

    cursor_x = cursor_y = 0;
    mark_damage(0, 0, (uint32_t)screen_width, (uint32_t)screen_height);

    spin_unlock_irqrestore(&video_lock, flags);
}

static void limine_video_scroll(void)
{
    if (screen_height <= FONT_HEIGHT) return;
    mark_damage(0, 0, (uint32_t)screen_width, (uint32_t)screen_height);

    uint64_t total = screen_height * phys_pitch;
    uint64_t row_bytes = FONT_HEIGHT * phys_pitch;

    if (scroll_cache) {

        sse2_memcpy(scroll_cache,
                    scroll_cache + row_bytes,
                    total - row_bytes);

        uint32_t last_px = (uint32_t)(FONT_HEIGHT * screen_width);
        uint8_t* last = scroll_cache + (screen_height - FONT_HEIGHT) * phys_pitch;

        fast_memset32(last, current_bg_color, last_px);

        sse2_memcpy_nt(backbuffer, scroll_cache, total);

    } else {

        sse2_memcpy_nt(backbuffer, backbuffer + row_bytes, total - row_bytes);

        sse2_memset32_nt(backbuffer + (screen_height - FONT_HEIGHT) * phys_pitch,
                         current_bg_color,
                         (uint32_t)(FONT_HEIGHT * screen_width));
    }
}

static void limine_video_printf(const char* string, uint32_t color)
{
    uint64_t flags = spin_lock_irqsave(&video_lock);

    draw_cursor_shape(current_bg_color);
    while (*string) {
        switch (*string) {
            case '\n': cursor_x = 0; cursor_y++; break;
            case '\r': cursor_x = 0; break;
            case '\b':
                if (cursor_x > 0) cursor_x--;
                else if (cursor_y > 0) { cursor_y--; cursor_x = max_chars_x - 1; }
                limine_video_char_at_raw(' ', current_bg_color, cursor_x, cursor_y);
                break;
            default:
                limine_video_char_at_raw(*string, color, cursor_x, cursor_y);
                if (++cursor_x >= max_chars_x) { cursor_x = 0; cursor_y++; }
                break;
        }
        if (cursor_y >= max_chars_y) {
            limine_video_scroll();
            cursor_y = max_chars_y - 1;
        }
        string++;
    }
    draw_cursor_shape(color);

    spin_unlock_irqrestore(&video_lock, flags);
}

static void limine_video_set_cursor_visible(bool visible) {
    uint64_t flags = spin_lock_irqsave(&video_lock);
    if (!visible) draw_cursor_shape(current_bg_color);
    cursor_enabled = visible;
    spin_unlock_irqrestore(&video_lock, flags);
}

static void limine_video_set_cursor_pos(uint32_t x, uint32_t y) {
    uint64_t flags = spin_lock_irqsave(&video_lock);
    draw_cursor_shape(current_bg_color);
    if (x < max_chars_x) cursor_x = x;
    if (y < max_chars_y) cursor_y = y;
    draw_cursor_shape(LIMINE_COLOR_WHITE);
    spin_unlock_irqrestore(&video_lock, flags);
}

static void limine_video_get_display_resolution(uint64_t* w, uint64_t* h) {
    uint64_t flags = spin_lock_irqsave(&video_lock);
    if (w) *w = phys_width;
    if (h) *h = phys_height;
    spin_unlock_irqrestore(&video_lock, flags);
}

static bool limine_video_set_resolution(uint64_t width, uint64_t height) {
    if (!width || width > phys_width || !height || height > phys_height) {
        LOG_WARNING("rejected resolution %llux%llu, framebuffer is %llux%llu",
                    (unsigned long long)width, (unsigned long long)height,
                    (unsigned long long)phys_width, (unsigned long long)phys_height);
        return false;
    }

    uint64_t flags = spin_lock_irqsave(&video_lock);

    screen_width = width;
    screen_height = height;
    max_chars_x = (uint32_t)(width / FONT_WIDTH);
    max_chars_y = (uint32_t)(height / FONT_HEIGHT);
    cursor_x = cursor_y = 0;

    scroll_cache_init();

    spin_unlock_irqrestore(&video_lock, flags);

    LOG_INFO("text area set to %llux%llu (%ux%u cells)%s",
             (unsigned long long)width, (unsigned long long)height,
             max_chars_x, max_chars_y, scroll_cache ? "" : ", no scroll cache");
    return true;
}

static void reflow_scroll_cache(uint64_t width, uint64_t height, uint64_t pitch, uint32_t skip_rows)
{
    if (height * pitch > SCROLL_CACHE_MAX_BYTES) {
        scroll_cache = NULL;
        return;
    }
    if (!scroll_cache) {
        scroll_cache = scroll_cache_storage;
        for (uint64_t r = 0; r < height; r++) fast_memset32(scroll_cache + r * pitch, current_bg_color, (uint32_t)width);
        return;
    }
    uint64_t old_pitch = phys_pitch;
    uint64_t rows = screen_height > skip_rows ? screen_height - skip_rows : 0;
    if (rows > height) rows = height;
    uint64_t copy_px = screen_width < width ? screen_width : width;
    if (pitch > old_pitch) {
        for (uint64_t r = rows; r-- > 0;)
            memmove(scroll_cache + r * pitch, scroll_cache + (r + skip_rows) * old_pitch, copy_px * 4);
    } else {
        for (uint64_t r = 0; r < rows; r++)
            memmove(scroll_cache + r * pitch, scroll_cache + (r + skip_rows) * old_pitch, copy_px * 4);
    }
    for (uint64_t r = 0; r < height; r++) {
        if (r < rows) {
            if (copy_px < width) fast_memset32(scroll_cache + r * pitch + copy_px * 4, current_bg_color, (uint32_t)(width - copy_px));
        } else {
            fast_memset32(scroll_cache + r * pitch, current_bg_color, (uint32_t)width);
        }
    }
}

static void video_retarget(uint8_t* pixels, uint64_t width, uint64_t height, uint64_t pitch)
{
    uint32_t new_rows = (uint32_t)(height / FONT_HEIGHT);
    uint32_t skip = cursor_y >= new_rows ? cursor_y + 1 - new_rows : 0;

    reflow_scroll_cache(width, height, pitch, skip * FONT_HEIGHT);

    framebuffer_base = pixels;
    backbuffer = pixels;
    phys_width = screen_width = width;
    phys_height = screen_height = height;
    phys_pitch = pitch;
    max_chars_x = (uint32_t)(width / FONT_WIDTH);
    max_chars_y = new_rows;
    cursor_y -= skip;
    if (cursor_x >= max_chars_x) cursor_x = max_chars_x ? max_chars_x - 1 : 0;

    if (scroll_cache) sse2_memcpy(backbuffer, scroll_cache, height * pitch);
    else for (uint64_t r = 0; r < height; r++) fast_memset32(backbuffer + r * pitch, current_bg_color, (uint32_t)width);
    damage_x2 = 0;
    mark_damage(0, 0, (uint32_t)width, (uint32_t)height);
}

bool limine_video_attach(void* pixels, uint64_t width, uint64_t height, uint64_t pitch)
{
    if (!pixels || width < FONT_WIDTH || height < FONT_HEIGHT || pitch < width * 4) return false;
    uint64_t flags = spin_lock_irqsave(&video_lock);
    video_retarget((uint8_t*)pixels, width, height, pitch);
    spin_unlock_irqrestore(&video_lock, flags);
    LOG_INFO("console moved to a %llux%llu framebuffer at 0x%llx", (unsigned long long)width,
             (unsigned long long)height, (unsigned long long)(uintptr_t)pixels);
    return true;
}

void limine_video_detach(void)
{
    uint64_t flags = spin_lock_irqsave(&video_lock);
    if (firmware_base) video_retarget(firmware_base, firmware_width, firmware_height, firmware_pitch);
    spin_unlock_irqrestore(&video_lock, flags);
}

void limine_video_panic_takeover(void)
{
    spin_lock_init(&video_lock);
    if (firmware_base && framebuffer_base != firmware_base)
        video_retarget(firmware_base, firmware_width, firmware_height, firmware_pitch);
}

bool limine_video_take_damage(uint32_t rect[4])
{
    uint64_t flags = spin_lock_irqsave(&video_lock);
    bool any = damage_x2 != 0;
    if (any) {
        rect[0] = damage_x1;
        rect[1] = damage_y1;
        rect[2] = damage_x2;
        rect[3] = damage_y2;
        damage_x2 = 0;
    }
    spin_unlock_irqrestore(&video_lock, flags);
    return any;
}

bool limine_video_fb_info(uint8_t** pixels, uint32_t* width, uint32_t* height, uint32_t* pitch)
{
    if (!backbuffer || !screen_width || !screen_height) return false;
    uint64_t flags = spin_lock_irqsave(&video_lock);
    if (pixels) *pixels = backbuffer;
    if (width) *width = (uint32_t)screen_width;
    if (height) *height = (uint32_t)screen_height;
    if (pitch) *pitch = (uint32_t)phys_pitch;
    spin_unlock_irqrestore(&video_lock, flags);
    return true;
}

void limine_video_announce_damage(void)
{
    uint64_t flags = spin_lock_irqsave(&video_lock);
    if (backbuffer && screen_width && screen_height)
        mark_damage(0, 0, screen_width, screen_height);
    spin_unlock_irqrestore(&video_lock, flags);
}

struct limine_video_driver limine_loaded_driver = {
    .clear = limine_video_clear,
    .char_at = limine_video_char_at,
    .printf = limine_video_printf,
    .set_cursor_visible = limine_video_set_cursor_visible,
    .set_cursor_pos = limine_video_set_cursor_pos,
    .get_display_resolution = limine_video_get_display_resolution,
    .set_resolution = limine_video_set_resolution,
};

struct limine_video_driver* return_limine_video_driver(void) {
    struct limine_framebuffer_request* req = get_framebuffer_request();
    if (!req || !req->response || req->response->framebuffer_count < 1) {
        LOG_ERROR("bootloader provided no framebuffer");
        return NULL;
    }

    struct limine_framebuffer* fb = req->response->framebuffers[0];

    framebuffer_base = (uint8_t*)(uintptr_t)fb->address;
    phys_width = fb->width;
    phys_height = fb->height;
    phys_pitch = fb->pitch;
    firmware_base = framebuffer_base;
    firmware_width = phys_width;
    firmware_height = phys_height;
    firmware_pitch = phys_pitch;

    backbuffer = framebuffer_base;
    screen_width = phys_width;
    screen_height = phys_height;
    max_chars_x = (uint32_t)(screen_width / FONT_WIDTH);
    max_chars_y = (uint32_t)(screen_height / FONT_HEIGHT);

    scroll_cache_init();

    LOG_INFO("framebuffer %llux%llu, %u bpp, pitch %llu at 0x%llx, %ux%u text cells, %llu framebuffer(s)",
             (unsigned long long)phys_width, (unsigned long long)phys_height, (unsigned)fb->bpp,
             (unsigned long long)phys_pitch, (unsigned long long)(uintptr_t)framebuffer_base,
             max_chars_x, max_chars_y, (unsigned long long)req->response->framebuffer_count);
    if (!scroll_cache)
        LOG_WARNING("framebuffer larger than %ux%u, scroll cache disabled (slower scrolling)",
                    SCROLL_CACHE_MAX_WIDTH, SCROLL_CACHE_MAX_HEIGHT);
    return &limine_loaded_driver;
}

struct driver* return_meta_limine_video_driver(void) {
    static struct driver meta = {
        .name = "Limine Video Driver",
        .type = LIMINE_VIDEO_DRIVER,
        .sub_type = 0,
        .status = DRIVER_STATUS_READY,
        .dependencies = {0},
        .dependency_count = 0,
        .self = &limine_loaded_driver,
        .init = (void*)return_limine_video_driver,
    };
    return &meta;
}
