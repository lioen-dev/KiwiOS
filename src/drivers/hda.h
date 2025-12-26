#ifndef DRIVERS_HDA_H
#define DRIVERS_HDA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Basic HDA structures derived from https://github.com/inclementine/intelHDA
struct hda_bdl_entry {
    uint32_t paddr;
    uint32_t paddr_high;
    uint32_t length;
    uint32_t flags;
};

struct hda_output {
    uint8_t  codec;
    uint16_t node_id;
    uint16_t pin_node_id;
    uint32_t sample_rate;
    int      amp_gain_steps;
    int      num_channels;
};

typedef struct hda_device {
    struct hda_output output;

    uint32_t  mmio_size;
    uint8_t  *mmio_base;

    uint32_t *buffer;
    size_t    buffer_size;
    volatile uint32_t            *corb;   ///< Command Outbound Ring Buffer
    volatile uint32_t            *rirb;   ///< Response Inbound Ring Buffer
    volatile struct hda_bdl_entry* bdl;    ///< Buffer Descriptor List
    volatile uint32_t            *dma_pos; ///< DMA Position in Current Buffer

    int16_t  *pcm_queue;           ///< Software PCM queue (interleaved int16)
    size_t    pcm_queue_capacity;  ///< Capacity in frames
    size_t    pcm_queue_head;      ///< Read position (in samples)
    size_t    pcm_queue_tail;      ///< Write position (in samples)
    size_t    pcm_queue_samples;   ///< Samples currently queued

    uint32_t  corb_entries;   ///< Number of CORB entries
    uint32_t  rirb_entries;   ///< Number of RIRB entries
    uint16_t  rirb_read_pointer; ///< RIRB Read Pointer

    int       buffers_completed;

    size_t    bdl_entry_size;
    size_t    bdl_entries;
    size_t    current_bdl_index;

    // IRQ / interrupt wiring info
    uint8_t   irq_line;          // value read from PCI config space (0x3C)
    uint8_t   irq_vector;        // IDT vector used for legacy PIC interrupt (if any)
    bool      irq_legacy_routed; // true if we successfully routed via 8259 PIC
} hda_device;

// Initialize the Intel High Definition Audio controller (best-effort).
void hda_init(void);

// Write interleaved PCM samples (int16, signed) into the active buffer.
size_t HDA_write_interleaved_pcm(const int16_t* samples, size_t frames);

// Enqueue PCM frames into the software queue.
size_t HDA_enqueue_interleaved_pcm(const int16_t* samples, size_t frames);

// Query the active output channel count.
size_t HDA_output_channels(void);

#endif // DRIVERS_HDA_H
