#include "panic.h"

#include "components/drivers.h"
#include "drivers/Video/limine_video_driver.h"

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
