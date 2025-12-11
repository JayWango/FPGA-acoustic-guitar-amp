# FPGA Acoustic Guitar Amplifier

Contains 3 effects (delay, tremolo, and chorus) and ability to adjust a low-pass and high-pass filter
- delay uses a circular buffer
- tremolo uses a sine table as a LFO to modulate the amplitude of the signal
- chorus uses a sine table as a LFO to modulate the frequency of the signal (by modulating the read head of the circular buffer to access different delayed samples). The phase offset of these signals creates a pitch shifting effect 
- the low pass filter is constructed by cascading 3 digital IIR filters (effectively an elliptical filter (3rd order) as stated by Prof. Brewer)
- the high pass filter is a 1st order digital IIR filter

[Youtube Demo] (https://www.youtube.com/watch?v=MQhzvkPLK8Q&t=74s)

Completed by Jason Wang & Christopher Lai for ECE 253 - Embedded Systems Design (Graduate Version)