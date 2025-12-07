#include "bsp.h"
#include "delay.h"
#include "encoder.h"
#include "xil_printf.h"

XIntc sys_intc;
XGpio enc;
XGpio pushBtn;
XTmrCtr sampling_tmr; // axi_timer_0
XTmrCtr pwm_tmr; // axi_timer_1

volatile u32 circular_buffer[BUFFER_SIZE] = {0};
volatile u32 read_head = READ_START;
volatile u32 write_head = 0;

volatile u8 delay_enabled = 0;
volatile u32 delay_samples = DELAY_SAMPLES_DEFAULT;
volatile u32 samples_written = 0;

// variables used in sampling_ISR() for printing statistics and collecting the DC offset of the raw data
static int32_t dc_bias_drift = 0;
static int32_t dc_bias_static = 0;
static int first_run = 1; // just a simple flag

static int32_t hp_filter_state = 0;
static int32_t lp_filter_state = 0;
static int32_t agc_gain = 256;

volatile u32 btn_prev_press_time = 0;
static unsigned int enc_prev_press = 0;

volatile u32 sys_tick_counter = 0;

void BSP_init() {
	// interrupt controller
	XIntc_Initialize(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_DEVICE_ID);
	XIntc_Start(&sys_intc, XIN_REAL_MODE);

	init_btn_gpio();
	init_enc_gpio();
	init_pwm_timer();
	init_sampling_timer();
}

// samples are grabbed from the streamer at 48828.125 Hz, so need to modify this sampling ISR to grab data at the same frequency
// grab more than 1 sample in each ISR, for example grab 5 at a time and print out the sample index to ensure that we aren't skipping samples
// currently, there's a fundamental mismatch between our sampling ISR (44.1 kHz) and the stream grabber (48.828125 kHz)
void sampling_ISR() {
	sys_tick_counter++;

	// jason note: i realized that it doesn't make sense to have a for loop within the ISR; if we are doing this interrupt at the same rate as the stream grabber, we would never read the next index of the grabber
	// refer to stream_grabber.c from lab3a for why this is necessary
	// BASEADDR + 4 is the offset of where you "select" which index to read from the stream grabber
	// BASEADDR + 8 is the offset of where you actually read the raw data of the mic

	Xil_Out32(XPAR_MIC_BLOCK_STREAM_GRABBER_0_BASEADDR + 4, 0);
	int32_t curr_sample = (int32_t) Xil_In32(XPAR_MIC_BLOCK_STREAM_GRABBER_0_BASEADDR + 8);

    // set first sample from mic as the dc_bias
    if (first_run) {
        dc_bias_static = curr_sample;
        first_run = 0;
    }

	// self note: try to comment out this exponential moving average and see how it affects our audio signal
    // Exponential Moving Average
	// this line expands to: dc_bias = dc_bias + ((curr_sample - dc_bias) >> 10);
    dc_bias_drift += (curr_sample - dc_bias_drift) >> 10;

//    if ((dc_bias_drift > curr_sample + 100000) || (dc_bias_drift < curr_sample - 100000)) {
//    	dc_bias_drift = dc_bias_static;
//    }

    // remove the DC offset from the current sample
    int32_t audio_signal = curr_sample - dc_bias_drift;

    // HIGH-PASS FILTER (removes low-frequency rumble)
    hp_filter_state = hp_filter_state + ((audio_signal - hp_filter_state) * HP_FILTER_COEFF >> 8);
    int32_t filtered_signal = audio_signal - hp_filter_state;

    // int32_t hpf_signal = filtered_signal >> 15;

    // LPF filter to remove squeals?
    lp_filter_state = lp_filter_state + ((filtered_signal - lp_filter_state) * LP_FILTER_COEFF >> 8);

    // now that we preserve the sign, we can shift safely
	// scale the signal down to a nice number ideally between -1024 and 1024
    int32_t scaled_signal = audio_signal >> 16; // change num back to 15 if it sounds bad
    //int32_t scaled_signal = lp_filter_state >> 16;

    //int32_t scaled_signal_before_agc = scaled_signal;

    // AUTOMATIC GAIN CONTROL (prevents feedback)
    // Detect input level and reduce gain when input is loud
    int32_t input_level = (scaled_signal < 0) ? -scaled_signal : scaled_signal;

    // Calculate gain reduction when input exceeds threshold
    if (input_level > AGC_THRESHOLD) {
        // Reduce gain proportionally to input level
        // Gain reduction = (input_level - threshold) / reduction_rate
        int32_t excess = input_level - AGC_THRESHOLD;
        int32_t gain_reduction = excess >> AGC_REDUCTION_RATE;  // Divide by 16
        agc_gain = 256 - gain_reduction;
        if (agc_gain < AGC_MIN_GAIN) {
            agc_gain = AGC_MIN_GAIN;  // Never go below minimum gain
        }
    } else {
        // Gradually restore gain when input is below threshold
        // Slowly increase gain back to full (256)
        if (agc_gain < 256) {
            agc_gain += 1;  // Slow recovery
            if (agc_gain > 256) agc_gain = 256;
        }
    }

    // Apply AGC gain to signal
    int32_t agc_signal = (scaled_signal * agc_gain) >> 8;

    // INPUT LIMITER (prevents clipping in processing chain)
    // Soft limiter: compress signal above threshold
    int32_t limited_signal = agc_signal;
    if (limited_signal > INPUT_LIMIT_THRESHOLD) {
        // Soft compression: threshold + (excess / 4)
        limited_signal = INPUT_LIMIT_THRESHOLD + ((limited_signal - INPUT_LIMIT_THRESHOLD) >> 2);
    } else if (limited_signal < -INPUT_LIMIT_THRESHOLD) {
        limited_signal = -INPUT_LIMIT_THRESHOLD + ((limited_signal + INPUT_LIMIT_THRESHOLD) >> 2);
    }

    // Use limited_signal for further processing
    scaled_signal = limited_signal;

    // ************************************************************************************************

    circular_buffer[write_head] = scaled_signal;
    samples_written++;

//    int32_t delayed_signal = circular_buffer[read_head];
//
//    int32_t dry_mixed = (scaled_signal * DRY_MIX) >> 8;
//    int32_t wet_mixed = (delayed_signal * WET_MIX) >> 8;
//    int32_t mixed_signal = dry_mixed + wet_mixed;
//
//    write_head = (write_head + 1) % BUFFER_SIZE;
//    read_head = (read_head + 1) % BUFFER_SIZE;

    int32_t mixed_signal;
    if (delay_enabled) {
//    	int32_t delayed_signal = circular_buffer[read_head];
//
//		int32_t dry_mixed = (scaled_signal * DRY_MIX) >> 8;
//		int32_t wet_mixed = (delayed_signal * WET_MIX) >> 8;
//		int32_t mixed_signal = dry_mixed + wet_mixed;
//
//		write_head = (write_head + 1) % BUFFER_SIZE;
//		read_head = (read_head + 1) % BUFFER_SIZE;
        // Only process delay if buffer has enough samples written
        // Need at least delay_samples worth of data in buffer
        if (samples_written > delay_samples) {
            // Calculate read_head dynamically based on current write_head and delay_samples
            // This ensures read_head always points to data written delay_samples ago
            // We calculate it here in the ISR to avoid race conditions
            u32 current_read_head = (write_head - delay_samples + BUFFER_SIZE) % BUFFER_SIZE;

            // Read delayed sample from circular buffer
            int32_t delayed_signal = (int32_t)circular_buffer[current_read_head];

            // DEBUG: Print values occasionally to debug (remove in production)
//            static u32 debug_count = 0;
//            if (debug_count++ % 48000 == 0) {  // Once per second
//                xil_printf("DEBUG: write_head=%lu, read_head=%lu, calc_read_head=%lu, delay_samples=%lu\r\n",
//                           write_head, read_head, current_read_head, delay_samples);
//                xil_printf("DEBUG: scaled_signal=%ld, delayed_signal=%ld, samples_written=%lu\r\n",
//                           scaled_signal, delayed_signal, samples_written);
//            }

            // Mix dry (current) and wet (delayed) signals
            int32_t dry_mixed = (scaled_signal * DRY_MIX) >> 8;
            int32_t wet_mixed = (delayed_signal * WET_MIX) >> 8;
            mixed_signal = dry_mixed + wet_mixed;

            // Update read_head for tracking
            read_head = current_read_head;
        } else {
            // Buffer not ready yet - output dry signal until buffer fills
            mixed_signal = scaled_signal;
        }

        // Always update write_head
        write_head = (write_head + 1) % BUFFER_SIZE;

    } else {
    	mixed_signal = scaled_signal;

    	write_head = (write_head + 1) % BUFFER_SIZE;
    }

    // OUTPUT LIMITER
    int32_t output_signal = mixed_signal;
    if (output_signal > OUTPUT_LIMIT_THRESHOLD) {
    	output_signal = OUTPUT_LIMIT_THRESHOLD;
    }
    else if (output_signal < -OUTPUT_LIMIT_THRESHOLD) {
    	output_signal = -OUTPUT_LIMIT_THRESHOLD;
    }

    int32_t pwm_sample = output_signal + (RESET_VALUE / 2);

    // re-center for PWM (unsigned output between 0 to 2267)
    // we add the mid-point of the PWM ticks (2267/2 = 1133) to turn the signed AC wave into a positive DC wave
//    int32_t pwm_sample = mixed_signal + (RESET_VALUE / 2);

    // clip the audio for safety 
    if (pwm_sample < 0) pwm_sample = 0;
    if (pwm_sample > RESET_VALUE) pwm_sample = RESET_VALUE;

	if (sys_tick_counter >= 72000) {  // Print once per second at 48kHz
//		xil_printf("=== Signal Processing Debug ===\r\n");
		xil_printf("Raw sample:        %ld\r\n", curr_sample);
		xil_printf("DC bias(static):            %ld\r\n", dc_bias_static);
		xil_printf("DC bias(drift):            %ld\r\n", dc_bias_drift);
		xil_printf("Input level:        %ld (threshold: %d)\r\n", input_level, AGC_THRESHOLD);
		xil_printf("After input limit:  %ld (threshold: %d)\r\n", limited_signal, INPUT_LIMIT_THRESHOLD);
		xil_printf("After output limit: %ld (threshold: %d)\r\n", output_signal, OUTPUT_LIMIT_THRESHOLD);
		xil_printf("PWM sample:         %ld\r\n", pwm_sample);
		xil_printf("Delay: enabled=%d, samples=%lu\r\n", delay_enabled, delay_samples);
		xil_printf("===============================\r\n\r\n");
		sys_tick_counter = 0;
	}

	// set the duty cycle of the PWM signal
    XTmrCtr_SetResetValue(&pwm_tmr, 1, pwm_sample);

    // need to write some value to baseaddr of stream grabber to reset it for the next sample
    Xil_Out32(XPAR_MIC_BLOCK_STREAM_GRABBER_0_BASEADDR, 0);

    // clear the interrupt flag to enable the interrupt to trigger again; csr = control status register
    Xuint32 csr = XTmrCtr_ReadReg(sampling_tmr.BaseAddress, 0, XTC_TCSR_OFFSET);
    XTmrCtr_WriteReg(sampling_tmr.BaseAddress, 0, XTC_TCSR_OFFSET, csr | XTC_CSR_INT_OCCURED_MASK);
}

void init_btn_gpio() {
	XGpio_Initialize(&pushBtn, XPAR_AXI_GPIO_BTN_DEVICE_ID);
	XGpio_SetDataDirection(&pushBtn, 1, 0xFFFFFFFF);
	XGpio_InterruptEnable(&pushBtn, XGPIO_IR_CH1_MASK);
	XGpio_InterruptGlobalEnable(&pushBtn);
	XIntc_Connect(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_AXI_GPIO_BTN_IP2INTC_IRPT_INTR, (XInterruptHandler) pushBtn_ISR, &pushBtn);
	XIntc_Enable(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_AXI_GPIO_BTN_IP2INTC_IRPT_INTR);
}

void init_enc_gpio() {
	// encoder GPIO, interrupt controller, and ISR initialization
	XGpio_Initialize(&enc, XPAR_ENCODER_DEVICE_ID);
	XGpio_InterruptEnable(&enc, XGPIO_IR_CH1_MASK);
	XGpio_InterruptGlobalEnable(&enc);
	XIntc_Connect(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_ENCODER_IP2INTC_IRPT_INTR, (XInterruptHandler) enc_ISR, &enc);
	XIntc_Enable(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_ENCODER_IP2INTC_IRPT_INTR);
}

void pushBtn_ISR(void *CallbackRef) {
	XGpio *GpioPtr = (XGpio *)CallbackRef;
	unsigned int btn_val = XGpio_DiscreteRead(GpioPtr, 1);

	u32 btn_curr_press_time = sys_tick_counter;
	u32 time_between_press = btn_curr_press_time - btn_prev_press_time;

	// BTN_MIDDLE, RIGHT, etc. are bit masks defined in bsp.h
	if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_MIDDLE)) {
		btn_prev_press_time = btn_curr_press_time;
		xil_printf("btn middle press\r\n");
	}

	else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_RIGHT)) {
		btn_prev_press_time = btn_curr_press_time;
		xil_printf("btn right press\r\n");
	}
    else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_TOP)) {
        btn_prev_press_time = btn_curr_press_time;
        // Button 0: Toggle delay effect on/off
        delay_enabled = !delay_enabled;
        if (delay_enabled) {
            // Recalculate read_head when enabling delay
            // Ensure read_head is valid (within buffer bounds)
            read_head = (write_head - delay_samples + BUFFER_SIZE) % BUFFER_SIZE;
            xil_printf("Delay ON: %lu samples (~%lu ms)\r\n", delay_samples, (delay_samples * 1000) / 48000);
        }
        else {
            xil_printf("Delay OFF\r\n");
        }
    }

	else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_LEFT)) {
		btn_prev_press_time = btn_curr_press_time;
		xil_printf("btn left press\r\n");
	}

	else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_BOTTOM)) {
		btn_prev_press_time = btn_curr_press_time;
		xil_printf("btn bottom press\r\n");
	}

	XGpio_InterruptClear(GpioPtr, XGPIO_IR_CH1_MASK);
}

void enc_ISR(void *CallbackRef) {
	//xil_printf("enc ISR hit\r\n");

	XGpio *GpioPtr = (XGpio *)CallbackRef;
	unsigned int curr_press = XGpio_DiscreteRead(GpioPtr, 1);

	uint32_t v = XGpio_DiscreteRead(&enc, ENCODER_GPIO_CH);
	uint8_t A  = (v >> 0) & 1u;
	uint8_t B  = (v >> 1) & 1u;
	uint8_t ab = (A << 1) | B;
	quad_step(ab);

	if (delay_enabled) {
		/* Raise flags on completion */
		if (s_saw_ccw) {
			s_saw_ccw  = 0;
			delay_samples += DELAY_ADJUST_STEP;
			if (delay_samples > DELAY_SAMPLES_MAX) {
				delay_samples = DELAY_SAMPLES_MAX;
			}
			read_head = (write_head - delay_samples + BUFFER_SIZE) % BUFFER_SIZE;
			xil_printf("Delay: %lu samples (~%lu ms)\r\n", delay_samples, (delay_samples * 1000) / 48000);
		}

		if (s_saw_cw) {
			s_saw_cw = 0;
			if (delay_samples > DELAY_ADJUST_STEP) {
				delay_samples -= DELAY_ADJUST_STEP;
			} else {
				delay_samples = DELAY_SAMPLES_MIN;
			}
			read_head = (write_head - delay_samples + BUFFER_SIZE) % BUFFER_SIZE;
			xil_printf("Delay: %lu samples (~%lu ms)\r\n", delay_samples, (delay_samples * 1000) / 48000);
		}
	}
	else {
		if (s_saw_cw) {
			s_saw_cw  = 0;
			xil_printf("CW turn\r\n");
		}

		if (s_saw_ccw) {
			s_saw_ccw = 0;
			xil_printf("CCW turn\r\n");
		}
	}

//	/* Raise flags on completion */
//	if (s_saw_cw) {
//		s_saw_cw  = 0;
//		xil_printf("CW turn\r\n");
//	}
//
//	if (s_saw_ccw) {
//		s_saw_ccw = 0;
//		xil_printf("CCW turn\r\n");
//	}

	if (!enc_prev_press && (curr_press & ENC_BTN)) {
		xil_printf("enc btn press\r\n");
	}

	enc_prev_press = curr_press & ENC_BTN; // to prevent interrupts from constantly firing when button is held down

	XGpio_InterruptClear(GpioPtr, XGPIO_IR_CH1_MASK);
}

int init_sampling_timer() {
	XStatus Status;
	Status = XST_SUCCESS;
	Status = XIntc_Connect(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_AXI_TIMER_0_INTERRUPT_INTR,
			(XInterruptHandler) sampling_ISR, &sampling_tmr);
	if (Status != XST_SUCCESS) {
		xil_printf("Failed to connect the application handlers to the interrupt controller...\r\n");
		return XST_FAILURE;
	}
	xil_printf("Connected to Interrupt Controller!\r\n");

	/*
	 * Enable the interrupt for the timer counter
	 */
	XIntc_Enable(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_AXI_TIMER_0_INTERRUPT_INTR);
	/*
	 * Initialize the timer counter so that it's ready to use,
	 * specify the device ID that is generated in xparameters.h
	 */
	Status = XTmrCtr_Initialize(&sampling_tmr, XPAR_AXI_TIMER_0_DEVICE_ID);
	if (Status != XST_SUCCESS) {
		xil_printf("Timer initialization failed...\r\n");
		return XST_FAILURE;
	}
	xil_printf("Initialized Timer!\r\n");
	/*
	 * Enable the interrupt of the timer counter so interrupts will occur
	 * and use auto reload mode such that the timer counter will reload
	 * itself automatically and continue repeatedly, without this option
	 * it would expire once only
	 */
	XTmrCtr_SetOptions(&sampling_tmr, 0, XTC_INT_MODE_OPTION | XTC_AUTO_RELOAD_OPTION);
	/*
	 * Set a reset value for the timer counter such that it will expire
	 * eariler than letting it roll over from 0, the reset value is loaded
	 * into the timer counter when it is started
	 */
	// clk cycles / 100 Mhz = period
	XTmrCtr_SetResetValue(&sampling_tmr, 0, 0xFFFFFFFF - RESET_VALUE);// 2267 clk cycles @ 100MHz = 22.67 us
	/*
	 * Start the timer counter such that it's incrementing by default,
	 * then wait for it to timeout a number of times
	 */
	XTmrCtr_Start(&sampling_tmr, 0);

	/*
	 * Register the intc device driver’s handler with the Standalone
	 * software platform’s interrupt table
	 */
	microblaze_register_handler(
			(XInterruptHandler) XIntc_DeviceInterruptHandler,
			(void*) XPAR_MICROBLAZE_0_AXI_INTC_DEVICE_ID);

	// refer to stream_grabber.c from lab3a; need to write some value to the base address to reinitialize the stream grabber
	Xil_Out32(XPAR_MIC_BLOCK_STREAM_GRABBER_0_BASEADDR, 0);

	xil_printf("Interrupts enabled!\r\n");
	microblaze_enable_interrupts();

	return XST_SUCCESS;
}

int init_pwm_timer() {
	XStatus Status;

	// Initialize the PWM Timer instance
	Status = XTmrCtr_Initialize(&pwm_tmr, XPAR_AXI_TIMER_1_DEVICE_ID);
	if (Status != XST_SUCCESS) {
		return XST_FAILURE;
	}

	// Configure the timer for PWM Mode
	// Generate Output, and Down Counting (easier for duty cycle)
	XTmrCtr_SetOptions(&pwm_tmr, 0, XTC_EXT_COMPARE_OPTION | XTC_DOWN_COUNT_OPTION | XTC_AUTO_RELOAD_OPTION);
	XTmrCtr_SetOptions(&pwm_tmr, 1, XTC_EXT_COMPARE_OPTION | XTC_DOWN_COUNT_OPTION | XTC_AUTO_RELOAD_OPTION);

	// Set the Period (Frequency) in the first register (TLR0)
	// We match the sampling frequency: 2267 ticks
	// Side Note: we can decrease 2267 to a smaller number to increase the amount of 'pwm cycles' in one sampling cycle; this leads to a smoother signal because of analog filtering
	// think of channel 0 of the pwm_tmr as modifying the "Auto Reload Register (ARR)" of STM32 timers
	XTmrCtr_SetResetValue(&pwm_tmr, 0, RESET_VALUE); //

	// Set the Duty Cycle (High Time) in the second register (TLR1)
	// Start with 50% duty cycle (silence)
	// think of channel 1 of the pwm_tmr as modifying the "Capture Compare Register (CCR)" of STM32 timers
	XTmrCtr_SetResetValue(&pwm_tmr, 1, RESET_VALUE / 2); //

	// This function sets the specific bits in the Control Status Register to turn on PWM
	XTmrCtr_PwmEnable(&pwm_tmr);

	// Start the PWM generation
	XTmrCtr_Start(&pwm_tmr, 0);

	xil_printf("PWM Timer successfully initialized!\r\n");

	return XST_SUCCESS;
}
