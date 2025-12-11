#ifndef DELAY_H
#define DELAY_H

#include <stdint.h>
#include "xil_types.h"

// ============================================================================
// DELAY EFFECT CONFIGURATION
// ============================================================================

// Buffer size (shared with other effects that use delay)
#define BUFFER_SIZE 40000 // this buffer size is statically set by the programmers; size is not dynamic

// Delay range (in samples)
#define DELAY_SAMPLES_MIN 1000
#define DELAY_SAMPLES_MAX 38000
#define DELAY_SAMPLES_DEFAULT 8000 // this controls the default spacing between read/write head

// Adjustment step size
#define DELAY_ADJUST_STEP 1000

// Dry/wet mix ratios (0-256 scale)
#define WET_MIX 192    // 75% wet signal
#define DRY_MIX 62     // 24% dry signal

// ============================================================================
// DELAY STATE VARIABLES (extern for access from bsp.c)
// ============================================================================

extern volatile u8 delay_enabled;      // Effect enable flag
extern volatile u32 delay_samples;     // Delay time (samples)

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Process audio sample through delay effect
// Requires access to delay buffer
// Returns: processed audio sample (dry + wet mix)
int32_t process_delay(int32_t input, volatile u32* buffer, u32 buffer_size, u32 write_head);

// Initialize delay effect
void init_delay(void);

#endif // DELAY_H
