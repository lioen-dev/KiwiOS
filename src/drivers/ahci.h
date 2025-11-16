#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <stdint.h>
#include <stdbool.h>

#include "drivers/pci.h"
#include "drivers/blockdev.h"

/* SATA signatures (PxSIG) */
#define SATA_SIG_ATA    0x00000101U
#define SATA_SIG_ATAPI  0xEB140101U
#define SATA_SIG_PM     0x96690101U
#define SATA_SIG_SEMB   0xC33C0101U

/* ---------- HBA register blocks ---------- */

typedef volatile struct hba_port {
    uint32_t clb;        /* 0x00: Command list base addr (low) 1K-aligned */
    uint32_t clbu;       /* 0x04: Command list base addr (high) */
    uint32_t fb;         /* 0x08: FIS base addr (low) 256B aligned */
    uint32_t fbu;        /* 0x0C: FIS base addr (high) */
    uint32_t is;         /* 0x10: Interrupt status */
    uint32_t ie;         /* 0x14: Interrupt enable */
    uint32_t cmd;        /* 0x18: Command and status */
    uint32_t rsv0;       /* 0x1C */
    uint32_t tfd;        /* 0x20: Task file data (status) */
    uint32_t sig;        /* 0x24: Signature */
    uint32_t ssts;       /* 0x28: SATA status (SCR0) */
    uint32_t sctl;       /* 0x2C: SATA control (SCR2) */
    uint32_t serr;       /* 0x30: SATA error (SCR1) */
    uint32_t sact;       /* 0x34: SATA active (NCQ) */
    uint32_t ci;         /* 0x38: Command issue */
    uint32_t sntf;       /* 0x3C: SATA notification */
    uint32_t fbs;        /* 0x40: FIS-based switching control */
    uint32_t rsv1[11];   /* 0x44–0x6F */
    uint32_t vendor[4];  /* 0x70–0x7F */
} hba_port_t;

typedef volatile struct hba_mem {
    uint32_t cap;        /* 0x00: Capabilities */
    uint32_t ghc;        /* 0x04: Global host control */
    uint32_t is;         /* 0x08: Interrupt status */
    uint32_t pi;         /* 0x0C: Ports implemented */
    uint32_t vs;         /* 0x10: Version */
    uint32_t ccc_ctl;    /* 0x14 */
    uint32_t ccc_pts;    /* 0x18 */
    uint32_t em_loc;     /* 0x1C */
    uint32_t em_ctl;     /* 0x20 */
    uint32_t cap2;       /* 0x24 */
    uint32_t bohc;       /* 0x28 */
    uint8_t  rsv[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[32];/* 0x100+ */
} hba_mem_t;

/* GHC bits */
#define AHCI_GHC_HR   (1u << 0)
#define AHCI_GHC_IE   (1u << 1)
#define AHCI_GHC_AE   (1u << 31)

/* PxCMD bits */
#define PxCMD_ST      (1u << 0)
#define PxCMD_SUD     (1u << 1)
#define PxCMD_POD     (1u << 2)
#define PxCMD_FRE     (1u << 4)
#define PxCMD_FR      (1u << 14)
#define PxCMD_CR      (1u << 15)

/* PxSCTL DET field */
#define PxSCTL_DET_MASK  0xF
#define PxSCTL_DET_INIT  0x1
#define PxSCTL_DET_NONE  0x0

/* PxSSTS helpers */
#define PxSSTS_DET(x)    ((x) & 0xF)
#define PxSSTS_IPM(x)    (((x) >> 8) & 0xF)

/* ---------- FIS / Command list / PRDT ---------- */

#define FIS_TYPE_REG_H2D 0x27

typedef struct __attribute__((packed)) {
    uint8_t fis_type;   /* 0x27 */
    uint8_t pmport:4;
    uint8_t rsv0:3;
    uint8_t c:1;        /* 1=command */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2;
    uint8_t device;
    uint8_t lba3, lba4, lba5;
    uint8_t featureh;
    uint8_t countl, counth;
    uint8_t icc, control;
    uint8_t rsv1[4];
} fis_reg_h2d_t;

typedef struct __attribute__((packed)) {
    /* DW0 (low 16 bits) */
    uint16_t cfl:5;
    uint16_t atapi:1;
    uint16_t write:1;      /* 1=host->device (WRITE), 0=device->host (READ) */
    uint16_t prefetch:1;
    uint16_t reset:1;
    uint16_t bist:1;
    uint16_t clear:1;
    uint16_t rsv0:1;
    uint16_t pmp:4;
    /* DW0 (high 16 bits) */
    uint16_t prdt_length;
    /* DW1 */
    volatile uint32_t prd_byte_count;
    /* DW2–DW3 */
    uint32_t ctba;
    uint32_t ctbau;
    /* DW4–DW7 */
    uint32_t rsv1[4];
} hba_cmd_header_t;

typedef struct __attribute__((packed)) {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;
    uint32_t dbc:22;    /* byte count (0-based) */
    uint32_t rsv1:9;
    uint32_t i:1;       /* interrupt on completion */
} hba_prdt_entry_t;

typedef struct __attribute__((packed)) {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  rsv[48];
    hba_prdt_entry_t prdt[128]; /* fits in one 4K page */
} hba_cmd_tbl_t;

/* ---------- Internal / public ---------- */

typedef struct ahci_device {
    hba_mem_t *abar;
    volatile hba_port_t *port;
    uint8_t port_num;
} ahci_dev_t;

/* API */
int ahci_init(void);

#endif // DRIVERS_AHCI_H
