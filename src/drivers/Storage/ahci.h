#ifndef AHCI_H
#define AHCI_H

#include "ata.h"
#include "block_device.h"
#include "components/drivers.h"
#include "components/pci.h"
#include "kernel/sched/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

#define AHCI_MAX_PORTS 32
#define AHCI_MAX_DISKS 8
#define AHCI_CMD_SLOTS 32

#define HBA_PORT_SIZE 0x80

#define HBA_GHC_AE (1u << 31)
#define HBA_GHC_IE (1u << 1)
#define HBA_GHC_HR (1u << 0)

#define HBA_CMD_ST (1u << 0)
#define HBA_CMD_SUD (1u << 1)
#define HBA_CMD_POD (1u << 2)
#define HBA_CMD_FRE (1u << 4)
#define HBA_CMD_FR (1u << 14)
#define HBA_CMD_CR (1u << 15)

#define HBA_CAP_SSS (1u << 27)
#define HBA_CAP_SPM (1u << 17)

#define HBA_SCTL_DET_MASK 0x0000000Fu
#define HBA_SCTL_DET_COMRESET 0x1u
#define HBA_SCTL_IPM_MASK 0x00000F00u
#define HBA_SCTL_IPM_NO_LOWPOWER 0x00000300u

#define HBA_IS_TFES (1u << 30)
#define HBA_IS_HBFS (1u << 29)
#define HBA_IS_IFS (1u << 27)
#define HBA_IS_DHRS (1u << 0)

#define HBA_SSTS_DET_PRESENT 0x3

#define FIS_TYPE_REG_H2D 0x27

#define BLOCK_ERR_TIMEOUT -1
#define BLOCK_ERR_IO -2
#define BLOCK_OK 0
#define BLOCK_ERR_PARAM -3
#define BLOCK_ERR_UNSUPP -4

typedef struct {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed)) hba_port_t;

typedef struct {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t rsv[0xA0 - 0x2C];
    uint8_t vendor[0x100 - 0xA0];
    hba_port_t ports[32];
} __attribute__((packed)) hba_mem_t;

typedef struct {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc;
} __attribute__((packed)) hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[1];
} __attribute__((packed)) hba_cmd_table_t;

typedef struct {

    volatile hba_port_t *regs;
    hba_cmd_header_t *cmd_list;
    uint8_t *fis_buf;
    hba_cmd_table_t *cmd_tables[AHCI_CMD_SLOTS];
    struct block_device blkdev;
    bool present;
    uint64_t sector_count;
    uint32_t sector_size;
    spinlock_t lock;
} ahci_port_t;

typedef struct ahci_driver {
    int disk_count;
    ahci_port_t ports[AHCI_MAX_DISKS];

    int (*read)(int disk,
                uint64_t lba, uint32_t count, void *buf);

    int (*write)(int disk,
                 uint64_t lba, uint32_t count, void *buf);

    int (*get_disk_count)(void);
} ahci_driver;

struct ahci_driver *return_ahci_driver(void);
struct driver *return_meta_ahci_driver(void);

#endif
