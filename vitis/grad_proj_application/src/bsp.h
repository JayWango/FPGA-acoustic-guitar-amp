#ifndef BSP_H
#define BSP_H

#include "xtmrctr.h" // timer/counter driver
#include "xintc.h" // 	interrupt controller driver
#include "xparameters.h"
#include "xtmrctr_l.h" // low-level driver for timer/counter
#include "xintc_l.h" //	low-level driver for interrupt controller
#include "mb_interface.h" //
#include <xbasic_types.h> //
#include <xio.h> // provides I/O utility macros for r/w to hardware registers

#define RESET_VALUE 2048 // modify this to change frequency of sampling_ISR()
#define SAMPLES 5

// Filter coefficient ranges (0-256 scale)
#define HP_FILTER_COEFF_MIN  1   // less filtering (removes less low frequencies)
#define HP_FILTER_COEFF_MAX  256  // more filtering (removes more low frequencies)
#define HP_FILTER_COEFF_DEFAULT  10

#define LP_FILTER_COEFF_MIN  1   // More filtering (removes more high frequencies)
#define LP_FILTER_COEFF_MAX  256 // Less filtering (removes fewer high frequencies)
#define LP_FILTER_COEFF_DEFAULT  90

#define FILTER_COEFF_ADJUST_STEP  2  // Step size for encoder adjustment

// Filter coefficients (adjustable via buttons)
extern volatile u16 hp_filter_coeff;
extern volatile u16 lp_filter_coeff;

// Filter adjustment mode flags
extern volatile u8 adjusting_hp_filter;
extern volatile u8 adjusting_lp_filter;

#define INPUT_LIMIT_THRESHOLD 400
#define OUTPUT_LIMIT_THRESHOLD 400

// defines for 5 pushbuttons
#define BTN_MIDDLE  BTN4_MASK
#define BTN_RIGHT   BTN2_MASK
#define BTN_LEFT    BTN1_MASK
#define BTN_TOP     BTN0_MASK
#define BTN_BOTTOM  BTN3_MASK
#define BTN0_MASK   0x01  // bit 0 --> (BTNU on fpga board)
#define BTN1_MASK   0x02  // bit 1 --> (BTNL on fpga board)
#define BTN2_MASK   0x04  // bit 2 --> (BTNR on fpga board)
#define BTN3_MASK   0x08  // bit 3 --> (BTND on fpga board)
#define BTN4_MASK   0x10  // bit 4 --> (BTNC on fpga board)
#define DEBOUNCE_TIME 8000 // button presses only registered every ~0.5 sec

// defines for encoders
#define ENC_A       0x01
#define ENC_B       0x02
#define ENC_BTN     0x04

// =====================================================
// logging variables (declared as extern)
extern volatile u32 sys_tick_counter;
extern volatile int32_t curr_sample;

extern volatile int32_t tiny_buffer[SAMPLES];

void BSP_init();

// encoder
void init_btn_gpio();
void init_enc_gpio();
void pushBtn_ISR(void *CallbackRef);
void enc_ISR(void *CallbackRef);

// axi_timer_0
int init_sampling_timer();
void sampling_ISR();

// axi_timer_1
int init_pwm_timer();

#endif
