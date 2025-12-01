# Issue Log:
1. Needed a way to test if the PWM signal to the audio output was working  
Solution: 
- Modify the constraints (.xdc) file by remapping pin H17 (corresponds to LED 0 on the FPGA board) to the 'audio_pwm_output' signal.
- Remap pin A11 (the actual pwm output signal) to LED 0 temporarily for testing purposes, and to avoid errors when generating bitstream.
- Regenerate the bitstream in Vivado, then change the hardware specification of the platform project in Vitis to the newly generated .xsa file, clean & rebuild platform project, clean & rebuild the application project. 
- Make sure to set the options of the PWM timer (axi_timer_1) for both channels 0 and 1 to have down-counting and auto-reload on. 