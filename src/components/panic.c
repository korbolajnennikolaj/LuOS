#include "panic.h"

#include "components/drivers.h"
#include "drivers/Video/gop_font.h"
#include "drivers/Video/limine_video_driver.h"
#include "kernel/scheduler/scheduler.h"

#include <stdarg.h>
#include <stdio.h>

#define PANIC_SCREEN_BASE_ROWS 44
#define PANIC_SCREEN_LEGEND_ROWS 9

static char panic_log_tail[PANIC_MAX_LOG_PRINT_SIZE * LOGGER_LINE_SIZE + 1];
static size_t panic_log_tail_length = 0;

static void panic_collect_log(const char *message, size_t length) {
    for (size_t i = 0; i < length && panic_log_tail_length < sizeof(panic_log_tail) - 1; i++) {
        panic_log_tail[panic_log_tail_length++] = message[i];
    }
    panic_log_tail[panic_log_tail_length] = '\0';
}

static void panic_log(const char *format, ...) {
    static struct logger_message_t message;
    const char *caller = "panic";

    va_list args;
    va_start(args, format);
    int n = vsnprintf(message.message, sizeof(message.message), format, args);
    va_end(args);

    message.length = (n < 0) ? 0 : (size_t)n;
    message.caller_name_length = 0;
    while (caller[message.caller_name_length]) {
        message.caller_name[message.caller_name_length] = caller[message.caller_name_length];
        message.caller_name_length++;
    }
    message.caller_name[message.caller_name_length] = '\0';
    message.level = LOGGER_LEVEL_ERROR;
    message.use_uart = true;
    message.use_limine_video = false;

    logger_log(&message);
}

static const char *panic_code_name(enum panic_code code) {
    switch (code) {
        case PANIC_CODE_GENERAL: return "GENERAL";
        case PANIC_CODE_PAGE_FAULT: return "PAGE FAULT";
        case PANIC_CODE_DOUBLE_FAULT: return "DOUBLE FAULT";
        case PANIC_CODE_INVALID_OPCODE: return "INVALID OPCODE";
        case PANIC_CODE_STACK_OVERFLOW: return "STACK OVERFLOW";
        default: return "UNKNOWN";
    }
}

static void panic_log_report(struct panic_info *info) {
    struct task *t = current_task();

    panic_log("KERNEL PANIC: %s - %s", panic_code_name(info->code), info->message ? info->message : "(no message)");
    panic_log("task %s, core %d", t ? t->name : "(no task)", current_core());

    if (!info->regs) return;

    cpu_regs_t *r = info->regs;
    panic_log("rax=0x%016llx rbx=0x%016llx rcx=0x%016llx rdx=0x%016llx",
              (unsigned long long)r->rax, (unsigned long long)r->rbx,
              (unsigned long long)r->rcx, (unsigned long long)r->rdx);
    panic_log("rsi=0x%016llx rdi=0x%016llx rbp=0x%016llx rsp=0x%016llx",
              (unsigned long long)r->rsi, (unsigned long long)r->rdi,
              (unsigned long long)r->rbp, (unsigned long long)r->rsp);
    panic_log("r8 =0x%016llx r9 =0x%016llx r10=0x%016llx r11=0x%016llx",
              (unsigned long long)r->r8, (unsigned long long)r->r9,
              (unsigned long long)r->r10, (unsigned long long)r->r11);
    panic_log("r12=0x%016llx r13=0x%016llx r14=0x%016llx r15=0x%016llx",
              (unsigned long long)r->r12, (unsigned long long)r->r13,
              (unsigned long long)r->r14, (unsigned long long)r->r15);
    panic_log("rip=0x%016llx rflags=0x%016llx cr2=0x%016llx cr3=0x%016llx",
              (unsigned long long)r->rip, (unsigned long long)r->rflags,
              (unsigned long long)r->cr2, (unsigned long long)r->cr3);
}

static size_t panic_log_tail_lines(void) {
    size_t lines = 0;
    for (size_t i = 0; i < panic_log_tail_length; i++) {
        if (panic_log_tail[i] == '\n') lines++;
    }
    if (panic_log_tail_length > 0 && panic_log_tail[panic_log_tail_length - 1] != '\n') lines++;
    return lines;
}

static void print_log_tail(
    struct limine_video_driver *video_driver,
    size_t max_lines,
    size_t max_columns
) {
    if (panic_log_tail_length == 0 || max_lines == 0) {
        video_driver->printf(
            "  (log buffer is empty)\n",
            LIMINE_COLOR_DARK_GRAY
        );
        return;
    }

    size_t total_lines = panic_log_tail_lines();
    size_t skip = (total_lines > max_lines) ? total_lines - max_lines : 0;

    char *line = panic_log_tail;
    while (*line) {
        char *end = line;
        while (*end && *end != '\n') end++;

        char saved = *end;
        char *cut = end;
        if (max_columns > 0 && (size_t)(end - line) > max_columns) cut = line + max_columns;
        char saved_cut = *cut;

        if (skip > 0) {
            skip--;
        } else {
            *cut = '\0';

            video_driver->printf(
                "  ",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                line,
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            *cut = saved_cut;
        }

        line = saved ? end + 1 : end;
    }
}

static void print_hex64(
    struct limine_video_driver *video_driver,
    uint64_t value,
    uint32_t color
) {
    const char *hex = "0123456789ABCDEF";
    char buf[19];

    buf[0] = '0';
    buf[1] = 'x';

    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(value >> (60 - i * 4)) & 0xF];
    }

    buf[18] = '\0';

    video_driver->printf(buf, color);
}

static void print_reg_name(
    struct limine_video_driver *video_driver,
    const char *name
) {
    video_driver->printf(name, LIMINE_COLOR_YELLOW);

    int len = 0;

    while (name[len] != '\0') {
        len++;
    }

    while (len < 6) {
        video_driver->printf(" ", LIMINE_COLOR_YELLOW);
        len++;
    }
}

static void print_reg_pair(
    struct limine_video_driver *video_driver,

    const char *name1,
    uint64_t value1,

    const char *name2,
    uint64_t value2
) {
    video_driver->printf(
        "  ",
        LIMINE_COLOR_LIGHT_GRAY
    );

    print_reg_name(
        video_driver,
        name1
    );

    video_driver->printf(
        "  ",
        LIMINE_COLOR_LIGHT_GRAY
    );

    print_hex64(
        video_driver,
        value1,
        LIMINE_COLOR_WHITE
    );

    video_driver->printf(
        "    ",
        LIMINE_COLOR_LIGHT_GRAY
    );

    print_reg_name(
        video_driver,
        name2
    );

    video_driver->printf(
        "  ",
        LIMINE_COLOR_LIGHT_GRAY
    );

    print_hex64(
        video_driver,
        value2,
        LIMINE_COLOR_WHITE
    );

    video_driver->printf(
        "\n",
        LIMINE_COLOR_LIGHT_GRAY
    );
}

void panic(struct panic_info *info) {
    asm volatile("cli");

    logger_enter_panic_mode();

    panic_log_tail_length = 0;
    panic_log_tail[0] = '\0';
    logger_dump_last_to_text(PANIC_MAX_LOG_PRINT_SIZE, panic_collect_log);

    panic_log_report(info);

    struct limine_video_driver *video_driver =
        get_self_driver(LIMINE_VIDEO_DRIVER, 0);

    if (video_driver) {

        video_driver->clear(
            LIMINE_COLOR_BLACK
        );

        uint64_t width;
        uint64_t height;

        video_driver->get_display_resolution(
            &width,
            &height
        );

        size_t screen_rows = (size_t)(height / FONT_HEIGHT);
        size_t screen_columns = (size_t)(width / FONT_WIDTH);
        size_t tail_lines = panic_log_tail_lines();
        if (tail_lines == 0) tail_lines = 1;

        bool show_register_info =
            screen_rows >= PANIC_SCREEN_BASE_ROWS + PANIC_SCREEN_LEGEND_ROWS + tail_lines;

        size_t tail_rows = (screen_rows > PANIC_SCREEN_BASE_ROWS)
            ? screen_rows - PANIC_SCREEN_BASE_ROWS
            : 1;
        if (show_register_info) tail_rows -= PANIC_SCREEN_LEGEND_ROWS;

        video_driver->printf(
            "\n",
            LIMINE_COLOR_LIGHT_GRAY
        );

        video_driver->printf(
            "================================================================\n",
            LIMINE_COLOR_LIGHT_RED
        );

        video_driver->printf(
            "                              LuOS",
            LIMINE_COLOR_YELLOW
        );

        video_driver->printf(
            "\n",
            LIMINE_COLOR_LIGHT_RED
        );

        video_driver->printf(
            "                         KERNEL PANIC\n",
            LIMINE_COLOR_LIGHT_RED
        );

        video_driver->printf(
            "================================================================\n",
            LIMINE_COLOR_LIGHT_RED
        );

        video_driver->printf(
            "\n",
            LIMINE_COLOR_LIGHT_GRAY
        );

        video_driver->printf(
            "  PANIC INFORMATION\n",
            LIMINE_COLOR_YELLOW
        );

        video_driver->printf(
            "  ----------------------------------------------------------------\n",
            LIMINE_COLOR_LIGHT_GRAY
        );

        video_driver->printf(
            "  Code    : ",
            LIMINE_COLOR_LIGHT_GRAY
        );

        switch (info->code) {

            case PANIC_CODE_GENERAL:
                video_driver->printf(
                    "GENERAL",
                    LIMINE_COLOR_LIGHT_RED
                );
                break;

            case PANIC_CODE_PAGE_FAULT:
                video_driver->printf(
                    "PAGE FAULT",
                    LIMINE_COLOR_LIGHT_RED
                );
                break;

            case PANIC_CODE_DOUBLE_FAULT:
                video_driver->printf(
                    "DOUBLE FAULT",
                    LIMINE_COLOR_LIGHT_RED
                );
                break;

            case PANIC_CODE_INVALID_OPCODE:
                video_driver->printf(
                    "INVALID OPCODE",
                    LIMINE_COLOR_LIGHT_RED
                );
                break;

            case PANIC_CODE_STACK_OVERFLOW:
                video_driver->printf(
                    "STACK OVERFLOW",
                    LIMINE_COLOR_LIGHT_RED
                );
                break;

            default:
                video_driver->printf(
                    "UNKNOWN",
                    LIMINE_COLOR_LIGHT_RED
                );
                break;
        }

        video_driver->printf(
            "\n",
            LIMINE_COLOR_LIGHT_GRAY
        );

        video_driver->printf(
            "  Reason  : ",
            LIMINE_COLOR_LIGHT_GRAY
        );

        video_driver->printf(
            info->message,
            LIMINE_COLOR_WHITE
        );

        {
            char line[96];
            int n = 0;
            const char *pfx = "\n  Task    : ";
            while (*pfx && n < 90) line[n++] = *pfx++;
            struct task *t = current_task();
            const char *nm = t ? t->name : "(no task)";
            for (int i = 0; nm[i] && i < TASK_NAME_MAX && n < 70; i++) line[n++] = nm[i];
            const char *cs = "  core ";
            while (*cs && n < 90) line[n++] = *cs++;
            int c = current_core();
            if (c >= 10 && n < 90) line[n++] = (char)('0' + (c / 10) % 10);
            if (n < 90) line[n++] = (char)('0' + c % 10);
            line[n] = 0;
            video_driver->printf(line, LIMINE_COLOR_LIGHT_GRAY);
        }

        video_driver->printf(
            "\n\n",
            LIMINE_COLOR_LIGHT_GRAY
        );

        if (info->regs) {

            video_driver->printf(
                "  CPU REGISTERS\n",
                LIMINE_COLOR_YELLOW
            );

            video_driver->printf(
                "  ----------------------------------------------------------------\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  Register  Value                  Register  Value\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  ----------------------------------------------------------------\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            print_reg_pair(
                video_driver,
                "rax",
                info->regs->rax,
                "rbx",
                info->regs->rbx
            );

            print_reg_pair(
                video_driver,
                "rcx",
                info->regs->rcx,
                "rdx",
                info->regs->rdx
            );

            print_reg_pair(
                video_driver,
                "rsi",
                info->regs->rsi,
                "rdi",
                info->regs->rdi
            );

            print_reg_pair(
                video_driver,
                "rbp",
                info->regs->rbp,
                "rsp",
                info->regs->rsp
            );

            print_reg_pair(
                video_driver,
                "r8",
                info->regs->r8,
                "r9",
                info->regs->r9
            );

            print_reg_pair(
                video_driver,
                "r10",
                info->regs->r10,
                "r11",
                info->regs->r11
            );

            print_reg_pair(
                video_driver,
                "r12",
                info->regs->r12,
                "r13",
                info->regs->r13
            );

            print_reg_pair(
                video_driver,
                "r14",
                info->regs->r14,
                "r15",
                info->regs->r15
            );

            video_driver->printf(
                "\n"
                "  CPU STATE\n",
                LIMINE_COLOR_YELLOW
            );

            video_driver->printf(
                "  ----------------------------------------------------------------\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            print_reg_pair(
                video_driver,
                "rip",
                info->regs->rip,
                "rflags",
                info->regs->rflags
            );

            print_reg_pair(
                video_driver,
                "cr2",
                info->regs->cr2,
                "cr3",
                info->regs->cr3
            );

        }

        if (info->regs && show_register_info) {

            video_driver->printf(
                "\n"
                "  REGISTER INFO\n",
                LIMINE_COLOR_YELLOW
            );

            video_driver->printf(
                "  ----------------------------------------------------------------\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  RAX-R15  General purpose registers\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  RIP      Instruction pointer\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  RFLAGS   CPU status and control flags\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  RSP      Current stack pointer\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  RBP      Current stack frame pointer\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  CR2      Virtual address that caused page fault\n",
                LIMINE_COLOR_LIGHT_GRAY
            );

            video_driver->printf(
                "  CR3      Current page table root\n",
                LIMINE_COLOR_LIGHT_GRAY
            );
        }

        video_driver->printf(
            "\n"
            "  LAST LOG MESSAGES\n",
            LIMINE_COLOR_YELLOW
        );

        video_driver->printf(
            "  ----------------------------------------------------------------\n",
            LIMINE_COLOR_LIGHT_GRAY
        );

        print_log_tail(
            video_driver,
            tail_rows,
            screen_columns > 3 ? screen_columns - 3 : 0
        );

        video_driver->printf(
            "\n"
            "================================================================\n",
            LIMINE_COLOR_LIGHT_RED
        );

        video_driver->printf(
            "                         LuOS HAS CRASHED\n",
            LIMINE_COLOR_LIGHT_RED
        );

        video_driver->printf(
            "================================================================\n",
            LIMINE_COLOR_LIGHT_RED
        );

        video_driver->printf(
            "\n"
            "  The kernel encountered a fatal error and cannot continue.\n",
            LIMINE_COLOR_LIGHT_GRAY
        );

        video_driver->printf(
            "  Please restart your computer.\n",
            LIMINE_COLOR_WHITE
        );

        video_driver->printf(
            "\n"
            "  System halted. [x_x]\n",
            LIMINE_COLOR_LIGHT_RED
        );
    }

    while (1) {
        asm volatile("hlt");
    }
}
