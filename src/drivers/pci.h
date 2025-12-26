#ifndef DRIVERS_PCI_H
#define DRIVERS_PCI_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;
} pci_device_t;

// Raw config space access (Mechanism #1)
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

// Enumeration
bool     pci_read_device(uint8_t bus, uint8_t slot, uint8_t func, pci_device_t* out);
int      pci_enum_devices(pci_device_t* out_array, int max_out);

// BAR helpers
uint32_t pci_read_bar(uint8_t bus, uint8_t slot, uint8_t func, int bar_index,
                      bool* is_io, uint8_t* bar_size_bits);

void     pci_enable_bus_mastering(uint8_t bus, uint8_t slot, uint8_t func);
void     pci_enable_mmio_and_bus_mastering(uint8_t bus, uint8_t slot, uint8_t func);

// Convenience: find first device by class/subclass
static inline bool pci_find_class_subclass(uint8_t cls, uint8_t sub, pci_device_t* out) {
    pci_device_t tmp[256];
    int n = pci_enum_devices(tmp, 256);
    for (int i = 0; i < n; ++i) {
        if (tmp[i].class_code == cls && tmp[i].subclass == sub) {
            if (out) *out = tmp[i];
            return true;
        }
    }
    return false;
}

#endif // DRIVERS_PCI_H
