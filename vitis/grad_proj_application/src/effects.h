#ifndef EFFECTS_H
#define EFFECTS_H

#include <stdint.h>
#include "xil_types.h"

// ============================================================================
// EFFECT STATE VARIABLES
// ============================================================================

// Effect enable flags (one per button)
extern volatile u8 chorus_enabled;
extern volatile u8 tremolo_enabled;
extern volatile u8 bass_boost_enabled;
extern volatile u8 reverb_enabled;

// ============================================================================
// CHORUS EFFECT (Button 1)
// ============================================================================
// Short delay (5-30ms) with LFO modulation
#define CHORUS_DELAY_MIN_SAMPLES  240   // ~5ms at 48kHz
#define CHORUS_DELAY_MAX_SAMPLES  1440  // ~30ms at 48kHz
#define CHORUS_DELAY_DEFAULT      480   // ~10ms default
#define CHORUS_DELAY_STEP         48    // ~1ms per encoder step

#define CHORUS_LFO_RATE_MIN       1     // ~0.1 Hz (slow)
#define CHORUS_LFO_RATE_MAX       50    // ~5 Hz (fast)
#define CHORUS_LFO_RATE_DEFAULT   10    // ~1 Hz default

#define CHORUS_DEPTH_DEFAULT      32    // Modulation depth (0-64, fixed-point *256)

extern volatile u32 chorus_delay_samples;      // Current delay time
extern volatile u32 chorus_lfo_rate;          // LFO rate (0.1-5 Hz)
extern volatile u32 chorus_lfo_phase;         // LFO phase accumulator
extern volatile u32 chorus_delay_buffer[1440]; // Delay buffer (max 30ms)
extern volatile u32 chorus_write_head;         // Write head for chorus buffer

// ============================================================================
// TREMOLO EFFECT (Button 2)
// ============================================================================
// Volume modulation with LFO
#define TREMOLO_RATE_MIN          5     // ~0.5 Hz
#define TREMOLO_RATE_MAX          100   // ~10 Hz
#define TREMOLO_RATE_DEFAULT      20    // ~2 Hz default

#define TREMOLO_DEPTH_MIN         64    // 25% depth
#define TREMOLO_DEPTH_MAX         192   // 75% depth
#define TREMOLO_DEPTH_DEFAULT     128   // 50% depth default

extern volatile u32 tremolo_rate;      // LFO rate
extern volatile u32 tremolo_depth;     // Modulation depth
extern volatile u32 tremolo_lfo_phase; // LFO phase accumulator
extern volatile u32 tremolo_phase_inc; // Pre-calculated phase increment

// ============================================================================
// BASS BOOST EFFECT (Button 3)
// ============================================================================
// Frequency-selective bass enhancement
#define BASS_BOOST_FREQ_MIN       50    // ~50 Hz cutoff
#define BASS_BOOST_FREQ_MAX       200   // ~200 Hz cutoff
#define BASS_BOOST_FREQ_DEFAULT   100   // ~100 Hz default

#define BASS_BOOST_GAIN_MIN       128   // +0 dB (no boost)
#define BASS_BOOST_GAIN_MAX       256   // +6 dB boost
#define BASS_BOOST_GAIN_DEFAULT   192   // +3 dB default

extern volatile u32 bass_boost_gain;   // Boost amount
extern volatile int32_t bass_boost_state_low;  // Low-pass filter state
extern volatile int32_t bass_boost_state_high; // High-pass filter state

// ============================================================================
// REVERB EFFECT (Button 4)
// ============================================================================
// Multiple delay taps for room simulation
#define REVERB_TAP_COUNT          4     // Number of delay taps
#define REVERB_DELAY_SAMPLES_1    480   // ~10ms
#define REVERB_DELAY_SAMPLES_2    960   // ~20ms
#define REVERB_DELAY_SAMPLES_3    1440  // ~30ms
#define REVERB_DELAY_SAMPLES_4    1920  // ~40ms

#define REVERB_MIX_MIN            64    // 25% reverb
#define REVERB_MIX_MAX            192   // 75% reverb
#define REVERB_MIX_DEFAULT        128   // 50% reverb default

extern volatile u32 reverb_mix;         // Reverb mix amount
extern volatile u32 reverb_buffer[1920]; // Reverb delay buffer (max 40ms)
extern volatile u32 reverb_write_head;   // Write head for reverb buffer

// ============================================================================
// FUNCTION PROTOTYPES
// ============================================================================
void effects_init(void);
void update_tremolo_phase_inc(void);  // Update phase increment when rate changes
int32_t process_chorus(int32_t input);
int32_t process_tremolo(int32_t input);
int32_t process_bass_boost(int32_t input);
int32_t process_reverb(int32_t input);

#endif // EFFECTS_H

