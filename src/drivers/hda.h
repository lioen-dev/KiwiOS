// src/drivers/hda.h

#ifndef DRIVERS_HDA_H
#define DRIVERS_HDA_H

#include <stdint.h>
#include <stdbool.h>

// Initialize Intel High Definition Audio controller (if present).
bool     hda_init(void);

// Check if controller was found.
bool     hda_is_present(void);

// Read cached controller registers.
uint16_t hda_get_gcap(void);
uint8_t  hda_get_version_major(void);
uint8_t  hda_get_version_minor(void);

// Did the controller complete reset successfully?
bool     hda_controller_was_reset(void);

// Codec presence info
bool     hda_has_codec(void);
uint8_t  hda_get_primary_codec_id(void);
uint16_t hda_get_codec_mask(void);

// CORB/RIRB DMA rings status
bool     hda_corb_rirb_ready(void);

// Query primary codec vendor ID using Immediate Command.
// Returns true on success and writes raw 32-bit response into *out_vendor.
//   top 16 bits: vendor ID
//   low  16 bits: device/subsystem ID
bool     hda_get_codec0_vendor_immediate(uint32_t* out_vendor);

// Query the sub-nodes of a parent node on the primary codec using GetParameter(NodeCount).
//   parent_nid: node to query (0 for root or an AFG).
//   out_start:  first child node ID
//   out_count:  number of child nodes
// Returns true on success.
bool     hda_codec0_get_sub_nodes(uint8_t parent_nid,
                                  uint8_t* out_start,
                                  uint8_t* out_count);

#endif // DRIVERS_HDA_H
