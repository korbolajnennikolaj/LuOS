#ifndef LIMINE_VIDEO_DRIVER_H
#define LIMINE_VIDEO_DRIVER_H
#include <stdbool.h>
#include <stdint.h>

enum limine_color {
    LIMINE_COLOR_BLACK = 0xFF000000,
    LIMINE_COLOR_BLUE = 0xFF0000AA,
    LIMINE_COLOR_GREEN = 0xFF00AA00,
    LIMINE_COLOR_CYAN = 0xFF00AAAA,
    LIMINE_COLOR_RED = 0xFFAA0000,
    LIMINE_COLOR_MAGENTA = 0xFFAA00AA,
    LIMINE_COLOR_BROWN = 0xFFAA5500,
    LIMINE_COLOR_LIGHT_GRAY = 0xFFAAAAAA,
    LIMINE_COLOR_DARK_GRAY = 0xFF555555,
    LIMINE_COLOR_LIGHT_BLUE = 0xFF5555FF,
    LIMINE_COLOR_LIGHT_GREEN = 0xFF55FF55,
    LIMINE_COLOR_LIGHT_CYAN = 0xFF55FFFF,
    LIMINE_COLOR_LIGHT_RED = 0xFFFF5555,
    LIMINE_COLOR_LIGHT_MAGENTA = 0xFFFF55FF,
    LIMINE_COLOR_YELLOW = 0xFFFFFF55,
    LIMINE_COLOR_WHITE = 0xFFFFFFFF,
    LIMINE_COLOR_ORANGE = 0xFFFF8800,
    LIMINE_COLOR_PURPLE = 0xFF8800FF,
    LIMINE_COLOR_PINK = 0xFFFF88FF,
    LIMINE_COLOR_TURQUOISE = 0xFF00FFCC,
    LIMINE_COLOR_DEEP_BLUE = 0xFF000088,
    LIMINE_COLOR_DARK_GREEN = 0xFF004400,
    LIMINE_COLOR_DARK_RED = 0xFF440000,
    LIMINE_COLOR_GOLD = 0xFFFFD700,
    LIMINE_COLOR_SILVER = 0xFFC0C0C0,
    LIMINE_COLOR_SKY_BLUE = 0xFF87CEEB,
    LIMINE_COLOR_HOT_PINK = 0xFFFF69B4,
    LIMINE_COLOR_LIME = 0xFF32CD32,

    LIMINE_COLOR_AMBER = 0xFFE5C07B,
};

typedef struct limine_video_driver {
    void (*clear)(uint32_t color);
    void (*char_at)(char c, uint32_t color, uint16_t x, uint16_t y);
    void (*printf)(const char* string, uint32_t color);
    void (*set_cursor_visible)(bool visible);
    void (*set_cursor_pos)(uint32_t x, uint32_t y);
    void (*get_display_resolution)(uint64_t* out_width, uint64_t* out_height);
    bool (*set_resolution)(uint64_t width, uint64_t height);
} limine_video_driver;

struct limine_video_driver* return_limine_video_driver();
struct driver* return_meta_limine_video_driver();

#endif
