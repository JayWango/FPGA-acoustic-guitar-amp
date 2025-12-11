#include "chorus.h"
#include "tremolo.h"  // For shared sine_table
#include "xil_printf.h"
#include <stdint.h>

// ============================================================================
// CHORUS STATE VARIABLES
// ============================================================================

volatile u8 chorus_enabled = 0;
volatile u32 chorus_rate = CHORUS_RATE_DEFAULT;
volatile u32 chorus_delay = CHORUS_DELAY_DEFAULT;
volatile u32 chorus_depth = CHORUS_DEPTH_DEFAULT;
volatile u8 chorus_adjust_mode = 0;


// Internal state (not exposed externally)
static volatile uint32_t chorus_lfo_phase = 0;        // LFO phase accumulator (0 to TREMOLO_SINE_TABLE_SIZE-1)
static volatile uint32_t chorus_phase_inc = 0;        // Phase increment per sample (fixed-point)

// ============================================================================
// PHASE INCREMENT CALCULATION
// ============================================================================
// determines the speed of chorus modulation; chorus_phase_inc is a number within range 1-9
void update_chorus_phase_inc(void) {
    // Calculate phase increment per sample
    // chorus_rate is in units of 0.1 Hz (e.g., 10 = 1.0 Hz)
    // Phase increment = (rate_hz * table_size) / sample_rate
    //                 = (chorus_rate * 0.1 * TREMOLO_SINE_TABLE_SIZE) / CHORUS_SAMPLE_RATE
    //                 = (chorus_rate * TREMOLO_SINE_TABLE_SIZE) / (CHORUS_SAMPLE_RATE * 10)
    // Note: Using shared sine table from tremolo.h

    // Use 64-bit math to avoid overflow, then scale by 256 for fractional precision
    uint64_t numerator = (uint64_t)chorus_rate * TREMOLO_SINE_TABLE_SIZE * 256;
    uint64_t denominator = (uint64_t) CHORUS_SAMPLE_RATE * 10;
    uint64_t result = numerator / denominator;

    chorus_phase_inc = (uint32_t)result;

    // Ensure minimum phase increment to prevent LFO from getting stuck
    if (chorus_phase_inc == 0 && chorus_rate > 0) {
        chorus_phase_inc = 1;  // Minimum increment (scaled by 256)
    }
}

// ============================================================================
// CHORUS PROCESSING
// ============================================================================
int32_t process_chorus(int32_t input, volatile u32* buffer, u32 buffer_size, u32 write_head) {
    // Update LFO phase with fractional precision
    // chorus_phase_inc is scaled by 256, so we accumulate it
    chorus_lfo_phase += chorus_phase_inc;

    // Extract integer phase index from index ranged between 0 to 255
    // The mask ensures we wrap at table size; for example, 256 & 255 = 0
    // this line is the same as: phase_index = (chorus_lfo_phase / 256) % TREMOLO_SINE_TABLE_SIZE
    // Note: Using shared sine table from tremolo.h
    uint32_t phase_index = (chorus_lfo_phase >> 8) & (TREMOLO_SINE_TABLE_SIZE - 1);

    // Reset accumulator when it exceeds one full cycle to prevent overflow
    // One full cycle = TREMOLO_SINE_TABLE_SIZE * 256 = 65536
    if (chorus_lfo_phase >= ((uint32_t) TREMOLO_SINE_TABLE_SIZE * 256)) {
        // keep all values between 0 to 65535
        chorus_lfo_phase &= 0xFFFF;
    }

    // Look up sine value from table (0-255 range, centered at 128)
    // sine_table represents sin(0) to sin(2π), where:
    // - 128 = sin(0) = sin(π) = sin(2π) (zero crossings)
    // - 255 = sin(π/2) (peak)
    // - 1 = sin(3π/2) (trough)
    uint32_t lfo_raw = sine_table[phase_index];

    // Convert sine table value to delay modulation
    // We want delay to oscillate between (chorus_delay - chorus_depth) and (chorus_delay + chorus_depth)
    // Map sine value (0-255, centered at 128) to delay offset (-chorus_depth to +chorus_depth)
    // When sine = 128 (zero): delay_offset = 0
    // When sine = 255 (peak): delay_offset = +chorus_depth
    // When sine = 1 (trough): delay_offset = -chorus_depth

    int32_t sine_offset = (int32_t) lfo_raw - 128;  // Range: -127 to +127

    // Calculate modulation: sine_offset * chorus_depth / 127
    // Use fixed-point math: multiply first, then divide
    int32_t delay_modulation = (sine_offset * (int32_t) chorus_depth) >> 7;

    // Calculate modulated delay
    int32_t modulated_delay = (int32_t) chorus_delay + delay_modulation;

    // Clamp delay to valid range (must be at least 1 sample, and less than buffer_size)
    if (modulated_delay < 1) modulated_delay = 1;
    if (modulated_delay >= (int32_t) buffer_size) modulated_delay = buffer_size - 1;

    // Calculate read position in circular buffer
    // Read from position that is 'modulated_delay' samples behind write_head
    u32 read_head = (write_head - modulated_delay + buffer_size) % buffer_size;

    // Get delayed sample from buffer
    int32_t delayed_signal = (int32_t) buffer[read_head];

    // Mix dry (current) and wet (delayed) signals
    int32_t dry_mixed = (input * CHORUS_DRY_MIX) >> 8;
    int32_t wet_mixed = (delayed_signal * CHORUS_WET_MIX) >> 8;
    int32_t output = dry_mixed + wet_mixed;

    return output;
}

// ============================================================================
// INITIALIZATION
// ============================================================================
void init_chorus(void) {
    chorus_enabled = 0;
    chorus_rate = CHORUS_RATE_DEFAULT;
    chorus_delay = CHORUS_DELAY_DEFAULT;
    chorus_depth = CHORUS_DEPTH_DEFAULT;
    chorus_adjust_mode = 0;
    chorus_lfo_phase = 0;
    update_chorus_phase_inc();
}
