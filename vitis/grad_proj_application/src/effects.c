#include "effects.h"
#include <stdint.h>
#include "xil_printf.h"

// ============================================================================
// EFFECT STATE INITIALIZATION
// ============================================================================

// Effect enable flags
volatile u8 chorus_enabled = 0;
volatile u8 tremolo_enabled = 0;
volatile u8 bass_boost_enabled = 0;
volatile u8 reverb_enabled = 0;

// Chorus state
volatile u32 chorus_delay_samples = CHORUS_DELAY_DEFAULT;
volatile u32 chorus_lfo_rate = CHORUS_LFO_RATE_DEFAULT;
volatile u32 chorus_lfo_phase = 0;
volatile u32 chorus_delay_buffer[1440] = {0};
volatile u32 chorus_write_head = 0;

// Tremolo state
volatile u32 tremolo_rate = TREMOLO_RATE_DEFAULT;
volatile u32 tremolo_depth = TREMOLO_DEPTH_DEFAULT;
volatile u32 tremolo_lfo_phase = 0;
volatile u32 tremolo_phase_inc = 0;  // Pre-calculated phase increment (updated when rate changes)

// Bass boost state
volatile u32 bass_boost_gain = BASS_BOOST_GAIN_DEFAULT;
volatile int32_t bass_boost_state_low = 0;
volatile int32_t bass_boost_state_high = 0;

// Reverb state
volatile u32 reverb_mix = REVERB_MIX_DEFAULT;
volatile u32 reverb_buffer[1920] = {0};
volatile u32 reverb_write_head = 0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Simple triangle wave LFO (faster than sine, good enough for modulation)
// Returns value in range -256 to +256 (fixed-point *256)
static int32_t fast_sin(uint32_t phase) {
    // Phase is 0-65535 (16-bit), maps to 0-2π
    // Use triangle wave approximation (faster than sine, sounds good for LFO)
    
    // Map phase to 0-1023 (10 bits)
    uint32_t phase_10bit = (phase >> 6) & 0x3FF;
    
    // Triangle wave: rising then falling
    int32_t result;
    if (phase_10bit < 512) {
        result = (int32_t)phase_10bit;  // Rising edge: 0 to 511
    } else {
        result = 1024 - (int32_t)phase_10bit;  // Falling edge: 512 to 0
    }
    
    // Center around 0 and scale to -256 to +256
    // Map 0-512 range to -256 to +256
    result = (result - 256) << 1;
    
    // Clamp to -256 to +256
    if (result > 256) result = 256;
    if (result < -256) result = -256;
    
    return result;
}

// ============================================================================
// CHORUS EFFECT
// ============================================================================
int32_t process_chorus(int32_t input) {
    if (!chorus_enabled) {
        return input;
    }
    
    // Update LFO phase
    // LFO rate: 0.1-5 Hz, sample rate: 48kHz
    // Phase increment per sample = (rate * 65536) / 48000
    uint32_t phase_inc = (chorus_lfo_rate * 65536) / 48000;
    chorus_lfo_phase += phase_inc;
    if (chorus_lfo_phase > 65535) {
        chorus_lfo_phase -= 65536;
    }
    
    // Get LFO value (-256 to +256)
    int32_t lfo_value = fast_sin(chorus_lfo_phase);
    
    // Calculate modulated delay time
    // Base delay + modulation depth (±32 samples max)
    int32_t mod_depth = (lfo_value * CHORUS_DEPTH_DEFAULT) >> 8;  // Scale to ±32 samples
    int32_t mod_delay = (int32_t)chorus_delay_samples + mod_depth;
    
    // Clamp delay time
    if (mod_delay < CHORUS_DELAY_MIN_SAMPLES) mod_delay = CHORUS_DELAY_MIN_SAMPLES;
    if (mod_delay > CHORUS_DELAY_MAX_SAMPLES) mod_delay = CHORUS_DELAY_MAX_SAMPLES;
    
    // Calculate read head
    u32 read_idx = (chorus_write_head - mod_delay + 1440) % 1440;
    
    // Read delayed sample
    int32_t delayed = (int32_t)chorus_delay_buffer[read_idx];
    
    // Write current sample to buffer
    chorus_delay_buffer[chorus_write_head] = input;
    chorus_write_head = (chorus_write_head + 1) % 1440;
    
    // Mix dry (50%) and wet (50%)
    int32_t dry = (input * 128) >> 8;      // 50% dry
    int32_t wet = (delayed * 128) >> 8;    // 50% wet
    return dry + wet;
}

// ============================================================================
// TREMOLO EFFECT
// ============================================================================
int32_t process_tremolo(int32_t input) {
    if (!tremolo_enabled) {
        return input;
    }
    
    // Update LFO phase using pre-calculated phase increment
    // (phase_inc is calculated when tremolo_rate changes, not every sample)
    tremolo_lfo_phase += tremolo_phase_inc;
    if (tremolo_lfo_phase > 65535) {
        tremolo_lfo_phase -= 65536;
    }
    
    // Get LFO value (-256 to +256)
    int32_t lfo_raw = fast_sin(tremolo_lfo_phase);
    // Convert to 0-256 range (centered)
    int32_t lfo_value = 128 + (lfo_raw >> 1);  // 0-256 range
    if (lfo_value > 256) lfo_value = 256;
    if (lfo_value < 0) lfo_value = 0;
    
    // Calculate gain: base + modulation
    // tremolo_depth controls how much modulation (64-192 = 25%-75%)
    // Gain = 256 - depth + (depth * lfo_value / 256)
    int32_t base_gain = 256 - tremolo_depth;
    int32_t mod_gain = (tremolo_depth * lfo_value) >> 8;
    int32_t total_gain = base_gain + mod_gain;
    
    // Safety check: ensure gain is never 0 (would cause silence)
    if (total_gain <= 0) {
        total_gain = 1;  // Minimum gain to prevent silence
    }
    
    // Debug: Print modulation values every 48000 samples (~1 second)
    static uint32_t debug_counter = 0;
    if (debug_counter++ >= 48000) {
        debug_counter = 0;
        xil_printf("=== Tremolo Modulation Debug ===\r\n");
        xil_printf("tremolo_rate: %lu, tremolo_depth: %lu\r\n", tremolo_rate, tremolo_depth);
        xil_printf("tremolo_phase_inc: %lu\r\n", tremolo_phase_inc);
        xil_printf("tremolo_lfo_phase: %lu\r\n", tremolo_lfo_phase);
        xil_printf("lfo_raw: %ld, lfo_value: %ld\r\n", lfo_raw, lfo_value);
        xil_printf("base_gain: %ld, mod_gain: %ld, total_gain: %ld\r\n", base_gain, mod_gain, total_gain);
        xil_printf("Gain range: %ld to %ld (%.1f%% to %.1f%%)\r\n", 
                   base_gain, base_gain + tremolo_depth,
                   (base_gain * 100.0f) / 256.0f,
                   ((base_gain + tremolo_depth) * 100.0f) / 256.0f);
        xil_printf("================================\r\n");
    }
    
    // Apply gain
    return (input * total_gain) >> 8;
}

// ============================================================================
// BASS BOOST EFFECT
// ============================================================================
int32_t process_bass_boost(int32_t input) {
    if (!bass_boost_enabled) {
        return input;
    }
    
    // Simple shelving filter: split into low and high frequencies
    // Low-pass filter (tracks bass)
    int32_t alpha_low = 240;  // ~100 Hz cutoff (fixed-point *256)
    bass_boost_state_low = bass_boost_state_low + ((input - bass_boost_state_low) * alpha_low >> 8);
    
    // High-pass filter (everything else)
    int32_t alpha_high = 16;  // ~100 Hz cutoff
    bass_boost_state_high = bass_boost_state_high + ((input - bass_boost_state_high) * alpha_high >> 8);
    int32_t high_freq = input - bass_boost_state_high;
    
    // Boost bass frequencies
    int32_t bass_boosted = (bass_boost_state_low * bass_boost_gain) >> 8;
    
    // Mix boosted bass with high frequencies
    return bass_boosted + high_freq;
}

// ============================================================================
// REVERB EFFECT
// ============================================================================
int32_t process_reverb(int32_t input) {
    if (!reverb_enabled) {
        return input;
    }
    
    // Write input to buffer
    reverb_buffer[reverb_write_head] = input;
    
    // Read from multiple delay taps with different delays
    int32_t tap1 = (int32_t)reverb_buffer[(reverb_write_head - REVERB_DELAY_SAMPLES_1 + 1920) % 1920];
    int32_t tap2 = (int32_t)reverb_buffer[(reverb_write_head - REVERB_DELAY_SAMPLES_2 + 1920) % 1920];
    int32_t tap3 = (int32_t)reverb_buffer[(reverb_write_head - REVERB_DELAY_SAMPLES_3 + 1920) % 1920];
    int32_t tap4 = (int32_t)reverb_buffer[(reverb_write_head - REVERB_DELAY_SAMPLES_4 + 1920) % 1920];
    
    // Mix taps with decreasing amplitude (simulate room reflections)
    int32_t reverb_signal = (tap1 >> 1) + (tap2 >> 2) + (tap3 >> 3) + (tap4 >> 4);
    
    // Update write head
    reverb_write_head = (reverb_write_head + 1) % 1920;
    
    // Mix dry and wet based on reverb_mix
    int32_t dry = (input * (256 - reverb_mix)) >> 8;
    int32_t wet = (reverb_signal * reverb_mix) >> 8;
    return dry + wet;
}

// ============================================================================
// HELPER: Update tremolo phase increment (called when rate changes)
// ============================================================================
void update_tremolo_phase_inc(void) {
    // Calculate phase increment: (tremolo_rate * 65536) / 480000
    // tremolo_rate is in units of 0.1 Hz (e.g., 5 = 0.5 Hz)
    // Use 64-bit math to avoid integer truncation to 0
    
    // Calculate intermediate values for debugging
    uint64_t numerator = (uint64_t)tremolo_rate * 65536;
    uint64_t denominator = 480000;
    uint64_t result_64bit = numerator / denominator;
    
    // Print calculation details
    xil_printf("=== Tremolo Phase Increment Calculation ===\r\n");
    xil_printf("tremolo_rate: %lu\r\n", tremolo_rate);
    xil_printf("numerator (rate * 65536): %llu\r\n", numerator);
    xil_printf("denominator: %llu\r\n", denominator);
    xil_printf("64-bit result: %llu\r\n", result_64bit);
    
    tremolo_phase_inc = (uint32_t)result_64bit;
    
    xil_printf("32-bit tremolo_phase_inc: %lu\r\n", tremolo_phase_inc);
    
    // Calculate expected values for verification (using integer math to avoid float printf issues)
    uint32_t expected_hz_x10 = tremolo_rate;  // Already in 0.1 Hz units
    uint32_t expected_period_samples = (48000 * 10) / expected_hz_x10;  // Period in samples * 10
    uint32_t expected_phase_inc_x1000 = (65536 * 1000) / expected_period_samples;  // Scaled by 1000
    
    xil_printf("Expected: %lu.%lu Hz = %lu.%lu samples/cycle = %lu.%03lu phase_inc/sample\r\n", 
               expected_hz_x10 / 10, expected_hz_x10 % 10,
               expected_period_samples / 10, expected_period_samples % 10,
               expected_phase_inc_x1000 / 1000, expected_phase_inc_x1000 % 1000);
    
    // Ensure minimum phase increment of 1 to prevent LFO from getting stuck
    if (tremolo_phase_inc == 0 && tremolo_rate > 0) {
        xil_printf("WARNING: phase_inc was 0, setting to 1\r\n");
        tremolo_phase_inc = 1;
    }
    
    xil_printf("Final tremolo_phase_inc: %lu\r\n", tremolo_phase_inc);
    xil_printf("tremolo_depth: %lu (modulation depth)\r\n", tremolo_depth);
    xil_printf("==========================================\r\n");
}

// ============================================================================
// INITIALIZATION
// ============================================================================
void effects_init(void) {
    // All effects start disabled
    chorus_enabled = 0;
    tremolo_enabled = 0;
    bass_boost_enabled = 0;
    reverb_enabled = 0;
    
    // Initialize to default values (already done by static initialization)
    // Pre-calculate tremolo phase increment
    update_tremolo_phase_inc();
    
    // Clear buffers
    for (int i = 0; i < 1440; i++) {
        chorus_delay_buffer[i] = 0;
    }
    for (int i = 0; i < 1920; i++) {
        reverb_buffer[i] = 0;
    }
}

