#include "bsp.h"
#include "delay.h"
#include "encoder.h"
#include "xil_printf.h"
#include "tremolo.h"
#include "chorus.h"

XIntc sys_intc;
XGpio enc;
XGpio pushBtn;
XTmrCtr sampling_tmr; // axi_timer_0
XTmrCtr pwm_tmr; // axi_timer_1

// variables for circular buffer
volatile u32 circular_buffer[BUFFER_SIZE] = {0};
volatile u32 write_head = 0;
volatile u32 samples_written = 0;

// variables used in sampling_ISR() for printing statistics and collecting the DC offset of the raw data
volatile u32 sys_tick_counter = 0;
volatile int32_t curr_sample = 0;
volatile int32_t tiny_buffer[SAMPLES] = {0,0,0,0,0};
static int tiny_buffer_index = 0;
static int32_t dc_bias_drift = 0;
static int32_t dc_bias_static = 0;
static int first_run = 1; // just a simple flag
static int32_t hp_filter_state = 0;
static int32_t lp_filter_state = 0;
static int32_t lp_filter_state_2 = 0;
static int32_t lp_filter_state_3 = 0;

// encoder variables
volatile u32 btn_prev_press_time = 0;
static unsigned int enc_prev_press = 0;

// hpf/lpf variables
volatile u8 adjusting_lp_filter = 0;
volatile u8 adjusting_hp_filter = 0;
volatile u16 lp_filter_coeff = LP_FILTER_COEFF_DEFAULT;
volatile u16 hp_filter_coeff = HP_FILTER_COEFF_DEFAULT;

void BSP_init() {
	// interrupt controller
	XIntc_Initialize(&sys_intc, XPAR_MICROBLAZE_0_AXI_INTC_DEVICE_ID);
	XIntc_Start(&sys_intc, XIN_REAL_MODE);

	init_btn_gpio();
	init_enc_gpio();
	init_pwm_timer();
	init_sampling_timer();

	init_delay();
	init_tremolo();
	init_chorus();
}

// samples are grabbed from the streamer at 48828.125 Hz, so need to modify this sampling ISR to grab data at the same frequency
// grab more than 1 sample in each ISR, for example grab 5 at a time and print out the sample index to ensure that we aren't skipping samples
// currently, there's a fundamental mismatch between our sampling ISR (44.1 kHz) and the stream grabber (48.828125 kHz)
void sampling_ISR() {
	sys_tick_counter++;

	// BASEADDR + 4 is the offset of where you "select" which index to read from the stream grabber
	// BASEADDR + 8 is the offset of where you actually read the raw data of the mic
	Xil_Out32(XPAR_MIC_BLOCK_STREAM_GRABBER_0_BASEADDR + 4, 0);
	int32_t new_sample = (int32_t) Xil_In32(XPAR_MIC_BLOCK_STREAM_GRABBER_0_BASEADDR + 8);

	// tiny_buffer holds the most recent 5 samples, which is used to calculate a rolling average
	tiny_buffer[tiny_buffer_index] = new_sample;
	tiny_buffer_index = (tiny_buffer_index + 1) % 5;

    // set first sample from mic as the dc_bias
    if (first_run) {
		for(int i=0; i<5; i++) tiny_buffer[i] = new_sample;
        dc_bias_static = new_sample;
		dc_bias_drift = new_sample;
		curr_sample = new_sample; 
        first_run = 0;
    }
	else {
		// calculate average immediately
		int32_t sum = 0;
		for (int i = 0; i < 5; i++) {
			sum += tiny_buffer[i];
		}
		// Update global variable with the SMOOTHED value
		curr_sample = sum / 5;
	}

    // moving average of the dc_bias to track it
    dc_bias_drift += (curr_sample - dc_bias_drift) >> 10;
    if ((dc_bias_drift > curr_sample + 1000000) || (dc_bias_drift < curr_sample - 1000000)) {
    	dc_bias_drift = dc_bias_static;
    }

    // remove the DC offset from the current sample
    int32_t audio_signal = curr_sample - dc_bias_drift;

    // HIGH-PASS FILTER (removes low-frequency rumble)
    hp_filter_state = hp_filter_state + ((audio_signal - hp_filter_state) * hp_filter_coeff >> 8);
	// HPF = original signal - LPF; hp_filter_state is the LPF and subtracting it from 'audio_signal' returns the actual HPF signal
    int32_t filtered_signal = audio_signal - hp_filter_state;

    // two cascaded LPF filter (which forms a 2nd order filter) to remove high frequency squeals
    lp_filter_state = lp_filter_state + ((filtered_signal - lp_filter_state) * lp_filter_coeff >> 8);
    lp_filter_state_2 = lp_filter_state_2 + ((lp_filter_state - lp_filter_state_2) * lp_filter_coeff >> 8);
    lp_filter_state_3 = lp_filter_state_3 + ((lp_filter_state_2 - lp_filter_state_3) * lp_filter_coeff >> 8);

    // now that we preserve the sign, we can shift safely
	// scale the signal down to a nice number ideally between -1024 and 1024
    // int32_t scaled_signal = audio_signal >> 16; // change num back to 15 if it sounds bad
    int32_t scaled_signal = lp_filter_state_3 >> 16; // revert back to 17if necessary

    // INPUT LIMITER (prevents clipping in processing chain)
    // Soft limiter: compress signal above threshold, which enables "soft clipping" (sounds better than maxing out the signal)
	// JW Note: on Wed, we should print out scaled_signal at a fast rate to see how we can set a good INPUT LIMIT THRESHOLD
    int32_t limited_signal = scaled_signal;
    if (limited_signal > INPUT_LIMIT_THRESHOLD) {
        // Soft compression: threshold + (excess / 4)
        limited_signal = INPUT_LIMIT_THRESHOLD + ((limited_signal - INPUT_LIMIT_THRESHOLD) >> 2);
    }
    else if (limited_signal < -INPUT_LIMIT_THRESHOLD) {
        limited_signal = -INPUT_LIMIT_THRESHOLD + ((limited_signal + INPUT_LIMIT_THRESHOLD) >> 2);
    }

    // ************************************************************************************************

    circular_buffer[write_head] = limited_signal;
    samples_written++;
    write_head = (write_head + 1) % BUFFER_SIZE;

    int32_t mixed_signal = limited_signal;
    if (delay_enabled && (samples_written > delay_samples)) {
    	mixed_signal = process_delay(mixed_signal, circular_buffer, BUFFER_SIZE, write_head);
    }

    if (tremolo_enabled) {
    	mixed_signal = process_tremolo(mixed_signal);
    }

    if (chorus_enabled && (samples_written > (chorus_delay + chorus_depth))) {
    	mixed_signal = process_chorus(mixed_signal, circular_buffer, BUFFER_SIZE, write_head);
    }

    // OUTPUT LIMITER
     int32_t output_signal = mixed_signal;
     if (output_signal > OUTPUT_LIMIT_THRESHOLD) {
     	output_signal = OUTPUT_LIMIT_THRESHOLD;
     }
     else if (output_signal < -OUTPUT_LIMIT_THRESHOLD) {
     	output_signal = -OUTPUT_LIMIT_THRESHOLD;
     }

    // re-center for PWM (unsigned output between 0 to 2048)
    // we add the mid-point of the PWM ticks (2048/2 = 1024) to turn the signed AC wave into a positive DC wave
    int32_t pwm_sample = output_signal + (RESET_VALUE / 2);

    // clip the audio for safety 
    if (pwm_sample < 0) pwm_sample = 0;
    if (pwm_sample > RESET_VALUE) pwm_sample = RESET_VALUE;

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

	// BTN_TOP, BOTTOM, etc. are bit masks defined in bsp.h
    if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_TOP)) {
        btn_prev_press_time = btn_curr_press_time;
        delay_enabled = !delay_enabled;
        if (delay_enabled) {
            xil_printf("Delay ON: %lu samples (~%lu ms)\r\n", delay_samples, (delay_samples * 1000) / 48000);
        }
        else {
            xil_printf("Delay OFF\r\n");
        }
    }
	else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_MIDDLE)) {
		btn_prev_press_time = btn_curr_press_time;
		tremolo_enabled = !tremolo_enabled;
		if (tremolo_enabled) {
			// update_tremolo_phase_inc(); // note jw: try removing this to see if performance is changed; no need to update tremolo phase here?
			xil_printf("Tremolo ON: rate=%lu, depth=%lu\r\n", tremolo_rate, tremolo_depth);
		}
		else {
			xil_printf("Tremolo OFF\r\n");
		}
	}
	else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_BOTTOM)) {
		btn_prev_press_time = btn_curr_press_time;
		chorus_enabled = !chorus_enabled;
		if (chorus_enabled) {
			xil_printf("Chorus ON: rate=%lu.%lu Hz, delay=%lu, depth=%lu\r\n", chorus_rate / 10, chorus_rate % 10, chorus_delay, chorus_depth);
		}
		else {
			xil_printf("Chorus OFF\r\n");
		}
	}
    else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_RIGHT)) {
        btn_prev_press_time = btn_curr_press_time;
        adjusting_lp_filter = !adjusting_lp_filter;
        if (adjusting_lp_filter) {
            adjusting_hp_filter = 0;  // Only one filter adjustment mode at a time
            xil_printf("Adjusting LP Filter (current: %lu)\r\n", lp_filter_coeff);
        }
        else {
            xil_printf("LP Filter adjustment OFF (coeff: %lu)\r\n", lp_filter_coeff);
        }
    }
    else if ((time_between_press > DEBOUNCE_TIME) && (btn_val & BTN_LEFT)) {
		btn_prev_press_time = btn_curr_press_time;
		adjusting_hp_filter = !adjusting_hp_filter;
		if (adjusting_hp_filter) {
			adjusting_lp_filter = 0;  // Only one filter adjustment mode at a time
			xil_printf("Adjusting HP Filter (current: %lu)\r\n", hp_filter_coeff);
		}
		else {
			xil_printf("HP Filter adjustment OFF (coeff: %lu)\r\n", hp_filter_coeff);
		}
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
			xil_printf("Delay: %lu samples (~%lu ms)\r\n", delay_samples, (delay_samples * 1000) / 48000);
		}

		if (s_saw_cw) {
			s_saw_cw = 0;
			if (delay_samples > DELAY_ADJUST_STEP) {
				delay_samples -= DELAY_ADJUST_STEP;
			} else {
				delay_samples = DELAY_SAMPLES_MIN;
			}
			xil_printf("Delay: %lu samples (~%lu ms)\r\n", delay_samples, (delay_samples * 1000) / 48000);
		}
	}
	else if (tremolo_enabled) {
        // Mode 0: Adjust rate (modulation speed)
        // Mode 1: Adjust depth (modulation amount)
        if (tremolo_adjust_mode == 0) {
            // Adjust tremolo rate (modulation speed)
            // CCW = slower (lower rate), CW = faster (higher rate)
            if (s_saw_cw) {
                s_saw_cw = 0;
                if (tremolo_rate > TREMOLO_RATE_MIN) {
                    tremolo_rate -= 1;  // Slower rate (smaller step for finer control)
                } else {
                    tremolo_rate = TREMOLO_RATE_MIN;
                }
                update_tremolo_phase_inc();  // Recalculate phase increment (avoid division in ISR)
                xil_printf("Tremolo rate: %lu.%lu Hz - Slower\r\n", tremolo_rate / 10, tremolo_rate % 10);
            }
            if (s_saw_ccw) {
                s_saw_ccw = 0;
                if (tremolo_rate < TREMOLO_RATE_MAX) {
                    tremolo_rate += 1;  // Faster rate
                } else {
                    tremolo_rate = TREMOLO_RATE_MAX;  // Clamp at max
                }
                update_tremolo_phase_inc();  // Recalculate phase increment (avoid division in ISR)
                xil_printf("Tremolo rate: %lu.%lu Hz - Faster\r\n", tremolo_rate / 10, tremolo_rate % 10);
            }
        }
        else {
            // Adjust tremolo depth (modulation amount)
            // CCW = less depth, CW = more depth
            if (s_saw_cw) {
                s_saw_cw = 0;
                if (tremolo_depth > TREMOLO_DEPTH_MIN) {
                    tremolo_depth -= 4;  // Less depth (step of 4 for reasonable adjustment)
                    if (tremolo_depth < TREMOLO_DEPTH_MIN) {
                        tremolo_depth = TREMOLO_DEPTH_MIN;
                    }
                } else {
                    tremolo_depth = TREMOLO_DEPTH_MIN;
                }
                xil_printf("Tremolo depth: %lu (~%lu%%) - Less\r\n", tremolo_depth, (tremolo_depth * 100) / 256);
            }
            if (s_saw_ccw) {
                s_saw_ccw = 0;
                if (tremolo_depth < TREMOLO_DEPTH_MAX) {
                    tremolo_depth += 4;  // More depth
                    if (tremolo_depth > TREMOLO_DEPTH_MAX) {
                        tremolo_depth = TREMOLO_DEPTH_MAX;
                    }
                } else {
                    tremolo_depth = TREMOLO_DEPTH_MAX;  // Clamp at max
                }
                xil_printf("Tremolo depth: %lu (~%lu%%) - More\r\n", tremolo_depth, (tremolo_depth * 100) / 256);
            }
        }
	}
	else if (chorus_enabled) {
		// Adjust chorus parameters based on chorus_adjust_mode
		// Mode 0: Adjust rate (modulation speed)
		// Mode 1: Adjust delay (base delay time)
		// Mode 2: Adjust depth (modulation amount)
		if (chorus_adjust_mode == 0) {
			// Adjust chorus rate (modulation speed)
			// CCW = slower (lower rate), CW = faster (higher rate)
			if (s_saw_cw) {
				s_saw_cw = 0;
				if (chorus_rate > CHORUS_RATE_MIN) {
					chorus_rate -= 1;  // Slower rate (smaller step for finer control)
				} else {
					chorus_rate = CHORUS_RATE_MIN;
				}
				update_chorus_phase_inc();  // Recalculate phase increment (avoid division in ISR)
				xil_printf("Chorus rate: %lu.%lu Hz - Slower\r\n", chorus_rate / 10, chorus_rate % 10);
			}
			if (s_saw_ccw) {
				s_saw_ccw = 0;
				if (chorus_rate < CHORUS_RATE_MAX) {
					chorus_rate += 1;  // Faster rate
				} else {
					chorus_rate = CHORUS_RATE_MAX;  // Clamp at max
				}
				update_chorus_phase_inc();  // Recalculate phase increment (avoid division in ISR)
				xil_printf("Chorus rate: %lu.%lu Hz - Faster\r\n", chorus_rate / 10, chorus_rate % 10);
			}
		}
		else if (chorus_adjust_mode == 1) {
			// Adjust chorus delay (base delay time)
			// CCW = shorter delay, CW = longer delay
			if (s_saw_cw) {
				s_saw_cw = 0;
				if (chorus_delay > CHORUS_DELAY_MIN) {
					chorus_delay -= CHORUS_DELAY_ADJUST_STEP;
					if (chorus_delay < CHORUS_DELAY_MIN) {
						chorus_delay = CHORUS_DELAY_MIN;
					}
				} else {
					chorus_delay = CHORUS_DELAY_MIN;
				}
				xil_printf("Chorus delay: %lu samples (~%lu ms) - Shorter\r\n",
						   chorus_delay, (chorus_delay * 1000) / 48000);
			}
			if (s_saw_ccw) {
				s_saw_ccw = 0;
				if (chorus_delay < CHORUS_DELAY_MAX) {
					chorus_delay += CHORUS_DELAY_ADJUST_STEP;
					if (chorus_delay > CHORUS_DELAY_MAX) {
						chorus_delay = CHORUS_DELAY_MAX;
					}
				} else {
					chorus_delay = CHORUS_DELAY_MAX;  // Clamp at max
				}
				xil_printf("Chorus delay: %lu samples (~%lu ms) - Longer\r\n",
						   chorus_delay, (chorus_delay * 1000) / 48000);
			}
		}
		else {
			// Adjust chorus depth (modulation amount)
			// CCW = less depth, CW = more depth
			if (s_saw_cw) {
				s_saw_cw = 0;
				if (chorus_depth > CHORUS_DEPTH_MIN) {
					chorus_depth -= CHORUS_DEPTH_ADJUST_STEP;
					if (chorus_depth < CHORUS_DEPTH_MIN) {
						chorus_depth = CHORUS_DEPTH_MIN;
					}
				} else {
					chorus_depth = CHORUS_DEPTH_MIN;
				}
				xil_printf("Chorus depth: %lu samples (~%lu ms) - Less\r\n",
						   chorus_depth, (chorus_depth * 1000) / 48000);
			}
			if (s_saw_ccw) {
				s_saw_ccw = 0;
				if (chorus_depth < CHORUS_DEPTH_MAX) {
					chorus_depth += CHORUS_DEPTH_ADJUST_STEP;
					if (chorus_depth > CHORUS_DEPTH_MAX) {
						chorus_depth = CHORUS_DEPTH_MAX;
					}
				} else {
					chorus_depth = CHORUS_DEPTH_MAX;  // Clamp at max
				}
				xil_printf("Chorus depth: %lu samples (~%lu ms) - More\r\n",
						   chorus_depth, (chorus_depth * 1000) / 48000);
			}
		}
	}
	else if (adjusting_hp_filter) {
		// Adjust HP filter coefficient
		// CCW = more filtering (lower coeff), CW = less filtering (higher coeff)
		if (s_saw_cw) {
			s_saw_cw = 0;
			if (hp_filter_coeff > HP_FILTER_COEFF_MIN) {
				hp_filter_coeff -= FILTER_COEFF_ADJUST_STEP;
				if (hp_filter_coeff < HP_FILTER_COEFF_MIN) {
					hp_filter_coeff = HP_FILTER_COEFF_MIN;
				}
			} else {
				hp_filter_coeff = HP_FILTER_COEFF_MIN;
			}
            u32 cutoff_hz = (hp_filter_coeff * 3035) / 100;  // 30.4 * coeff, scaled by 10 for integer math
            xil_printf("HP Filter: %lu (cutoff: ~%lu Hz) - Less filtering\r\n", hp_filter_coeff, cutoff_hz);
		}
		if (s_saw_ccw) {
			s_saw_ccw = 0;
			if (hp_filter_coeff < HP_FILTER_COEFF_MAX) {
				hp_filter_coeff += FILTER_COEFF_ADJUST_STEP;
				if (hp_filter_coeff > HP_FILTER_COEFF_MAX) {
					hp_filter_coeff = HP_FILTER_COEFF_MAX;
				}
			}
			else {
				hp_filter_coeff = HP_FILTER_COEFF_MAX;
			}
            u32 cutoff_hz = (hp_filter_coeff * 3035) / 100;  // 30.4 * coeff, scaled by 10 for integer math
            xil_printf("HP Filter: %lu (cutoff: ~%lu Hz) - More filtering\r\n", hp_filter_coeff, cutoff_hz);
		}
	}
	else if (adjusting_lp_filter) {
		// Adjust LP filter coefficient
		// CW = more filtering (lower coeff), CCW = less filtering (higher coeff)
		if (s_saw_cw) {
			s_saw_cw = 0;
			if (lp_filter_coeff > LP_FILTER_COEFF_MIN) {
				lp_filter_coeff -= FILTER_COEFF_ADJUST_STEP;
				if (lp_filter_coeff < LP_FILTER_COEFF_MIN) {
					lp_filter_coeff = LP_FILTER_COEFF_MIN;
				}
			} else {
				lp_filter_coeff = LP_FILTER_COEFF_MIN;
			}
            u32 cutoff_hz = (lp_filter_coeff * 3035) / 100;  // 30.4 * coeff, scaled by 10 for integer math
            xil_printf("LP Filter: %lu (cutoff: ~%lu Hz) - More filtering\r\n", lp_filter_coeff, cutoff_hz);
		}
		if (s_saw_ccw) {
			s_saw_ccw = 0;
			if (lp_filter_coeff < LP_FILTER_COEFF_MAX) {
				lp_filter_coeff += FILTER_COEFF_ADJUST_STEP;
				if (lp_filter_coeff > LP_FILTER_COEFF_MAX) {
					lp_filter_coeff = LP_FILTER_COEFF_MAX;
				}
			} else {
				lp_filter_coeff = LP_FILTER_COEFF_MAX;
			}
            u32 cutoff_hz = (lp_filter_coeff * 3035) / 100;  // 30.4 * coeff, scaled by 10 for integer math
            xil_printf("LP Filter: %lu (cutoff: ~%lu Hz) - Less filtering\r\n", lp_filter_coeff, cutoff_hz);
		}
	}
	else {
		if (s_saw_ccw) {
			s_saw_ccw  = 0;
			xil_printf("CW turn\r\n");
		}

		if (s_saw_cw) {
			s_saw_cw = 0;
			xil_printf("CCW turn\r\n");
		}
	}

	if (!enc_prev_press && (curr_press & ENC_BTN)) {
		// Encoder button pressed - toggle adjustment mode if tremolo is enabled
		if (tremolo_enabled) {
			tremolo_adjust_mode = !tremolo_adjust_mode;  // Toggle between rate and depth
			if (tremolo_adjust_mode == 0) {
				xil_printf("Tremolo: Adjusting RATE (current: %lu.%lu Hz)\r\n",
						   tremolo_rate / 10, tremolo_rate % 10);
			}
			else {
				xil_printf("Tremolo: Adjusting DEPTH (current: %lu%%)\r\n",
						   (tremolo_depth * 100) / 256);
			}
		}
		else if (chorus_enabled) {
			chorus_adjust_mode = (chorus_adjust_mode + 1) % 3;  // Cycle through: rate, delay, depth
			if (chorus_adjust_mode == 0) {
				xil_printf("Chorus: Adjusting RATE (current: %lu.%lu Hz)\r\n",
						   chorus_rate / 10, chorus_rate % 10);
			} else if (chorus_adjust_mode == 1) {
				xil_printf("Chorus: Adjusting DELAY (current: %lu samples, ~%lu ms)\r\n",
						   chorus_delay, (chorus_delay * 1000) / 48000);
			} else {
				xil_printf("Chorus: Adjusting DEPTH (current: %lu samples, ~%lu ms)\r\n",
							chorus_depth, (chorus_depth * 1000) / 48000);
			}
		}
		else {
			xil_printf("enc btn press\r\n");
		}
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
	XTmrCtr_SetResetValue(&sampling_tmr, 0, 0xFFFFFFFF - RESET_VALUE);// 2048 clk cycles @ 100MHz = 20.8 us
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
	// We match the sampling frequency: 2048 ticks
	// Side Note: we can decrease 2048 to a smaller number to increase the amount of 'pwm cycles' in one sampling cycle; this leads to a smoother signal because of analog filtering
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
