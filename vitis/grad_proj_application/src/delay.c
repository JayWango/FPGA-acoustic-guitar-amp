#include "delay.h"
#include "xil_printf.h"
#include <stdint.h>

// ============================================================================
// DELAY STATE VARIABLES
// ============================================================================

volatile u8 delay_enabled = 0;
volatile u32 delay_samples = DELAY_SAMPLES_DEFAULT;

// ============================================================================
// DELAY PROCESSING
// ============================================================================
int32_t process_delay(int32_t input, volatile u32* buffer, u32 buffer_size, u32 write_head) {
	// Calculate read_head dynamically based on current write_head and delay_samples
	// Note: write_head points to the NEXT write position (already incremented after writing current sample)
	// Current sample is at (write_head - 1), so we need to read from (write_head - 1 - delay_samples)
	// This ensures 'read_head' always points to data written 'delay_samples' ago
	// We calculate it here to avoid race conditions
	u32 read_head = (write_head - delay_samples + buffer_size) % buffer_size;

	int32_t delayed_signal = (int32_t)buffer[read_head];

	// Mix dry (current) and wet (delayed) signals
	int32_t dry_mixed = (input * DRY_MIX) >> 8;
	int32_t wet_mixed = (delayed_signal * WET_MIX) >> 8;
	int32_t output = dry_mixed + wet_mixed;

	return output;
}

// ============================================================================
// INITIALIZATION
// ============================================================================
void init_delay(void) {
    delay_enabled = 0;
    delay_samples = DELAY_SAMPLES_DEFAULT;
}
