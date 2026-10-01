#ifndef UART_DRIVER_H
#define UART_DRIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UART_COM1_PORT 0x3F8
#define UART_COM2_PORT 0x2F8
#define UART_COM3_PORT 0x3E8
#define UART_COM4_PORT 0x2E8

#define UART_COM1_IRQ 4
#define UART_COM1_VECTOR 36

#define UART_REG_DATA 0
#define UART_REG_IER 1
#define UART_REG_IIR 2
#define UART_REG_FCR 2
#define UART_REG_LCR 3
#define UART_REG_MCR 4
#define UART_REG_LSR 5
#define UART_REG_MSR 6
#define UART_REG_SCRATCH 7

#define UART_LCR_8N1 0x03
#define UART_LCR_DLAB 0x80
#define UART_FCR_ENABLE_CLEAR_14 0xC7
#define UART_MCR_NORMAL 0x0F
#define UART_MCR_LOOPBACK 0x1E
#define UART_LSR_DATA_READY 0x01
#define UART_LSR_THR_EMPTY 0x20
#define UART_LSR_TX_IDLE 0x40
#define UART_IER_THR_EMPTY 0x02
#define UART_IIR_NO_INTERRUPT 0x01
#define UART_IIR_ID_MASK 0x0E
#define UART_IIR_MODEM_STATUS 0x00
#define UART_IIR_THR_EMPTY 0x02
#define UART_IIR_RX_DATA 0x04
#define UART_IIR_LINE_STATUS 0x06
#define UART_IIR_RX_TIMEOUT 0x0C
#define UART_IIR_FIFO_ENABLED 0xC0
#define UART_FIFO_SIZE_16550 16

#define UART_BASE_CLOCK 115200
#define UART_DEFAULT_BAUD 115200
#define UART_TX_TIMEOUT 100000
#define UART_TX_FAIL_LIMIT 4
#define UART_TX_BUFFER_SIZE (128 * 1024)
#define UART_BOOT_BUFFERED 1
#define UART_IRQ_MAX_LOOPS 16
#define UART_IRQ_TEST_TIMEOUT_MS 50
#define UART_IRQ_TEST_SPINS 2000000
#define UART_PANIC_LOCK_SPINS 1000000

enum UART_PORT {
    UART_COM1 = 0,
    UART_COM2 = 1,
    UART_COM3 = 2,
    UART_COM4 = 3,
};

enum uart_tx_mode {
    UART_TX_MODE_SYNC = 0,
    UART_TX_MODE_BUFFERED = 1,
    UART_TX_MODE_IRQ = 2,
};

struct uart_stats {
    enum uart_tx_mode mode;
    size_t queue_size;
    size_t pending;
    size_t peak_pending;
    uint8_t fifo_size;
    uint64_t queued_bytes;
    uint64_t sent_bytes;
    uint64_t irq_count;
    uint64_t stalls;
};

typedef struct uart_driver {
    bool (*is_present)(void);
    uint16_t (*get_io_base)(void);
    uint32_t (*get_baud_rate)(void);
    bool (*set_baud_rate)(uint32_t baud);
    void (*putc)(char c);
    void (*write)(const char *data, size_t length);
    void (*print)(const char *string);
    bool (*has_data)(void);
    char (*getc)(void);
    bool (*enable_irq)(void);
    void (*set_sync_mode)(void);
    void (*flush)(void);
    void (*enter_panic_mode)(void);
    void (*get_stats)(struct uart_stats *stats);
    const char *(*get_mode_name)(enum uart_tx_mode mode);
} uart_driver;

struct uart_driver *return_uart_driver(void);
struct driver *return_meta_uart_driver(void);

#endif
