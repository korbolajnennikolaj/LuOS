#ifndef NVME_H
#define NVME_H

#include "block_device.h"
#include "components/drivers.h"
#include "components/pci.h"
#include "kernel/scheduler/spinlock.h"

#include <stdbool.h>
#include <stdint.h>

#define NVME_MAX_CONTROLLERS 16
#define NVME_MAX_DISKS 32

#define NVME_IO_QUEUE_ID 1
#define NVME_ADMIN_QUEUE_ID 0

#define NVME_ADMIN_QUEUE_ENTRIES 32
#define NVME_IO_QUEUE_ENTRIES 32

#define NVME_SQE_SIZE 64
#define NVME_CQE_SIZE 16

#define BLOCK_ERR_TIMEOUT -1
#define BLOCK_ERR_IO -2
#define BLOCK_OK 0
#define BLOCK_ERR_PARAM -3
#define BLOCK_ERR_UNSUPP -4

typedef struct {
    uint64_t cap;
    uint32_t vs;
    uint32_t intms;
    uint32_t intmc;
    uint32_t cc;
    uint32_t rsv0;
    uint32_t csts;
    uint32_t nssr;
    uint32_t aqa;
    uint64_t asq;
    uint64_t acq;
    uint8_t rsv1[0x1000 - 0x38];
} __attribute__((packed)) nvme_regs_t;

#define NVME_CAP_MQES(cap) ((uint32_t)((cap) & 0xFFFF) + 1)
#define NVME_CAP_DSTRD(cap) ((uint32_t)(((cap) >> 32) & 0xF))
#define NVME_CAP_TO(cap) ((uint32_t)(((cap) >> 24) & 0xFF))
#define NVME_CAP_CSS_NVM(cap) (((cap) >> 37) & 0x1)

#define NVME_CC_EN (1u << 0)
#define NVME_CC_CSS_NVM (0u << 4)
#define NVME_CC_MPS_4K (0u << 7)
#define NVME_CC_AMS_RR (0u << 11)
#define NVME_CC_SHN_NONE (0u << 14)
#define NVME_CC_IOSQES(bits) (((uint32_t)(bits)) << 16)
#define NVME_CC_IOCQES(bits) (((uint32_t)(bits)) << 20)

#define NVME_CSTS_RDY (1u << 0)
#define NVME_CSTS_CFS (1u << 1)

typedef struct {
    uint32_t cdw0;
    uint32_t nsid;
    uint32_t cdw2;
    uint32_t cdw3;
    uint64_t mptr;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

typedef struct {
    uint32_t dw0;
    uint32_t dw1;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) nvme_cqe_t;

#define NVME_CQE_PHASE(cqe) ((cqe)->status & 1)
#define NVME_CQE_SC(cqe) (((cqe)->status >> 1) & 0xFF)
#define NVME_CQE_SCT(cqe) (((cqe)->status >> 9) & 0x7)

#define NVME_ADMIN_DELETE_SQ 0x00
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_DELETE_CQ 0x04
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY 0x06

#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ 0x02

#define NVME_IDENTIFY_CNS_NAMESPACE 0x00
#define NVME_IDENTIFY_CNS_CONTROLLER 0x01

typedef struct {
    nvme_sqe_t *sq;
    nvme_cqe_t *cq;
    uint32_t sq_entries;
    uint32_t cq_entries;
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t expected_phase;
    volatile uint32_t *sq_doorbell;
    volatile uint32_t *cq_doorbell;
    uint16_t next_cid;
    spinlock_t lock;
} nvme_queue_t;

typedef struct {
    struct block_device blkdev;
    bool present;
    uint32_t nsid;
    uint64_t sector_count;
    uint32_t sector_size;
    struct nvme_controller *ctrl;
} nvme_namespace_t;

typedef struct nvme_controller {

    volatile nvme_regs_t *regs;
    uint32_t dstrd;
    nvme_queue_t admin_q;
    nvme_queue_t io_q;
    nvme_namespace_t namespaces[NVME_MAX_DISKS];
    int namespace_count;
    bool present;
} nvme_controller_t;

typedef struct nvme_driver {
    int disk_count;
    nvme_controller_t controllers[NVME_MAX_CONTROLLERS];
    int controller_count;

    int (*read)(int disk, uint64_t lba, uint32_t count, void *buf);
    int (*write)(int disk, uint64_t lba, uint32_t count, void *buf);
    int (*get_disk_count)(void);
} nvme_driver;

struct nvme_driver *return_nvme_driver(void);
struct driver *return_meta_nvme_driver(void);

#endif
