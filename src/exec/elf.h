#ifndef EXEC_ELF_H
#define EXEC_ELF_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "core/process.h"

// ELF64 header
typedef struct {
    uint8_t  e_ident[16];     // Magic number and other info
    uint16_t e_type;          // Object file type
    uint16_t e_machine;       // Architecture
    uint32_t e_version;       // Object file version
    uint64_t e_entry;         // Entry point virtual address
    uint64_t e_phoff;         // Program header table file offset
    uint64_t e_shoff;         // Section header table file offset
    uint32_t e_flags;         // Processor-specific flags
    uint16_t e_ehsize;        // ELF header size
    uint16_t e_phentsize;     // Program header table entry size
    uint16_t e_phnum;         // Program header table entry count
    uint16_t e_shentsize;     // Section header table entry size
    uint16_t e_shnum;         // Section header table entry count
    uint16_t e_shstrndx;      // Section header string table index
} __attribute__((packed)) elf64_header_t;

// Program header
typedef struct {
    uint32_t p_type;          // Segment type
    uint32_t p_flags;         // Segment flags
    uint64_t p_offset;        // Segment file offset
    uint64_t p_vaddr;         // Segment virtual address
    uint64_t p_paddr;         // Segment physical address
    uint64_t p_filesz;        // Segment size in file
    uint64_t p_memsz;         // Segment size in memory
    uint64_t p_align;         // Segment alignment
} __attribute__((packed)) elf64_program_header_t;

// ELF identification indices
#define EI_MAG0       0
#define EI_MAG1       1
#define EI_MAG2       2
#define EI_MAG3       3
#define EI_CLASS      4
#define EI_DATA       5
#define EI_VERSION    6

// Magic numbers
#define ELFMAG0       0x7f
#define ELFMAG1       'E'
#define ELFMAG2       'L'
#define ELFMAG3       'F'
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EV_CURRENT    1

// Architecture
#define EM_X86_64     62

// Object file types
#define ET_NONE       0
#define ET_REL        1
#define ET_EXEC       2
#define ET_DYN        3

// Segment types
#define PT_NULL       0
#define PT_LOAD       1
#define PT_DYNAMIC    2
#define PT_INTERP     3
#define PT_GNU_STACK  0x6474e551

// Segment flags
#define PF_X          0x1
#define PF_W          0x2
#define PF_R          0x4

// Validate an ELF file
bool elf_validate(void* elf_data, size_t size);

// Load an ELF file and create a process
// Returns NULL on failure
process_t* elf_load(const char* name, void* elf_data, size_t size);
process_t* elf_load_with_args(const char* name, void* elf_data, size_t size,
                              int argc, const char** argv);

#endif // EXEC_ELF_H
