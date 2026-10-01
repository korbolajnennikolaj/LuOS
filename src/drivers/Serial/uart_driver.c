#include "uart_driver.h"

#include "components/drivers.h"
#include "components/Interruptions/isr.h"
#include "components/logger.h"
#include "drivers/Timer/timer.h"
#include "kernel/scheduler/spinlock.h"

#include <ports.h>
#include <stddef.h>

static spinlock_t uart_lock = SPINLOCK_INIT;

static uint16_t uart_io_base = UART_COM1_PORT;
static uint32_t uart_baud_rate = UART_DEFAULT_BAUD;
static volatile bool uart_present = false;
static uint32_t uart_tx_failures = 0;
static uint8_t uart_fifo_size = 1;

static char uart_tx_buffer[UART_TX_BUFFER_SIZE];
static volatile uint64_t uart_tx_head = 0;
static volatile uint64_t uart_tx_tail = 0;
static volatile enum uart_tx_mode uart_mode = UART_BOOT_BUFFERED ? UART_TX_MODE_BUFFERED : UART_TX_MODE_SYNC;
static bool uart_tx_armed = false;
static volatile bool uart_panic = false;

static size_t uart_tx_peak = 0;
static uint64_t uart_tx_queued = 0;
static uint64_t uart_tx_sent = 0;
static volatile uint64_t uart_irq_count = 0;
static uint64_t uart_tx_stalls = 0;

static inline void uart_out(uint16_t reg, uint8_t value) {
    outb((uint16_t)(uart_io_base + reg), value);
}

static inline uint8_t uart_in(uint16_t reg) {
    return inb((uint16_t)(uart_io_base + reg));
}

static bool uart_acquire(uint64_t *flags) {
    asm volatile("pushfq; pop %0" : "=r"(*flags));
    asm volatile("cli");

    if (!uart_panic) {
        spin_lock(&uart_lock);
        return true;
    }

    for (uint32_t i = 0; i < UART_PANIC_LOCK_SPINS; i++) {
        if (spin_trylock(&uart_lock)) return true;
        asm volatile("pause");
    }
    return false;
}

static void uart_release(uint64_t flags, bool owned) {
    if (owned) spin_unlock(&uart_lock);
    if (flags & (1 << 9))
        asm volatile("sti");
}

static void uart_program_divisor(uint32_t baud) {
    uint16_t divisor = (uint16_t)(UART_BASE_CLOCK / baud);
    if (divisor == 0) divisor = 1;

    uart_out(UART_REG_LCR, UART_LCR_DLAB);
    uart_out(UART_REG_DATA, (uint8_t)(divisor & 0xFF));
    uart_out(UART_REG_IER, (uint8_t)(divisor >> 8));
    uart_out(UART_REG_LCR, UART_LCR_8N1);
}

static bool uart_probe(void) {
    uart_out(UART_REG_SCRATCH, 0x5A);
    if (uart_in(UART_REG_SCRATCH) != 0x5A) return false;
    uart_out(UART_REG_SCRATCH, 0xA5);
    if (uart_in(UART_REG_SCRATCH) != 0xA5) return false;

    uart_out(UART_REG_IER, 0x00);
    uart_program_divisor(uart_baud_rate);
    uart_out(UART_REG_FCR, UART_FCR_ENABLE_CLEAR_14);
    uart_fifo_size = ((uart_in(UART_REG_IIR) & UART_IIR_FIFO_ENABLED) == UART_IIR_FIFO_ENABLED)
                   ? UART_FIFO_SIZE_16550 : 1;

    uart_out(UART_REG_MCR, UART_MCR_LOOPBACK);
    uart_out(UART_REG_DATA, 0xAE);

    bool echoed = false;
    for (int i = 0; i < 1000; i++) {
        if (uart_in(UART_REG_LSR) & UART_LSR_DATA_READY) {
            echoed = (uart_in(UART_REG_DATA) == 0xAE);
            break;
        }
    }

    uart_out(UART_REG_MCR, UART_MCR_NORMAL);
    return echoed;
}

static bool uart_wait_tx(void) {
    for (uint32_t i = 0; i < UART_TX_TIMEOUT; i++) {
        if (uart_in(UART_REG_LSR) & UART_LSR_THR_EMPTY) return true;
        asm volatile("pause");
    }
    return false;
}

static void uart_wait_idle(void) {
    for (uint32_t i = 0; i < UART_TX_TIMEOUT; i++) {
        if (uart_in(UART_REG_LSR) & UART_LSR_TX_IDLE) return;
        asm volatile("pause");
    }
}

static inline size_t uart_tx_pending(void) {
    return (size_t)(uart_tx_head - uart_tx_tail);
}

static void uart_tx_arm(bool armed) {
    if (uart_tx_armed == armed) return;
    uart_tx_armed = armed;
    uart_out(UART_REG_IER, armed ? UART_IER_THR_EMPTY : 0);
}

static void uart_tx_fail(void) {
    if (++uart_tx_failures < UART_TX_FAIL_LIMIT) return;
    uart_present = false;
    uart_tx_tail = uart_tx_head;
    uart_tx_arm(false);
}

static void uart_tx_fill_fifo(void) {
    uint8_t count = 0;
    while (count < uart_fifo_size && uart_tx_tail != uart_tx_head) {
        uart_out(UART_REG_DATA, (uint8_t)uart_tx_buffer[uart_tx_tail % UART_TX_BUFFER_SIZE]);
        uart_tx_tail++;
        count++;
    }
    uart_tx_sent += count;
}

static void uart_tx_pump(void) {
    if (!uart_present || uart_tx_tail == uart_tx_head) return;
    if (uart_in(UART_REG_LSR) & UART_LSR_THR_EMPTY) uart_tx_fill_fifo();
}

static void uart_tx_drain_step(void) {
    if (!uart_wait_tx()) {
        uart_tx_fail();
        return;
    }
    uart_tx_failures = 0;
    uart_tx_fill_fifo();
}

static void uart_tx_drain(void) {
    while (uart_present && uart_tx_tail != uart_tx_head)
        uart_tx_drain_step();
}

static void uart_tx_make_room(size_t need) {
    bool stalled = false;
    while (uart_present && UART_TX_BUFFER_SIZE - uart_tx_pending() < need) {
        if (!stalled) {
            uart_tx_stalls++;
            stalled = true;
        }
        uart_tx_drain_step();
    }
}

static inline void uart_tx_push(char c) {
    uart_tx_buffer[uart_tx_head % UART_TX_BUFFER_SIZE] = c;
    uart_tx_head++;
    uart_tx_queued++;
}

static void uart_tx_queue(const char *data, size_t length) {
    for (size_t i = 0; i < length && uart_present; i++) {
        if (UART_TX_BUFFER_SIZE - uart_tx_pending() < 2) {
            uart_tx_make_room(2);
            if (!uart_present) break;
        }
        if (data[i] == '\n') uart_tx_push('\r');
        uart_tx_push(data[i]);
    }

    size_t pending = uart_tx_pending();
    if (pending > uart_tx_peak) uart_tx_peak = pending;
}

static void uart_putc_raw(char c) {
    if (!uart_present) return;
    if (!uart_wait_tx()) {
        uart_tx_fail();
        return;
    }
    uart_tx_failures = 0;
    uart_out(UART_REG_DATA, (uint8_t)c);
    uart_tx_queued++;
    uart_tx_sent++;
}

static void uart_write_sync(const char *data, size_t length) {
    uart_tx_drain();
    for (size_t i = 0; i < length && uart_present; i++) {
        if (data[i] == '\n') uart_putc_raw('\r');
        uart_putc_raw(data[i]);
    }
}

static void uart_write(const char *data, size_t length) {
    if (!data || !uart_present) return;

    uint64_t flags;
    bool owned = uart_acquire(&flags);

    if (uart_mode == UART_TX_MODE_SYNC) {
        uart_write_sync(data, length);
    } else {
        uart_tx_queue(data, length);
        uart_tx_pump();
        if (uart_mode == UART_TX_MODE_IRQ) uart_tx_arm(uart_tx_tail != uart_tx_head);
    }

    uart_release(flags, owned);
}

static void uart_putc(char c) {
    uart_write(&c, 1);
}

static void uart_print(const char *string) {
    if (!string) return;
    size_t length = 0;
    while (string[length]) length++;
    uart_write(string, length);
}

static bool uart_has_data(void) {
    if (!uart_present) return false;
    return (uart_in(UART_REG_LSR) & UART_LSR_DATA_READY) != 0;
}

static char uart_getc(void) {
    if (!uart_has_data()) return 0;
    return (char)uart_in(UART_REG_DATA);
}

static bool uart_is_present(void) {
    return uart_present;
}

static uint16_t uart_get_io_base(void) {
    return uart_io_base;
}

static uint32_t uart_get_baud_rate(void) {
    return uart_baud_rate;
}

static bool uart_set_baud_rate(uint32_t baud) {
    if (baud == 0 || baud > UART_BASE_CLOCK || (UART_BASE_CLOCK % baud) != 0) return false;

    uint64_t flags;
    bool owned = uart_acquire(&flags);
    uart_baud_rate = baud;
    if (uart_present) {
        uart_tx_drain();
        uart_wait_idle();
        uart_program_divisor(baud);
    }
    uart_release(flags, owned);
    return true;
}

static void uart_irq_handler(struct registers *regs) {
    (void)regs;

    spin_lock(&uart_lock);
    uart_irq_count++;

    bool done = false;
    for (int i = 0; i < UART_IRQ_MAX_LOOPS && !done; i++) {
        uint8_t iir = uart_in(UART_REG_IIR);
        if (iir & UART_IIR_NO_INTERRUPT) break;

        switch (iir & UART_IIR_ID_MASK) {
            case UART_IIR_THR_EMPTY:
                uart_tx_fill_fifo();
                break;
            case UART_IIR_LINE_STATUS:
                (void)uart_in(UART_REG_LSR);
                break;
            case UART_IIR_MODEM_STATUS:
                (void)uart_in(UART_REG_MSR);
                break;
            default:
                done = true;
                break;
        }
    }

    if (uart_tx_tail == uart_tx_head || uart_mode != UART_TX_MODE_IRQ) uart_tx_arm(false);
    spin_unlock(&uart_lock);
}

static bool uart_irq_wait(uint64_t before) {
    struct tsc_driver *tsc = (struct tsc_driver *)get_self_driver(TIMER_DRIVER, TSC_TIMER);

    if (tsc && tsc->get_tsc_uptime_ms) {
        uint64_t deadline = tsc->get_tsc_uptime_ms() + UART_IRQ_TEST_TIMEOUT_MS;
        while (uart_irq_count == before && tsc->get_tsc_uptime_ms() < deadline)
            asm volatile("pause");
    } else {
        for (uint32_t i = 0; i < UART_IRQ_TEST_SPINS && uart_irq_count == before; i++)
            asm volatile("pause");
    }

    return uart_irq_count != before;
}

static void uart_set_sync_mode(void) {
    uint64_t flags;
    bool owned = uart_acquire(&flags);
    uart_mode = UART_TX_MODE_SYNC;
    if (uart_present) {
        uart_tx_arm(false);
        uart_tx_drain();
    }
    uart_release(flags, owned);
}

static bool uart_enable_irq(void) {
    if (!uart_present) return false;

    uint64_t rflags;
    asm volatile("pushfq; pop %0" : "=r"(rflags));
    if (!(rflags & (1 << 9))) {
        uart_set_sync_mode();
        LOG_WARNING("interrupts are disabled, COM1 output stays synchronous");
        return false;
    }

    irq_register_handler(UART_COM1_VECTOR, uart_irq_handler);

    uint64_t flags = spin_lock_irqsave(&uart_lock);
    uint64_t before = uart_irq_count;
    size_t pending = uart_tx_pending();
    uart_mode = UART_TX_MODE_IRQ;
    uart_tx_pump();
    uart_tx_arm(true);
    spin_unlock_irqrestore(&uart_lock, flags);

    if (!uart_irq_wait(before)) {
        uart_set_sync_mode();
        irq_unregister_handler(UART_COM1_VECTOR);
        LOG_WARNING("COM1 IRQ%u did not fire within %u ms, falling back to synchronous output",
                    UART_COM1_IRQ, UART_IRQ_TEST_TIMEOUT_MS);
        return false;
    }

    LOG_INFO("COM1 output is interrupt driven, IRQ%u vector 0x%x, %u KiB queue, %u byte FIFO, %llu byte(s) queued at boot",
             UART_COM1_IRQ, UART_COM1_VECTOR, (unsigned)(UART_TX_BUFFER_SIZE / 1024),
             (unsigned)uart_fifo_size, (unsigned long long)pending);
    return true;
}

static void uart_flush(void) {
    uint64_t flags;
    bool owned = uart_acquire(&flags);
    uart_tx_drain();
    uart_release(flags, owned);
}

static void uart_enter_panic_mode(void) {
    uart_panic = true;
    uart_set_sync_mode();
}

static void uart_get_stats(struct uart_stats *stats) {
    if (!stats) return;

    uint64_t flags;
    bool owned = uart_acquire(&flags);
    stats->mode = uart_mode;
    stats->queue_size = UART_TX_BUFFER_SIZE;
    stats->pending = uart_tx_pending();
    stats->peak_pending = uart_tx_peak;
    stats->fifo_size = uart_fifo_size;
    stats->queued_bytes = uart_tx_queued;
    stats->sent_bytes = uart_tx_sent;
    stats->irq_count = uart_irq_count;
    stats->stalls = uart_tx_stalls;
    uart_release(flags, owned);
}

static const char *uart_get_mode_name(enum uart_tx_mode mode) {
    switch (mode) {
        case UART_TX_MODE_SYNC: return "sync";
        case UART_TX_MODE_BUFFERED: return "buffered";
        case UART_TX_MODE_IRQ: return "irq";
        default: return "unknown";
    }
}

struct uart_driver uart_driver_loaded = {
    .is_present = uart_is_present,
    .get_io_base = uart_get_io_base,
    .get_baud_rate = uart_get_baud_rate,
    .set_baud_rate = uart_set_baud_rate,
    .putc = uart_putc,
    .write = uart_write,
    .print = uart_print,
    .has_data = uart_has_data,
    .getc = uart_getc,
    .enable_irq = uart_enable_irq,
    .set_sync_mode = uart_set_sync_mode,
    .flush = uart_flush,
    .enter_panic_mode = uart_enter_panic_mode,
    .get_stats = uart_get_stats,
    .get_mode_name = uart_get_mode_name,
};

struct uart_driver *return_uart_driver(void) {
    uint64_t flags = spin_lock_irqsave(&uart_lock);
    uart_tx_failures = 0;
    uart_tx_head = 0;
    uart_tx_tail = 0;
    uart_tx_armed = false;
    uart_mode = UART_BOOT_BUFFERED ? UART_TX_MODE_BUFFERED : UART_TX_MODE_SYNC;
    uart_present = uart_probe();
    spin_unlock_irqrestore(&uart_lock, flags);

    if (uart_present)
        LOG_INFO("COM1 at io 0x%x ready, %u baud 8N1, %u byte FIFO, output %s until IRQ%u is enabled",
                 uart_io_base, uart_baud_rate, (unsigned)uart_fifo_size,
                 uart_get_mode_name(uart_mode), UART_COM1_IRQ);
    else
        LOG_WARNING("COM1 at io 0x%x not responding, serial log disabled", uart_io_base);

    return &uart_driver_loaded;
}

struct driver *return_meta_uart_driver(void) {
    static struct driver meta = {
        .name = "UART Driver",
        .type = SERIAL_DRIVER,
        .sub_type = UART_COM1,
        .status = DRIVER_STATUS_UNINITIALIZED,
        .dependencies = {NULL},
        .dependency_count = 0,
        .self = &uart_driver_loaded,
        .init = (void*)return_uart_driver
    };

    return &meta;
}
