#include "logger.h"

#include "components/drivers.h"
#include "drivers/Serial/uart_driver.h"
#include "drivers/Timer/timer.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/scheduler/spinlock.h"

#include <stdio.h>
#include <string.h>

// format [timestamp] [level] [caller] message

static spinlock_t logger_lock = SPINLOCK_INIT;

static char logger_buffer[LOGGER_BUFFER_SIZE];
static uint64_t logger_buffer_head = 0;
static uint64_t logger_buffer_start = 0;
static uint64_t logger_messages = 0;
static uint64_t logger_dropped = 0;

static char logger_line[LOGGER_LINE_SIZE];

static enum logger_level_t logger_levels[LOGGER_OUTPUT_COUNT] = {
    [LOGGER_OUTPUT_BUFFER] = LOGGER_DEFAULT_BUFFER_LEVEL,
    [LOGGER_OUTPUT_UART] = LOGGER_DEFAULT_UART_LEVEL,
    [LOGGER_OUTPUT_VIDEO] = LOGGER_DEFAULT_VIDEO_LEVEL,
};

static struct uart_driver *logger_uart = NULL;
static struct limine_video_driver *logger_video = NULL;
static bool logger_uart_synced = false;

static volatile uint32_t logger_time_ready = 0;
static volatile uint32_t logger_time_syncing = 0;
static struct system_time logger_time_base;
static uint64_t logger_time_base_ms = 0;
static struct tsc_driver *logger_tsc = NULL;

static volatile bool logger_panic_mode = false;

static const char *const logger_level_names[] = {
    [LOGGER_LEVEL_DEBUG] = "DEBUG",
    [LOGGER_LEVEL_INFO] = "INFO",
    [LOGGER_LEVEL_WARNING] = "WARNING",
    [LOGGER_LEVEL_ERROR] = "ERROR",
};

static const uint32_t logger_level_colors[] = {
    [LOGGER_LEVEL_DEBUG] = LIMINE_COLOR_DARK_GRAY,
    [LOGGER_LEVEL_INFO] = LIMINE_COLOR_LIGHT_GREEN,
    [LOGGER_LEVEL_WARNING] = LIMINE_COLOR_GOLD,
    [LOGGER_LEVEL_ERROR] = LIMINE_COLOR_LIGHT_RED,
};

static const uint32_t logger_message_colors[] = {
    [LOGGER_LEVEL_DEBUG] = LIMINE_COLOR_LIGHT_GRAY,
    [LOGGER_LEVEL_INFO] = LIMINE_COLOR_LIGHT_GRAY,
    [LOGGER_LEVEL_WARNING] = LIMINE_COLOR_AMBER,
    [LOGGER_LEVEL_ERROR] = LIMINE_COLOR_LIGHT_RED,
};

static bool logger_acquire(uint64_t *flags) {
    asm volatile("pushfq; pop %0" : "=r"(*flags));
    asm volatile("cli");

    if (!logger_panic_mode) {
        spin_lock(&logger_lock);
        return true;
    }

    for (uint32_t i = 0; i < LOGGER_PANIC_LOCK_SPINS; i++) {
        if (spin_trylock(&logger_lock)) return true;
        asm volatile("pause");
    }
    return false;
}

static void logger_release(uint64_t flags, bool owned) {
    if (owned) spin_unlock(&logger_lock);
    if (flags & (1 << 9))
        asm volatile("sti");
}

static bool logger_driver_ready(enum DRIVER_TYPE type, int sub_type) {
    struct driver *meta = get_meta_driver(type, sub_type);
    return meta && meta->self && meta->status == DRIVER_STATUS_READY;
}

static void logger_resolve_outputs(void) {
    if (!logger_uart && logger_driver_ready(SERIAL_DRIVER, UART_COM1))
        logger_uart = (struct uart_driver *)get_meta_driver(SERIAL_DRIVER, UART_COM1)->self;

    if (!logger_video && logger_driver_ready(LIMINE_VIDEO_DRIVER, 0))
        logger_video = (struct limine_video_driver *)get_meta_driver(LIMINE_VIDEO_DRIVER, 0)->self;
}

static void logger_sync_time(void) {
    if (logger_time_ready) return;
    if (!logger_driver_ready(TIMER_DRIVER, RTC_TIMER)) return;
    if (!logger_driver_ready(TIMER_DRIVER, TSC_TIMER)) return;
    if (__atomic_exchange_n(&logger_time_syncing, 1, __ATOMIC_ACQUIRE)) return;

    struct rtc_driver *rtc = (struct rtc_driver *)get_meta_driver(TIMER_DRIVER, RTC_TIMER)->self;
    logger_tsc = (struct tsc_driver *)get_meta_driver(TIMER_DRIVER, TSC_TIMER)->self;

    struct system_time *now = rtc->get_rtc_time();
    if (now) logger_time_base = *now;
    logger_time_base_ms = logger_tsc->get_tsc_uptime_ms();

    __atomic_store_n(&logger_time_ready, 1, __ATOMIC_RELEASE);
}

static uint8_t logger_days_in_month(uint16_t year, uint8_t month) {
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    return days[month - 1];
}

static void logger_stamp(struct system_time *out, uint16_t *out_ms) {
    memset(out, 0, sizeof(*out));
    *out_ms = 0;

    logger_sync_time();
    if (!__atomic_load_n(&logger_time_ready, __ATOMIC_ACQUIRE)) return;

    uint64_t now_ms = logger_tsc->get_tsc_uptime_ms();
    uint64_t elapsed_ms = (now_ms > logger_time_base_ms) ? now_ms - logger_time_base_ms : 0;

    *out = logger_time_base;
    *out_ms = (uint16_t)(elapsed_ms % 1000);

    uint64_t total = (uint64_t)logger_time_base.hours * 3600
                   + (uint64_t)logger_time_base.minutes * 60
                   + logger_time_base.seconds
                   + elapsed_ms / 1000;

    uint64_t days = total / 86400;
    total %= 86400;

    out->hours = (uint8_t)(total / 3600);
    out->minutes = (uint8_t)((total / 60) % 60);
    out->seconds = (uint8_t)(total % 60);

    while (days--) {
        out->weekday = (uint8_t)(out->weekday % 7 + 1);
        if (++out->day > logger_days_in_month(out->year, out->month)) {
            out->day = 1;
            if (++out->month > 12) {
                out->month = 1;
                out->year++;
            }
        }
    }
}

static void logger_buffer_write(const char *data, size_t length) {
    if (length > LOGGER_BUFFER_SIZE) {
        data += length - LOGGER_BUFFER_SIZE;
        length = LOGGER_BUFFER_SIZE;
    }

    size_t offset = (size_t)(logger_buffer_head % LOGGER_BUFFER_SIZE);
    size_t first = LOGGER_BUFFER_SIZE - offset;
    if (first > length) first = length;

    memcpy(&logger_buffer[offset], data, first);
    if (length > first) memcpy(logger_buffer, data + first, length - first);

    uint64_t old_tail = (logger_buffer_head > LOGGER_BUFFER_SIZE) ? logger_buffer_head - LOGGER_BUFFER_SIZE : 0;
    logger_buffer_head += length;
    uint64_t new_tail = (logger_buffer_head > LOGGER_BUFFER_SIZE) ? logger_buffer_head - LOGGER_BUFFER_SIZE : 0;

    if (new_tail > logger_buffer_start) {
        uint64_t lost_from = (old_tail > logger_buffer_start) ? old_tail : logger_buffer_start;
        logger_dropped += new_tail - lost_from;
    }
}

static inline char logger_buffer_at(uint64_t position) {
    return logger_buffer[position % LOGGER_BUFFER_SIZE];
}

static uint64_t logger_first_line(void) {
    uint64_t tail = (logger_buffer_head > LOGGER_BUFFER_SIZE) ? logger_buffer_head - LOGGER_BUFFER_SIZE : 0;
    if (tail <= logger_buffer_start) return logger_buffer_start;

    uint64_t position = tail;
    while (position < logger_buffer_head && logger_buffer_at(position) != '\n') position++;
    if (position < logger_buffer_head) position++;
    return position;
}

static size_t logger_buffer_copy(uint64_t position, char *out, size_t length) {
    size_t offset = (size_t)(position % LOGGER_BUFFER_SIZE);
    size_t first = LOGGER_BUFFER_SIZE - offset;
    if (first > length) first = length;

    memcpy(out, &logger_buffer[offset], first);
    if (length > first) memcpy(out + first, logger_buffer, length - first);
    return length;
}

static void logger_uart_write_buffer(uint64_t from, uint64_t to) {
    char chunk[LOGGER_DUMP_CHUNK_SIZE];

    while (from < to) {
        size_t length = (size_t)(to - from);
        if (length > sizeof(chunk)) length = sizeof(chunk);
        logger_buffer_copy(from, chunk, length);
        logger_uart->write(chunk, length);
        from += length;
    }
}

static size_t logger_copy_text(char *dst, size_t pos, size_t cap, const char *src) {
    while (*src && pos + 1 < cap) dst[pos++] = *src++;
    dst[pos] = '\0';
    return pos;
}

static size_t logger_format_line(const struct logger_message_t *message, char *line, size_t cap) {
    const system_time *t = &message->timestamp;
    enum logger_level_t level = message->level;
    if ((unsigned)level > LOGGER_LEVEL_ERROR) level = LOGGER_LEVEL_ERROR;

    int n = snprintf(line, cap, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [%s] [",
                     (unsigned)t->year, (unsigned)t->month, (unsigned)t->day,
                     (unsigned)t->hours, (unsigned)t->minutes, (unsigned)t->seconds,
                     (unsigned)message->milliseconds, logger_level_names[level]);
    size_t pos = (n < 0) ? 0 : (size_t)n;
    if (pos >= cap) pos = cap - 1;

    pos = logger_copy_text(line, pos, cap, message->caller_name);
    pos = logger_copy_text(line, pos, cap, "] ");
    pos = logger_copy_text(line, pos, cap, message->message);

    if (pos + 1 >= cap) pos = cap - 2;
    line[pos++] = '\n';
    line[pos] = '\0';
    return pos;
}

static void logger_video_write(const struct logger_message_t *message, char *line) {
    enum logger_level_t level = message->level;
    if ((unsigned)level > LOGGER_LEVEL_ERROR) level = LOGGER_LEVEL_ERROR;

    char *level_open = strchr(line, ']');
    if (!level_open) return;
    level_open += 2;

    char *caller_open = strchr(level_open, ']');
    if (!caller_open) return;
    caller_open += 2;

    char *message_start = strchr(caller_open, ']');
    if (!message_start) return;
    message_start += 1;

    char saved = *level_open;
    *level_open = '\0';
    logger_video->printf(line, LIMINE_COLOR_DARK_GRAY);
    *level_open = saved;

    saved = *caller_open;
    *caller_open = '\0';
    logger_video->printf(level_open, logger_level_colors[level]);
    *caller_open = saved;

    saved = *message_start;
    *message_start = '\0';
    logger_video->printf(caller_open, LIMINE_COLOR_LIGHT_CYAN);
    *message_start = saved;

    logger_video->printf(message_start, logger_message_colors[level]);
}

static void logger_sanitize(struct logger_message_t *message) {
    size_t length = 0;
    while (length < MAX_LOGGER_MESSAGE_SIZE - 1 && message->message[length]) {
        char c = message->message[length];
        if (c == '\n' || c == '\r' || c == '\t') message->message[length] = ' ';
        else if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) message->message[length] = '?';
        length++;
    }
    while (length > 0 && message->message[length - 1] == ' ') length--;
    message->message[length] = '\0';
    message->length = length;

    size_t caller_length = 0;
    while (caller_length < MAX_LOGGER_CALLER_SIZE - 1 && message->caller_name[caller_length]) caller_length++;
    message->caller_name[caller_length] = '\0';
    message->caller_name_length = caller_length;
}

void logger_log(struct logger_message_t *message) {
    if (!message) return;

    logger_sanitize(message);
    logger_stamp(&message->timestamp, &message->milliseconds);
    logger_resolve_outputs();

    uint64_t flags;
    bool owned = logger_acquire(&flags);

    size_t length = logger_format_line(message, logger_line, sizeof(logger_line));

    if (logger_uart && !logger_uart_synced) {
        logger_uart_synced = true;
        if (logger_uart->is_present())
            logger_uart_write_buffer(logger_first_line(), logger_buffer_head);
    }

    if (message->level >= logger_levels[LOGGER_OUTPUT_BUFFER]) {
        logger_buffer_write(logger_line, length);
        logger_messages++;
    }

    if (message->use_uart && logger_uart && logger_uart->is_present())
        logger_uart->write(logger_line, length);

    if (message->use_limine_video && logger_video && logger_video->printf)
        logger_video_write(message, logger_line);

    logger_release(flags, owned);
}

void logger_vprintf(enum logger_level_t level, const char *caller, const char *format, va_list args) {
    struct logger_message_t message;

    if ((unsigned)level > LOGGER_LEVEL_ERROR) level = LOGGER_LEVEL_ERROR;

    int n = vsnprintf(message.message, sizeof(message.message), format ? format : "", args);
    message.length = (n < 0) ? 0 : (size_t)n;

    size_t i = 0;
    if (!caller) caller = "kernel";
    while (caller[i] && i < sizeof(message.caller_name) - 1) {
        message.caller_name[i] = caller[i];
        i++;
    }
    message.caller_name[i] = '\0';
    message.caller_name_length = i;

    message.level = level;
    message.milliseconds = 0;
    message.use_uart = level >= logger_levels[LOGGER_OUTPUT_UART];
    message.use_limine_video = level >= logger_levels[LOGGER_OUTPUT_VIDEO];

    if (!message.use_uart && !message.use_limine_video && level < logger_levels[LOGGER_OUTPUT_BUFFER])
        return;

    logger_log(&message);
}

void logger_printf(enum logger_level_t level, const char *caller, const char *format, ...) {
    va_list args;
    va_start(args, format);
    logger_vprintf(level, caller, format, args);
    va_end(args);
}

void logger_set_level(enum logger_output_t output, enum logger_level_t level) {
    if ((unsigned)output >= LOGGER_OUTPUT_COUNT) return;
    if ((unsigned)level > LOGGER_LEVEL_ERROR) level = LOGGER_LEVEL_ERROR;
    logger_levels[output] = level;
}

enum logger_level_t logger_get_level(enum logger_output_t output) {
    if ((unsigned)output >= LOGGER_OUTPUT_COUNT) return LOGGER_LEVEL_ERROR;
    return logger_levels[output];
}

const char *logger_level_name(enum logger_level_t level) {
    if ((unsigned)level > LOGGER_LEVEL_ERROR) return "UNKNOWN";
    return logger_level_names[level];
}

bool logger_level_from_name(const char *name, enum logger_level_t *out_level) {
    if (!name || !out_level) return false;

    for (unsigned i = 0; i <= LOGGER_LEVEL_ERROR; i++) {
        const char *a = name;
        const char *b = logger_level_names[i];
        while (*a && *b) {
            char c = *a;
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            if (c != *b) break;
            a++;
            b++;
        }
        if (*a == '\0' && *b == '\0') {
            *out_level = (enum logger_level_t)i;
            return true;
        }
    }

    if (strcmp(name, "warn") == 0 || strcmp(name, "WARN") == 0) {
        *out_level = LOGGER_LEVEL_WARNING;
        return true;
    }
    return false;
}

void logger_clear_buffer(void) {
    uint64_t flags;
    bool owned = logger_acquire(&flags);
    logger_buffer_start = logger_buffer_head;
    logger_messages = 0;
    logger_dropped = 0;
    logger_release(flags, owned);
}

static void logger_dump_range(uint64_t position, uint64_t end,
                              void (*output_function)(const char *message, size_t length)) {
    char chunk[LOGGER_DUMP_CHUNK_SIZE];

    while (position < end) {
        uint64_t flags;
        bool owned = logger_acquire(&flags);

        uint64_t oldest = logger_first_line();
        if (position < oldest) position = oldest;
        if (end > logger_buffer_head) end = logger_buffer_head;

        size_t length = 0;
        if (position < end) {
            length = (size_t)(end - position);
            if (length > sizeof(chunk)) length = sizeof(chunk);
            logger_buffer_copy(position, chunk, length);
        }

        logger_release(flags, owned);

        if (length == 0) break;
        output_function(chunk, length);
        position += length;
    }
}

void logger_dump_buffer_to_text(void (*output_function)(const char *message, size_t length)) {
    if (!output_function) return;

    uint64_t flags;
    bool owned = logger_acquire(&flags);
    uint64_t start = logger_first_line();
    uint64_t end = logger_buffer_head;
    logger_release(flags, owned);

    logger_dump_range(start, end, output_function);
}

void logger_dump_last_to_text(size_t count, void (*output_function)(const char *message, size_t length)) {
    if (!output_function || count == 0) return;

    uint64_t flags;
    bool owned = logger_acquire(&flags);

    uint64_t oldest = logger_first_line();
    uint64_t end = logger_buffer_head;
    uint64_t start = end;
    size_t lines = 0;

    while (start > oldest) {
        if (logger_buffer_at(start - 1) == '\n' && start != end) {
            if (++lines >= count) break;
        }
        start--;
    }

    logger_release(flags, owned);

    logger_dump_range(start, end, output_function);
}

size_t logger_buffer_used(void) {
    uint64_t flags;
    bool owned = logger_acquire(&flags);
    uint64_t used = logger_buffer_head - logger_first_line();
    logger_release(flags, owned);
    return (size_t)used;
}

uint64_t logger_message_count(void) {
    return logger_messages;
}

uint64_t logger_dropped_bytes(void) {
    return logger_dropped;
}

void logger_enter_panic_mode(void) {
    logger_panic_mode = true;
    logger_resolve_outputs();
    if (logger_uart && logger_uart->is_present()) logger_uart->enter_panic_mode();
}

void logger_init(void) {
    uint64_t flags;
    bool owned = logger_acquire(&flags);
    logger_buffer_head = 0;
    logger_buffer_start = 0;
    logger_messages = 0;
    logger_dropped = 0;
    logger_uart_synced = false;
    logger_release(flags, owned);

    LOG_INFO("logger ready, %u KiB ring buffer, levels buffer=%s uart=%s video=%s",
             (unsigned)(LOGGER_BUFFER_SIZE / 1024),
             logger_level_name(logger_levels[LOGGER_OUTPUT_BUFFER]),
             logger_level_name(logger_levels[LOGGER_OUTPUT_UART]),
             logger_level_name(logger_levels[LOGGER_OUTPUT_VIDEO]));
}
