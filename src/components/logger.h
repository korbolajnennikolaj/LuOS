#ifndef LOGGER_H
#define LOGGER_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "drivers/Timer/rtc_driver.h"

#define MAX_LOGGER_MESSAGE_SIZE 1024
#define MAX_LOGGER_CALLER_SIZE 64
#define LOGGER_LINE_SIZE (MAX_LOGGER_MESSAGE_SIZE + MAX_LOGGER_CALLER_SIZE + 64)

#define LOGGER_BUFFER_SIZE (256 * 1024)
#define LOGGER_DUMP_CHUNK_SIZE 512
#define LOGGER_PANIC_LOCK_SPINS 1000000

#define LOGGER_DEFAULT_BUFFER_LEVEL LOGGER_LEVEL_DEBUG
#define LOGGER_DEFAULT_UART_LEVEL LOGGER_LEVEL_DEBUG
#define LOGGER_DEFAULT_VIDEO_LEVEL LOGGER_LEVEL_INFO

enum logger_level_t {
    LOGGER_LEVEL_DEBUG,
    LOGGER_LEVEL_INFO,
    LOGGER_LEVEL_WARNING,
    LOGGER_LEVEL_ERROR
};

enum logger_output_t {
    LOGGER_OUTPUT_BUFFER,
    LOGGER_OUTPUT_UART,
    LOGGER_OUTPUT_VIDEO,
    LOGGER_OUTPUT_COUNT
};

struct logger_message_t {
    char message[MAX_LOGGER_MESSAGE_SIZE];
    size_t length;
    char caller_name[MAX_LOGGER_CALLER_SIZE];
    size_t caller_name_length;
    enum logger_level_t level;
    system_time timestamp;
    uint16_t milliseconds;

    bool use_uart;
    bool use_limine_video;
};

void logger_init(void);
void logger_log(struct logger_message_t *message);

void logger_printf(enum logger_level_t level, const char *caller, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
void logger_vprintf(enum logger_level_t level, const char *caller, const char *format, va_list args);

void logger_set_level(enum logger_output_t output, enum logger_level_t level);
enum logger_level_t logger_get_level(enum logger_output_t output);
const char *logger_level_name(enum logger_level_t level);
bool logger_level_from_name(const char *name, enum logger_level_t *out_level);

void logger_clear_buffer(void);
void logger_dump_buffer_to_text(void (*output_function)(const char *message, size_t length));
void logger_dump_last_to_text(size_t count, void (*output_function)(const char *message, size_t length));

size_t logger_buffer_used(void);
uint64_t logger_message_count(void);
uint64_t logger_dropped_bytes(void);

void logger_enter_panic_mode(void);

#define LOG_DEBUG(...) logger_printf(LOGGER_LEVEL_DEBUG, __func__, __VA_ARGS__)
#define LOG_INFO(...) logger_printf(LOGGER_LEVEL_INFO, __func__, __VA_ARGS__)
#define LOG_WARNING(...) logger_printf(LOGGER_LEVEL_WARNING, __func__, __VA_ARGS__)
#define LOG_ERROR(...) logger_printf(LOGGER_LEVEL_ERROR, __func__, __VA_ARGS__)

#endif
