#ifndef DRIVERS_HDA_H
#define DRIVERS_HDA_H

#include <stdint.h>
#include <stddef.h>

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

    uint32_t  corb_entries;   ///< Number of CORB entries
    uint32_t  rirb_entries;   ///< Number of RIRB entries
    uint16_t  rirb_read_pointer; ///< RIRB Read Pointer

    int       buffers_completed;
} hda_device;

// Initialize the Intel High Definition Audio controller (best-effort).
void hda_init(void);

// Write interleaved PCM samples (int16, signed) into the active buffer.
void HDA_write_interleaved_pcm(const int16_t* samples, size_t frames);

#endif // DRIVERS_HDA_H
