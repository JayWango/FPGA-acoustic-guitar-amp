#include "tremolo.h"
#include "xil_printf.h"
#include <stdint.h>

// ============================================================================
// SINE LOOKUP TABLE
// ============================================================================
// Pre-computed sine table for LFO generation
// Values range from 0 to 255 (8-bit), representing 0 to 2π
// This gives us smooth tremolo modulation without expensive sine calculations
// this table was generated using the formula: sine_table[i] = 128 + 127 * sin ((i * 2pi) / 256); essentially maps index i to a value between 0-255
const uint8_t sine_table[TREMOLO_SINE_TABLE_SIZE] = {
    128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 161, 164, 167, 170, 173,
    176, 179, 182, 185, 187, 190, 193, 195, 198, 201, 203, 206, 208, 210, 213, 215,
    217, 219, 222, 224, 226, 228, 230, 231, 233, 235, 236, 238, 240, 241, 242, 244,
    245, 246, 247, 248, 249, 250, 251, 251, 252, 253, 253, 254, 254, 254, 254, 254,
    255, 254, 254, 254, 254, 254, 253, 253, 252, 251, 251, 250, 249, 248, 247, 246,
    245, 244, 242, 241, 240, 238, 236, 235, 233, 231, 230, 228, 226, 224, 222, 219,
    217, 215, 213, 210, 208, 206, 203, 201, 198, 195, 193, 190, 187, 185, 182, 179,
    176, 173, 170, 167, 164, 161, 158, 155, 152, 149, 146, 143, 140, 137, 134, 131,
    128, 124, 121, 118, 115, 112, 109, 106, 103, 100, 97, 94, 91, 88, 85, 82,
    79, 76, 73, 70, 68, 65, 62, 60, 57, 54, 52, 49, 47, 45, 42, 40,
    38, 36, 33, 31, 29, 27, 25, 24, 22, 20, 19, 17, 15, 14, 13, 11,
    10, 9, 8, 7, 6, 5, 4, 4, 3, 2, 2, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 2, 2, 3, 4, 4, 5, 6, 7, 8, 9,
    10, 11, 13, 14, 15, 17, 19, 20, 22, 24, 25, 27, 29, 31, 33, 36,
    38, 40, 42, 45, 47, 49, 52, 54, 57, 60, 62, 65, 68, 70, 73, 76,
    79, 82, 85, 88, 91, 94, 97, 100, 103, 106, 109, 112, 115, 118, 121, 124
};

// ============================================================================
// TREMOLO STATE VARIABLES
// ============================================================================

volatile u8 tremolo_enabled = 0;
volatile u32 tremolo_rate = TREMOLO_RATE_DEFAULT;
volatile u32 tremolo_depth = TREMOLO_DEPTH_DEFAULT;
volatile u8 tremolo_adjust_mode = 0; // 0 = rate, 1 = depth

// Internal state (not exposed externally)
static volatile uint32_t tremolo_lfo_phase = 0;        // LFO phase accumulator (0 to TREMOLO_SINE_TABLE_SIZE-1)
static volatile uint32_t tremolo_phase_inc = 0;        // Phase increment per sample (fixed-point)

// ============================================================================
// PHASE INCREMENT CALCULATION
// ============================================================================
// determines the speed of tremolo effect; tremolo_phase_inc is a number within range 1-9
void update_tremolo_phase_inc(void) {
    // Calculate phase increment per sample
    // tremolo_rate is in units of 0.1 Hz (e.g., 10 = 1.0 Hz)
    // Phase increment = (rate_hz * table_size) / sample_rate
    //                 = (tremolo_rate * 0.1 * TREMOLO_SINE_TABLE_SIZE) / TREMOLO_SAMPLE_RATE
    //                 = (tremolo_rate * TREMOLO_SINE_TABLE_SIZE) / (TREMOLO_SAMPLE_RATE * 10)

    // Use 64-bit math to avoid overflow, then scale by 256 for fractional precision
    uint64_t numerator = (uint64_t)tremolo_rate * TREMOLO_SINE_TABLE_SIZE * 256;
    uint64_t denominator = (uint64_t) TREMOLO_SAMPLE_RATE * 10;
    uint64_t result = numerator / denominator;

    tremolo_phase_inc = (uint32_t)result;

    // Ensure minimum phase increment to prevent LFO from getting stuck
    if (tremolo_phase_inc == 0 && tremolo_rate > 0) {
        tremolo_phase_inc = 1;  // Minimum increment (scaled by 256)
    }
}

// ============================================================================
// TREMOLO PROCESSING
// ============================================================================
int32_t process_tremolo(int32_t input) {
    // Update LFO phase with fractional precision
    // tremolo_phase_inc is scaled by 256, so we accumulate it
    tremolo_lfo_phase += tremolo_phase_inc;

    // Extract integer phase index from index ranged between 0 to 255
    // The mask ensures we wrap at table size; for example, 256 & 255 = 0
    // this line is the same as: phase_index = (tremolo_lfo_phase / 256) % TREMOLO_SINE_TABLE_SIZE
    uint32_t phase_index = (tremolo_lfo_phase >> 8) & (TREMOLO_SINE_TABLE_SIZE - 1);

    // Reset accumulator when it exceeds one full cycle to prevent overflow
    // One full cycle = TREMOLO_SINE_TABLE_SIZE * 256 = 65536 
    if (tremolo_lfo_phase >= ((uint32_t) TREMOLO_SINE_TABLE_SIZE * 256)) {
        // keep all values between 0 to 65535
        tremolo_lfo_phase &= 0xFFFF;
    }

    // Look up sine value from table (0-255 range, centered at 128)
    // sine_table represents sin(0) to sin(2π), where:
    // - 128 = sin(0) = sin(π) = sin(2π) (zero crossings)
    // - 255 = sin(π/2) (peak)
    // - 1 = sin(3π/2) (trough)
    uint32_t lfo_raw = sine_table[phase_index];

    // Convert sine table value to gain modulation
    // We want gain to oscillate between min_gain and max_gain
    // min_gain = 256 - tremolo_depth
    // max_gain = 256
    //
    // Map sine value (0-255, centered at 128) to gain (min_gain to max_gain)
    // When sine = 128 (zero): gain = min_gain
    // When sine = 255 (peak): gain = max_gain
    // When sine = 1 (trough): gain = min_gain
    //
    // Formula: gain = min_gain + (sine_value - 128) * tremolo_depth / 127
    // But we need to handle the case when sine < 128 (negative part of wave)

    int32_t sine_offset = (int32_t) lfo_raw - 128;  // Range: -127 to +127
    uint32_t base_gain = 256 - tremolo_depth;

    // Calculate modulation: sine_offset * tremolo_depth / 127
    // Use fixed-point math: multiply first, then divide
    // int32_t modulation = (sine_offset * (int32_t) tremolo_depth) / 127;
    int32_t modulation = (sine_offset * (int32_t) tremolo_depth) >> 7;

    // Add modulation to base gain
    int32_t total_gain = (int32_t) base_gain + modulation;

    // Clamp gain to valid range (1 to 256)
    if (total_gain > 256) total_gain = 256;
    if (total_gain < 1) total_gain = 1;

    // Apply gain to input signal to modulate the input signal's amplitude (volume)
    // Scale by total_gain (1-256 range) and divide by 256
    int32_t output = (input * total_gain) >> 8;

    return output;
}

// ============================================================================
// INITIALIZATION
// ============================================================================
void init_tremolo(void) {
    tremolo_enabled = 0;
    tremolo_rate = TREMOLO_RATE_DEFAULT;
    tremolo_depth = TREMOLO_DEPTH_DEFAULT;
    tremolo_adjust_mode = 0;
    tremolo_lfo_phase = 0;
    update_tremolo_phase_inc();
}
