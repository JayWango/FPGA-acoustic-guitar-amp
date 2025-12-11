#ifndef CHORUS_H
#define CHORUS_H

#include <stdint.h>
#include "xil_types.h"

// ============================================================================
// CHORUS EFFECT CONFIGURATION
// ============================================================================

// Chorus modulation rate range (in units of 0.1 Hz, so 10 = 1.0 Hz)
#define CHORUS_RATE_MIN          1     // 0.1 Hz minimum
#define CHORUS_RATE_MAX          50    // 5.0 Hz maximum
#define CHORUS_RATE_DEFAULT      25    // 1.0 Hz default

// Chorus delay range (base delay in samples)
#define CHORUS_DELAY_MIN         200   // ~2ms at 48kHz
#define CHORUS_DELAY_MAX         1500  // ~31ms at 48kHz
#define CHORUS_DELAY_DEFAULT     1000   // ~10ms at 48kHz default // 800

// Chorus modulation depth (delay variation in samples)
#define CHORUS_DEPTH_MIN         20    // 0.2ms modulation
#define CHORUS_DEPTH_MAX         300   // 4ms modulation
#define CHORUS_DEPTH_DEFAULT     150    // 1ms modulation default // 120

// Adjust step sizes for encoder
#define CHORUS_DELAY_ADJUST_STEP 50
#define CHORUS_DEPTH_ADJUST_STEP 10

// Sample rate (Hz)
#define CHORUS_SAMPLE_RATE       48828 // match system sample rate

// Sine table size (must be power of 2 for efficient wrapping)
// Note: Uses shared sine table from tremolo.h (TREMOLO_SINE_TABLE_SIZE)
#define CHORUS_SINE_TABLE_SIZE   256   // Must match TREMOLO_SINE_TABLE_SIZE

// Dry/wet mix ratios (0-256 scale)
#define CHORUS_DRY_MIX           128    // 75% dry signal
#define CHORUS_WET_MIX           128    // 25% wet signal

// ============================================================================
// CHORUS STATE VARIABLES (extern for access from bsp.c)
// ============================================================================

extern volatile u8 chorus_enabled;      // Effect enable flag
extern volatile u32 chorus_rate;        // LFO rate (in 0.1 Hz units)
extern volatile u32 chorus_delay;       // Base delay (samples)
extern volatile u32 chorus_depth;       // Modulation depth (samples)
extern volatile u8 chorus_adjust_mode;	// 0 = rate, 1 = delay, 2 = depth

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================

// Process audio sample through chorus effect
// Requires access to delay buffer
// Returns: processed audio sample
int32_t process_chorus(int32_t input, volatile u32* buffer, u32 buffer_size, u32 write_head);

// Update phase increment when chorus rate changes
// Call this whenever chorus_rate is modified
void update_chorus_phase_inc(void);

// Initialize chorus effect
void init_chorus(void);

#endif // CHORUS_H
