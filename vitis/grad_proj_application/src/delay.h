#ifndef DELAY_H
#define DELAY_H

// this file contains all the definitions for the circular buffer

#define BUFFER_SIZE 40000 // this buffer size is statically set by the programmers; size is not dynamic

#define DELAY_SAMPLES_MIN 1000
#define DELAY_SAMPLES_MAX 38000
#define DELAY_SAMPLES_DEFAULT 8000 // this controls the default spacing between read/write head, but it gets modified in bsp.c under delay_samples variable
#define DELAY_ADJUST_STEP 1000

#define WET_MIX 192
#define DRY_MIX 62

extern volatile u32 circular_buffer[BUFFER_SIZE];
extern volatile u32 current_read_head;
extern volatile u32 write_head;

extern volatile u8 delay_enabled;
extern volatile u32 delay_samples;

#endif
