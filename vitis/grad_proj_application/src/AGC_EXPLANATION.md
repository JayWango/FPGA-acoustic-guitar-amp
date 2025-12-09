# AGC (Automatic Gain Control) Explanation

## Overview
AGC automatically reduces the gain (volume) when the input signal is too loud, preventing feedback and clipping. It's like an automatic volume control that turns down when things get too loud.

---

## How It Works - Step by Step

### 1. **Calculate Input Level**
```c
int32_t input_level = (scaled_signal < 0) ? -scaled_signal : scaled_signal;
```
- Takes the absolute value of `scaled_signal`
- This gives us the magnitude (how loud) regardless of positive/negative
- Example: If `scaled_signal = -500`, then `input_level = 500`

### 2. **Check if Input Exceeds Threshold**
```c
if (input_level > AGC_THRESHOLD) {
```
- Compares input level to `AGC_THRESHOLD` (currently 300)
- If input is louder than threshold → reduce gain
- If input is quieter than threshold → restore gain

### 3. **Calculate Gain Reduction (When Too Loud)**

When `input_level > AGC_THRESHOLD`:

```c
int32_t excess = input_level - AGC_THRESHOLD;
int32_t gain_reduction = excess >> AGC_REDUCTION_RATE;
agc_gain = 256 - gain_reduction;
```

**Example Calculation:**
- `AGC_THRESHOLD = 300`
- `AGC_REDUCTION_RATE = 1` (bit shift right by 1 = divide by 2)
- If `input_level = 500`:
  - `excess = 500 - 300 = 200`
  - `gain_reduction = 200 >> 1 = 100`
  - `agc_gain = 256 - 100 = 156`

**Gain Scale:**
- `agc_gain = 256` = Full gain (100%, no reduction)
- `agc_gain = 128` = Half gain (50%, 6dB reduction)
- `agc_gain = 64` = Quarter gain (25%, 12dB reduction)
- `agc_gain = 16` = Minimum gain (`AGC_MIN_GAIN`, prevents silence)

### 4. **Apply Minimum Gain Limit**
```c
if (agc_gain < AGC_MIN_GAIN) {
    agc_gain = AGC_MIN_GAIN;  // Never go below minimum gain
}
```
- Prevents gain from going below `AGC_MIN_GAIN` (16)
- This ensures audio never goes completely silent

### 5. **Gradually Restore Gain (When Input is Quiet)**

When `input_level <= AGC_THRESHOLD`:

```c
if (agc_gain < 256) {
    agc_gain += 1;  // Slow recovery
    if (agc_gain > 256) agc_gain = 256;
}
```
- Slowly increases gain back to full (256)
- Adds 1 per sample (at 48kHz, this is very slow)
- Prevents sudden volume jumps

### 6. **Apply Gain to Signal**
```c
int32_t agc_signal = (scaled_signal * agc_gain) >> 8;
```
- Multiplies signal by gain
- `>> 8` divides by 256 (since gain is in 0-256 scale)
- Result: Signal is reduced proportionally to gain

**Example:**
- `scaled_signal = 500`
- `agc_gain = 156` (from example above)
- `agc_signal = (500 * 156) >> 8 = 78000 >> 8 = 304`
- Original: 500, After AGC: 304 (39% reduction)

---

## Key Parameters

### `AGC_THRESHOLD` (Currently 300)
- **Lower** (e.g., 100-200) = More aggressive, triggers earlier
- **Higher** (e.g., 500-1000) = Less aggressive, only triggers on very loud signals
- **Effect**: Controls when AGC starts reducing gain

### `AGC_MIN_GAIN` (Currently 16)
- **Lower** (e.g., 8-16) = Allows more aggressive reduction, better feedback prevention
- **Higher** (e.g., 64-128) = Less aggressive, maintains more volume
- **Effect**: Minimum gain level (prevents complete silence)

### `AGC_REDUCTION_RATE` (Currently 1)
- **Lower** (1) = Faster reduction, more aggressive
- **Higher** (2-4) = Slower reduction, less aggressive
- **Effect**: Controls how quickly gain reduces when threshold is exceeded
- **Formula**: `gain_reduction = excess >> AGC_REDUCTION_RATE`

---

## Visual Example

### Scenario: Input Level Gradually Increases

```
Sample 1: input_level = 200 (< 300) → agc_gain = 256 (full gain)
Sample 2: input_level = 250 (< 300) → agc_gain = 256 (full gain)
Sample 3: input_level = 350 (> 300) → agc_gain = 256 - (50>>1) = 231 (reduced)
Sample 4: input_level = 500 (> 300) → agc_gain = 256 - (200>>1) = 156 (more reduced)
Sample 5: input_level = 400 (> 300) → agc_gain = 256 - (100>>1) = 206 (less reduced)
Sample 6: input_level = 250 (< 300) → agc_gain = 207 (slowly recovering)
Sample 7: input_level = 200 (< 300) → agc_gain = 208 (still recovering)
...
```

---

## Why AGC is Important for Feedback Prevention

1. **Feedback Loop**: Microphone picks up sound → Amplifies → Exciter outputs → Microphone picks up again → Loop continues
2. **AGC Breaks the Loop**: When feedback starts, input level increases → AGC reduces gain → Less amplification → Feedback stops building
3. **Dynamic Response**: Automatically adjusts based on actual input level, not fixed settings

---

## Current Settings Analysis

```c
AGC_THRESHOLD = 300
AGC_MIN_GAIN = 16
AGC_REDUCTION_RATE = 1
```

**Behavior:**
- Triggers when input exceeds 300
- Can reduce gain down to 16 (very aggressive, ~94% reduction)
- Reduces gain very quickly (divide excess by 2)
- Good for aggressive feedback prevention

**Potential Issues:**
- With `AGC_REDUCTION_RATE = 1`, gain reduces very fast
- May cause audio to sound "pumping" or "breathing"
- Consider increasing to 2-3 for smoother operation

---

## Tuning Tips

### For More Aggressive Feedback Prevention:
```c
AGC_THRESHOLD = 200        // Trigger earlier
AGC_MIN_GAIN = 8          // Allow more reduction
AGC_REDUCTION_RATE = 1    // Fast reduction
```

### For Smoother Operation:
```c
AGC_THRESHOLD = 400       // Trigger later
AGC_MIN_GAIN = 32         // Less aggressive reduction
AGC_REDUCTION_RATE = 2    // Slower reduction
```

### For Balanced Operation:
```c
AGC_THRESHOLD = 300       // Moderate threshold
AGC_MIN_GAIN = 16         // Good balance
AGC_REDUCTION_RATE = 2    // Smooth reduction
```

