#include <stdint.h>
#include <stdbool.h>
#include "arch/x86/io.h"
#include "drivers/pci.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static inline uint32_t pci_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    return 0x80000000U
        | ((uint32_t)bus  << 16)
        | ((uint32_t)slot << 11)
        | ((uint32_t)func << 8)
        | (offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t v = pci_config_read32(bus, slot, func, offset & 0xFC);
    uint8_t shift = (offset & 2) * 8;
    return (uint16_t)((v >> shift) & 0xFFFF);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset)
{
    uint32_t v = pci_config_read32(bus, slot, func, offset & 0xFC);
    uint8_t shift = (offset & 3) * 8;
    return (uint8_t)((v >> shift) & 0xFF);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t val)
{
    outl(PCI_CONFIG_ADDRESS, pci_addr(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, val);
}

void pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    uint32_t address = (uint32_t)(
        (1 << 31) |
        (bus << 16) |
        (slot << 11) |
        (func << 8) |
        (offset & 0xFC)
    );
    outl(0xCF8, address);
    outw(0xCFC + (offset & 2), value);
}

bool pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t* out)
{
    uint16_t vendor = pci_config_read16(bus, slot, func, 0x00);
    if (vendor == 0xFFFF) return false;

    out->bus  = bus;
    out->slot = slot;
    out->func = func;
    out->vendor_id = vendor;
    out->device_id = pci_config_read16(bus, slot, func, 0x02);

    out->class_code = pci_config_read8(bus, slot, func, 0x0B);
    out->subclass   = pci_config_read8(bus, slot, func, 0x0A);
    out->prog_if    = pci_config_read8(bus, slot, func, 0x09);
    out->header_type= pci_config_read8(bus, slot, func, 0x0E);
    return true;
}

int pci_enum_devices(pci_device_t* out_array, int max_out)
{
    int written = 0;

    for (int bus = 0; bus < 256; ++bus) {
        for (int slot = 0; slot < 32; ++slot) {
            uint16_t vendor0 = pci_config_read16((uint8_t)bus, (uint8_t)slot, 0, 0x00);
            if (vendor0 == 0xFFFF) continue;

            uint8_t hdr = pci_config_read8((uint8_t)bus, (uint8_t)slot, 0, 0x0E);
            bool multi = (hdr & 0x80) != 0;
            int funcs = multi ? 8 : 1;

            for (int func = 0; func < funcs; ++func) {
                pci_device_t dev;
                if (!pci_read_device((uint8_t)bus, (uint8_t)slot, (uint8_t)func, &dev)) continue;
                if (written < max_out) out_array[written] = dev;
                ++written;
            }
        }
    }
    return (written < max_out) ? written : max_out;
}

// Read BARn and (optionally) determine size (as log2) via write-all-ones probe.
// NOTE: Safe for listing; do not call while device is actively used / DMA running.
uint32_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func, int bar_index,
                      bool* is_io, uint8_t* bar_size_bits)
{
    if (bar_index < 0 || bar_index > 5) return 0;
    uint8_t off = (uint8_t)(0x10 + bar_index * 4);
    uint32_t bar = pci_config_read32(bus, slot, func, off);

    if (is_io) *is_io = (bar & 0x1) != 0;

    if (bar_size_bits) {
        // Probe size
        pci_config_write32(bus, slot, func, off, 0xFFFFFFFF);
        uint32_t mask = pci_config_read32(bus, slot, func, off);
        pci_config_write32(bus, slot, func, off, bar);

        if (bar & 1) mask &= ~0x3U;   // I/O BAR mask
        else         mask &= ~0xFU;   // MMIO BAR mask

        uint32_t size = (~mask) + 1;
        uint8_t bits = 0;
        while (size > 1) { size >>= 1; bits++; }
        *bar_size_bits = bits;
    }
    return bar;
}

void pci_enable_bus_mastering(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint16_t cmd = pci_config_read16(bus, slot, func, 0x04);
    cmd |= (1 << 2); // Bus Master Enable
    uint32_t cur = pci_config_read32(bus, slot, func, 0x04);
    cur = (cur & 0xFFFF0000u) | cmd;
    pci_config_write32(bus, slot, func, 0x04, cur);
}

void pci_enable_mmio_and_bus_mastering(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t cmd32 = pci_config_read32(bus, slot, func, 0x04);
    uint16_t cmd = (uint16_t)(cmd32 & 0xFFFF);
    cmd |= (1 << 1); // Memory Space enable
    cmd |= (1 << 2); // Bus Master
    cmd32 = (cmd32 & 0xFFFF0000u) | cmd;
    pci_config_write32(bus, slot, func, 0x04, cmd32);
}
