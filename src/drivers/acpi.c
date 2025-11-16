#include "drivers/acpi.h"
#include "limine.h"
#include "arch/x86/io.h"
#include "memory/vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------- Limine RSDP request ----------------
static volatile struct limine_rsdp_request RSDP_REQ = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

// ---------------- ACPI table structs -----------------
#pragma pack(push,1)
typedef struct {
    char     Signature[4];
    uint32_t Length;
    uint8_t  Revision;
    uint8_t  Checksum;
    char     OemId[6];
    char     OemTableId[8];
    uint32_t OemRevision;
    uint32_t CreatorId;
    uint32_t CreatorRevision;
} acpi_sdt_header_t;

typedef struct {
    uint8_t  SpaceId;     // 0=SystemMemory, 1=SystemIO
    uint8_t  BitWidth;
    uint8_t  BitOffset;
    uint8_t  AccessSize;  // 1=byte,2=word,3=dword,4=qword
    uint64_t Address;     // physical address or I/O port / MMIO addr
} acpi_gas_t;

// Minimal FADT we need (adds X_* PM blocks and SleepControl/Status for HW-reduced)
typedef struct {
    acpi_sdt_header_t h;
    uint32_t FirmwareCtrl;
    uint32_t Dsdt;
    uint8_t  Reserved;

    uint8_t  PreferredPMProfile;
    uint16_t SCI_Interrupt;
    uint32_t SMI_CommandPort;
    uint8_t  AcpiEnable;
    uint8_t  AcpiDisable;
    uint8_t  S4BIOS_REQ;
    uint8_t  PSTATE_Control;

    uint32_t PM1a_EVT_BLK;
    uint32_t PM1b_EVT_BLK;
    uint32_t PM1a_CNT_BLK;
    uint32_t PM1b_CNT_BLK;
    uint32_t PM2_CNT_BLK;
    uint32_t PM_TMR_BLK;
    uint32_t GPE0_BLK;
    uint32_t GPE1_BLK;

    uint8_t  PM1_EVT_LEN;
    uint8_t  PM1_CNT_LEN;
    uint8_t  PM2_CNT_LEN;
    uint8_t  PM_TMR_LEN;
    uint8_t  GPE0_LEN;
    uint8_t  GPE1_LEN;
    uint8_t  GPE1_BASE;
    uint8_t  CST_CNT;

    uint16_t P_LVL2_LAT;
    uint16_t P_LVL3_LAT;
    uint16_t FlushSize;
    uint16_t FlushStride;
    uint8_t  DutyOffset;
    uint8_t  DutyWidth;
    uint8_t  DayAlarm;
    uint8_t  MonAlarm;
    uint8_t  Century;

    uint16_t IAPC_BOOT_ARCH;
    uint8_t  Reserved2;
    uint32_t Flags;

    // ACPI 2.0+ Reset
    acpi_gas_t ResetReg;
    uint8_t    ResetValue;
    uint8_t    __pad[3];

    uint64_t   X_FirmwareCtrl;
    uint64_t   X_Dsdt;

    // Extended fixed blocks
    uint64_t   X_PM1a_EVT_BLK;
    uint64_t   X_PM1b_EVT_BLK;
    uint64_t   X_PM1a_CNT_BLK;
    uint64_t   X_PM1b_CNT_BLK;
    uint64_t   X_PM2_CNT_BLK;
    uint64_t   X_PM_TMR_BLK;
    uint64_t   X_GPE0_BLK;
    uint64_t   X_GPE1_BLK;

    // HW-reduced ACPI sleep registers (ACPI 5.0+)
    acpi_gas_t SleepControlReg;
    acpi_gas_t SleepStatusReg;
} fadt_t;
#pragma pack(pop)

// FADT Flags bit for HW-reduced ACPI
#define FADT_HW_REDUCED_ACPI  (1u << 20)

// ---------------- Local state -----------------------
static bool       g_have_reset = false;
static acpi_gas_t g_reset_reg;
static uint8_t    g_reset_val = 0;

static uint16_t   g_pm1a_cnt = 0, g_pm1b_cnt = 0;
static uint8_t    g_pm1_cnt_len = 0;         // 2 or 4
static uint8_t    g_s5_typa = 0, g_s5_typb = 0;
static bool       g_have_s5 = false;

static uint32_t   g_smi_cmd = 0;
static uint8_t    g_acpi_enable = 0;
static bool       g_tried_enable = false;

// HW-reduced ACPI
static bool       g_hw_reduced = false;
static acpi_gas_t g_sleep_ctrl = {0}, g_sleep_status = {0};

// ---------------- Helpers ---------------------------
static inline uint8_t checksum8(const void* buf, size_t len) {
    const uint8_t* p = (const uint8_t*)buf; uint8_t s = 0;
    for (size_t i = 0; i < len; i++) s += p[i];
    return s;
}
static bool sdt_is(const acpi_sdt_header_t* h, const char sig[4]) {
    return h && h->Signature[0]==sig[0] && h->Signature[1]==sig[1] &&
           h->Signature[2]==sig[2] && h->Signature[3]==sig[3];
}

// Tiny AML scan for _S5_ (pull two BytePrefix values)
static void parse__S5_from_dsdt(void* dsdt_phys) {
    if (!dsdt_phys) return;
    acpi_sdt_header_t* dsdt = (acpi_sdt_header_t*)phys_to_virt((uint64_t)dsdt_phys);
    if (!dsdt || checksum8(dsdt, dsdt->Length) != 0 || !sdt_is(dsdt, "DSDT")) return;

    uint8_t* p = (uint8_t*)dsdt;
    uint8_t* end = p + dsdt->Length;

    for (uint8_t* cur = p; cur + 4 < end; cur++) {
        if (cur[0] != 0x5F || cur[1] != 0x53 || cur[2] != 0x35 || cur[3] != 0x5F) continue; // "_S5_"
        for (uint8_t* r = cur + 4; r < end && r < cur + 64; r++) {
            if (*r == 0x12 /*PackageOp*/) {
                r++;
                if (r >= end) break;
                // AML PkgLen
                uint32_t pkg_len = 0;
                uint8_t lead = *r++;
                if ((lead & 0x80) == 0) { pkg_len = lead; }
                else {
                    uint8_t bytes = (lead >> 6) + 1;
                    pkg_len = (lead & 0x0F);
                    for (uint8_t i=0;i<bytes-1 && r<end;i++) pkg_len |= (*r++) << (4 + 8*i);
                }
                if (r >= end) break;

                // element count (skip)
                if (r >= end) break;
                r++;

                // two BytePrefix values
                if (r+1 < end && r[0]==0x0A) { g_s5_typa = r[1]; r += 2; }
                if (r+1 < end && r[0]==0x0A) { g_s5_typb = r[1]; }
                g_have_s5 = true;
                return;
            }
        }
    }
}

static void discover_fadt(const acpi_sdt_header_t* xsdt_or_rsdt, bool is_xsdt) {
    if (!xsdt_or_rsdt) return;
    size_t n = (xsdt_or_rsdt->Length - sizeof(acpi_sdt_header_t)) / (is_xsdt ? 8 : 4);

    for (size_t i = 0; i < n; i++) {
        uint64_t addr = is_xsdt
            ? ((const uint64_t*)((const uint8_t*)xsdt_or_rsdt + sizeof(*xsdt_or_rsdt)))[i]
            : ((const uint32_t*)((const uint8_t*)xsdt_or_rsdt + sizeof(*xsdt_or_rsdt)))[i];

        acpi_sdt_header_t* h = (acpi_sdt_header_t*)phys_to_virt(addr);
        if (!h || checksum8(h, h->Length) != 0) continue;

        if (sdt_is(h, "FACP")) {
            fadt_t* fadt = (fadt_t*)h;

            // Reset register for reboot
            if (fadt->ResetReg.Address) {
                g_have_reset = true;
                g_reset_reg  = fadt->ResetReg;
                g_reset_val  = fadt->ResetValue;
            }

            // Prefer extended PM1 control blocks; else legacy
            uint64_t pm1a = fadt->X_PM1a_CNT_BLK ? fadt->X_PM1a_CNT_BLK : (uint64_t)fadt->PM1a_CNT_BLK;
            uint64_t pm1b = fadt->X_PM1b_CNT_BLK ? fadt->X_PM1b_CNT_BLK : (uint64_t)fadt->PM1b_CNT_BLK;

            g_pm1a_cnt    = (uint16_t)pm1a;
            g_pm1b_cnt    = (uint16_t)pm1b;
            g_pm1_cnt_len = fadt->PM1_CNT_LEN ? fadt->PM1_CNT_LEN : 4;

            g_smi_cmd     = fadt->SMI_CommandPort;
            g_acpi_enable = fadt->AcpiEnable;

            // HW-reduced ACPI path (SleepControl/Status)
            g_hw_reduced  = (fadt->Flags & FADT_HW_REDUCED_ACPI) != 0;
            g_sleep_ctrl  = fadt->SleepControlReg;
            g_sleep_status= fadt->SleepStatusReg;

            uint64_t dsdt_phys = fadt->X_Dsdt ? fadt->X_Dsdt : (uint64_t)fadt->Dsdt;
            parse__S5_from_dsdt((void*)dsdt_phys);
            return;
        }
    }
}

static void maybe_enable_acpi_and_wait_sci(void) {
    if (g_tried_enable) return;
    g_tried_enable = true;

    if (g_smi_cmd && g_acpi_enable) {
        outb((uint16_t)g_smi_cmd, g_acpi_enable);
        // Wait briefly for SCI_EN (bit 0) in PM1a_CNT
        if (g_pm1a_cnt && g_pm1_cnt_len >= 2) {
            for (int i = 0; i < 500000; i++) {
                if (g_pm1_cnt_len >= 4) { if (inl(g_pm1a_cnt) & 1u) break; }
                else { if (inw(g_pm1a_cnt) & 1u) break; }
            }
        }
    }
}

// MMIO write helper for Generic Address Structure
static void gas_write(const acpi_gas_t* gas, uint64_t value) {
    if (!gas || !gas->Address) return;
    if (gas->SpaceId == 1 /*SystemIO*/) {
        uint16_t port = (uint16_t)gas->Address;
        switch (gas->BitWidth) {
            case 8:  outb(port,  (uint8_t)value);  break;
            case 16: outw(port,  (uint16_t)value); break;
            case 32: outl(port,  (uint32_t)value); break;
            default: outl(port,  (uint32_t)value); break;
        }
    } else if (gas->SpaceId == 0 /*SystemMemory*/) {
        volatile uint8_t* p = (volatile uint8_t*)phys_to_virt(gas->Address);
        switch (gas->BitWidth) {
            case 8:  *(volatile uint8_t*)p  = (uint8_t)value;  break;
            case 16: *(volatile uint16_t*)p = (uint16_t)value; break;
            case 32: *(volatile uint32_t*)p = (uint32_t)value; break;
            case 64: *(volatile uint64_t*)p = (uint64_t)value; break;
            default: *(volatile uint32_t*)p = (uint32_t)value; break;
        }
    }
}

// ---------------- Public: init/reboot/shutdown ------
void acpi_init(void) {
    g_have_reset = false; g_have_s5 = false;
    g_pm1a_cnt = g_pm1b_cnt = 0; g_pm1_cnt_len = 0;
    g_s5_typa = g_s5_typb = 0;
    g_smi_cmd = 0; g_acpi_enable = 0; g_tried_enable = false;
    g_hw_reduced = false; g_sleep_ctrl.Address = 0; g_sleep_status.Address = 0;

    if (!RSDP_REQ.response) return;

#if LIMINE_API_REVISION >= 1
    uint64_t rsdp_phys = RSDP_REQ.response->address;
#else
    void* rsdp_phys_ptr = RSDP_REQ.response->address;
    uint64_t rsdp_phys = (uint64_t)rsdp_phys_ptr;
#endif
    if (!rsdp_phys) return;

    struct {
        char     Signature[8];
        uint8_t  Checksum;
        char     OemId[6];
        uint8_t  Revision;
        uint32_t RsdtAddress;
        uint32_t Length;
        uint64_t XsdtAddress;
        uint8_t  ExtendedChecksum;
        uint8_t  Reserved[3];
    } __attribute__((packed)) *rsdp = (void*)phys_to_virt(rsdp_phys);

    bool has_xsdt = rsdp->Revision >= 2 && rsdp->XsdtAddress;
    if (has_xsdt) {
        acpi_sdt_header_t* xsdt = (acpi_sdt_header_t*)phys_to_virt(rsdp->XsdtAddress);
        if (xsdt && checksum8(xsdt, xsdt->Length) == 0 && sdt_is(xsdt, "XSDT")) { discover_fadt(xsdt, true); return; }
    }
    acpi_sdt_header_t* rsdt = (acpi_sdt_header_t*)phys_to_virt((uint64_t)rsdp->RsdtAddress);
    if (rsdt && checksum8(rsdt, rsdt->Length) == 0 && sdt_is(rsdt, "RSDT")) { discover_fadt(rsdt, false); }
}

__attribute__((noreturn)) void acpi_reboot(void) {
    asm volatile ("cli");

    // 1) ACPI FADT Reset Register
    if (g_have_reset && g_reset_reg.Address && g_reset_reg.SpaceId == 1 /*SystemIO*/) {
        uint16_t port = (uint16_t)g_reset_reg.Address;
        switch (g_reset_reg.AccessSize) {
            case 1: outb(port, g_reset_val); break;
            case 2: outw(port, (uint16_t)g_reset_val); break;
            case 3: outl(port, (uint32_t)g_reset_val); break;
            default: outb(port, g_reset_val); break;
        }
    }

    // 2) Chipset reset control
    outb(0xCF9, 0x02); outb(0xCF9, 0x06);

    // 3) Legacy KBC
    outb(0x64, 0xFE);

    // 4) Triple fault
    asm volatile("lidt (%0); int $3" : : "r"((uint64_t[3]){0,0,0}) : "memory");
    for (;;) asm volatile ("hlt");
}

// Map 3-bit SLP_TYP to HW-reduced SleepControl value per spec:
// bits: [3]=SLP_EN, [2:0]=Sx type (this is a simplified mapping used by many firmware)
static inline uint32_t hw_reduced_sleep_control_value(uint8_t slp_typ) {
    return (1u << 3) | (slp_typ & 0x7);
}

__attribute__((noreturn)) void acpi_poweroff(void) {
    asm volatile ("cli");

    // HW-reduced ACPI path (use SleepControlReg if present)
    if (g_hw_reduced && g_sleep_ctrl.Address && g_have_s5) {
        // (OSDev/spec: write sleep type with enable in the sleep control reg)
        gas_write(&g_sleep_ctrl, hw_reduced_sleep_control_value(g_s5_typa));
        for(volatile int i=0;i<2000000;i++){ __asm__ __volatile__ ("" ::: "memory"); }
    }

    // Classic ACPI: PM1 control blocks
    if (g_have_s5 && g_pm1a_cnt && g_pm1_cnt_len >= 2) {
        maybe_enable_acpi_and_wait_sci();

        const uint32_t SLP_EN = (1u << 13);
        const uint32_t SLP_TYP_SHIFT = 10;
        const uint32_t TYP_A = ((uint32_t)(g_s5_typa & 7)) << SLP_TYP_SHIFT;
        const uint32_t TYP_B = ((uint32_t)(g_s5_typb & 7)) << SLP_TYP_SHIFT;

        if (g_pm1_cnt_len >= 4) {
            // Program TYP on A/B (no EN), then set EN only on A
            uint32_t v = inl(g_pm1a_cnt);
            v = (v & ~(7u << SLP_TYP_SHIFT)) | TYP_A; outl(g_pm1a_cnt, v);
            if (g_pm1b_cnt) { uint32_t vb = inl(g_pm1b_cnt); vb = (vb & ~(7u << SLP_TYP_SHIFT)) | TYP_B; outl(g_pm1b_cnt, vb); }
            outl(g_pm1a_cnt, inl(g_pm1a_cnt) | SLP_EN);
        } else {
            uint16_t v = inw(g_pm1a_cnt);
            v = (uint16_t)((v & (uint16_t)~((uint16_t)7 << SLP_TYP_SHIFT)) | (uint16_t)TYP_A);
            outw(g_pm1a_cnt, v);
            if (g_pm1b_cnt) {
                uint16_t vb = inw(g_pm1b_cnt);
                vb = (uint16_t)((vb & (uint16_t)~((uint16_t)7 << SLP_TYP_SHIFT)) | (uint16_t)TYP_B);
                outw(g_pm1b_cnt, vb);
            }
            outw(g_pm1a_cnt, (uint16_t)(inw(g_pm1a_cnt) | (uint16_t)SLP_EN));
        }

        for (volatile int i=0;i<2000000;i++) { __asm__ __volatile__ ("" ::: "memory"); }
    }

    // Legacy fallbacks
    outw(0xB004, 0x2000);
    outw(0x604,  0x2000);

    for (;;) asm volatile ("hlt");
}