#ifndef TREMOLO_H
#define TREMOLO_H

#include <stdint.h>
#include "xil_types.h"

// ============================================================================
// TREMOLO EFFECT CONFIGURATION
// ============================================================================

// Tremolo rate range (in units of 0.1 Hz, so 10 = 1.0 Hz)
#define TREMOLO_RATE_MIN          1     // 0.1 Hz minimum
#define TREMOLO_RATE_MAX          100   // 10.0 Hz maximum
#define TREMOLO_RATE_DEFAULT      10    // 1.0 Hz default

// Tremolo depth range (modulation amount)
#define TREMOLO_DEPTH_MIN         64    // 25% depth
#define TREMOLO_DEPTH_MAX         256   // 100% depth
#define TREMOLO_DEPTH_DEFAULT     128   // 75% depth default (more noticeable tremolo effect)

// Sample rate (Hz)
#define TREMOLO_SAMPLE_RATE       48000

// Sine table size (must be power of 2 for efficient wrapping)
#define TREMOLO_SINE_TABLE_SIZE   256

// ============================================================================
// TREMOLO STATE VARIABLES (extern for access from bsp.c)
// ============================================================================

extern volatile u8 tremolo_enabled;      // Effect enable flag
extern volatile u32 tremolo_rate;        // LFO rate (in 0.1 Hz units)
extern volatile u32 tremolo_depth;       // Modulation depth

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Process audio sample through tremolo effect
// Returns: modulated audio sample
int32_t process_tremolo(int32_t input);

// Update phase increment when tremolo rate changes
// Call this whenever tremolo_rate is modified
void update_tremolo_phase_inc(void);

// Initialize tremolo effect
void tremolo_init(void);

#endif // TREMOLO_H
