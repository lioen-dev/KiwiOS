#ifndef DRIVERS_ACPI_H
#define DRIVERS_ACPI_H

#include <stdint.h>

// Initialize ACPI (discovers FADT reset register if present)
void acpi_init(void);

// Power off the machine (tries ACPI/QEMU methods, then HLT loop).
__attribute__((noreturn)) void acpi_poweroff(void);

// Reboot the machine (tries ACPI reset register, then 0xCF9, KBC, triple fault).
__attribute__((noreturn)) void acpi_reboot(void);

#endif // DRIVERS_ACPI_H
