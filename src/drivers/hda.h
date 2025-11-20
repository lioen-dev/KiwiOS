// src/drivers/hda.h

#ifndef DRIVERS_HDA_H
#define DRIVERS_HDA_H

#include <stdint.h>
#include <stdbool.h>

// Initialize Intel High Definition Audio controller (if present).
bool     hda_init(void);

// Power state constants for widgets/codecs.
#define HDA_POWER_STATE_D0 0x00
#define HDA_POWER_STATE_D3 0x03

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

// Generic parameter query helper for the primary codec (uses CORB if available, falls back to immediate).
bool     hda_codec0_get_parameter(uint8_t nid, uint16_t parameter, uint32_t* out_resp);

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

// Request a power state change for a codec node and optionally report the resulting state (lower 4 bits of response).
bool     hda_codec0_set_power_state(uint8_t nid, uint8_t target_state, uint8_t* out_state);

#endif // DRIVERS_HDA_H
