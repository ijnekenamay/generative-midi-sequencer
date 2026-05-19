#include "synth_engine.hpp"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include <math.h>

// --- Raw PIO Assembler Opcodes for Stereo 16-bit I2S ---
static const uint16_t i2s_pio_instructions[] = {
    0xe02e, //  0: set    x, 14           side 2     ; BCLK=1, LRCK=0
    0x6001, //  1: out    pins, 1         side 0     ; BCLK=0, LRCK=0
    0x0059, //  2: jmp    x--, 1          side 1     ; BCLK=1, LRCK=0
    0x6001, //  3: out    pins, 1         side 0     ; BCLK=0, LRCK=0
    0xe03e, //  4: set    x, 14           side 3     ; BCLK=1, LRCK=1
    0x6001, //  5: out    pins, 1         side 1     ; BCLK=0, LRCK=1
    0x007d, //  6: jmp    x--, 5          side 3     ; BCLK=1, LRCK=1
    0x6001  //  7: out    pins, 1         side 1     ; BCLK=0, LRCK=1
};

static const pio_program_t i2s_pio_program = {
    .instructions = i2s_pio_instructions,
    .length = 8,
    .origin = -1,
};

SynthEngine::SynthEngine() {
    pio_instance = 0; // Use PIO0
    state_machine = 0; // Use SM0
    
    // Initialize DSP states to zero
    kick_phase = 0.0f; kick_freq = 0.0f; kick_amp = 0.0f;
    bass_phase = 0.0f; bass_freq = 0.0f; bass_amp = 0.0f; bass_cutoff = 0.0f; bass_filter_out = 0.0f;
    noise_register = 0xACE1u; hat_amp = 0.0f; hat_filter_out = 0.0f;
    drone_phase = 0.0f; drone_freq = 0.0f; drone_amp = 0.0f;
}

void SynthEngine::init_pio_i2s() {
    PIO pio = pio0;
    
    // 1. Claim and load the program into PIO instruction memory
    uint offset = pio_add_program(pio, &i2s_pio_program);
    
    // 2. Set GP26 (Data) as PIO Output Pin
    pio_gpio_init(pio, 26);
    
    // 3. Set GP21 (BCLK) and GP22 (LRCK) as PIO Side-Set Pins
    pio_gpio_init(pio, 21);
    pio_gpio_init(pio, 22);
    
    // Configure pin directions
    pio_sm_set_consecutive_pindirs(pio, state_machine, 26, 1, true); // Data Out
    pio_sm_set_consecutive_pindirs(pio, state_machine, 21, 2, true); // BCLK & LRCK Out
    
    // 4. Configure State Machine settings
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + 7);
    
    // Map side-set pins (GP21, 2 bits)
    sm_config_set_sideset_pins(&c, 21);
    
    // Map data out pin (GP26, 1 bit)
    sm_config_set_out_pins(&c, 26, 1);
    
    // Configure FIFO shifting (shift right, autopull enabled, threshold = 32 bits)
    sm_config_set_out_shift(&c, true, true, 32);
    
    // Calculate Clock Divider
    // Sample Rate = 22,050 Hz
    // Bits per frame = 32 (16 left + 16 right)
    // Master Clock = 22,050 * 32 = 705,600 Hz
    // Each bit in our PIO loop takes 2 instructions.
    // PIO Target Frequency = 705,600 * 2 = 1,411,200 Hz
    // Clock Divisor = 125,000,000 / 1,411,200 = 88.577
    float div = (float)clock_get_hz(clk_sys) / 1411200.0f;
    sm_config_set_clkdiv(&c, div);
    
    // 5. Start State Machine
    pio_sm_init(pio, state_machine, offset, &c);
    pio_sm_set_enabled(pio, state_machine, true);
}

void SynthEngine::init() {
    init_pio_i2s();
}

void SynthEngine::trigger(uint8_t voice_id, uint8_t midi_note) {
    // Math to convert MIDI note number to frequency (f = 440 * 2^((note-69)/12))
    float freq = 440.0f * powf(2.0f, (static_cast<float>(midi_note) - 69.0f) / 12.0f);
    
    switch (voice_id) {
        case 0: // Kick Drum (Pitch bend, high initial amplitude)
            kick_phase = 0.0f;
            kick_freq = 160.0f; // Start at 160Hz for kick punch
            kick_amp = 1.0f;
            break;
            
        case 1: // Bass Voice (Decay env, resonant lowpass sweep)
            bass_phase = -1.0f; // Reset to sawtooth low
            bass_freq = freq;
            bass_amp = 0.6f;
            bass_cutoff = freq * 3.5f; // Open filter initially
            break;
            
        case 2: // Hi-hat Noise (Immediate click, rapid decay)
            hat_amp = 0.4f;
            break;
            
        case 3: // Slow Drone Voice (Slow attack/decay triangle wave)
            drone_phase = 0.0f;
            drone_freq = freq;
            drone_amp = 0.5f;
            break;
    }
}

uint32_t SynthEngine::process() {
    // --- 1. Synthesize Kick Voice (Sine wave pitch envelope) ---
    float kick_sample = 0.0f;
    if (kick_amp > 0.001f) {
        kick_phase += 2.0f * 3.14159f * kick_freq / static_cast<float>(SAMPLE_RATE);
        if (kick_phase > 2.0f * 3.14159f) kick_phase -= 2.0f * 3.14159f;
        
        kick_sample = sinf(kick_phase) * kick_amp;
        
        // Decay parameters
        kick_freq = kick_freq * 0.992f + 48.0f * 0.008f; // Decays down to 48 Hz (techno sub frequency)
        kick_amp *= 0.990f; // Exponential volume decay
    } else {
        kick_amp = 0.0f;
    }
    
    // --- 2. Synthesize Bass Voice (Sawtooth wave + resonant 1-pole lowpass) ---
    float bass_sample = 0.0f;
    if (bass_amp > 0.001f) {
        bass_phase += 2.0f * bass_freq / static_cast<float>(SAMPLE_RATE);
        if (bass_phase > 1.0f) bass_phase -= 2.0f;
        
        float raw_saw = bass_phase * bass_amp;
        
        // Clean 1-pole lowpass sweep filter
        bass_cutoff = bass_cutoff * 0.994f + (bass_freq * 0.8f) * 0.006f; // Slowly sweep filter down to root freq
        float alpha = bass_cutoff / static_cast<float>(SAMPLE_RATE);
        if (alpha > 0.99f) alpha = 0.99f;
        
        bass_filter_out += alpha * (raw_saw - bass_filter_out);
        bass_sample = bass_filter_out;
        
        bass_amp *= 0.993f;
    } else {
        bass_amp = 0.0f;
    }
    
    // --- 3. Synthesize Hi-hat Voice (Galois LFSR noise + Highpass filter) ---
    float hat_sample = 0.0f;
    if (hat_amp > 0.001f) {
        // Step 32-bit Galois LFSR for high quality pseudo-noise
        noise_register = (noise_register >> 1) ^ (-(noise_register & 1u) & 0xD0000001u);
        float raw_noise = ((static_cast<float>(noise_register & 0xFFFF) / 65535.0f) - 0.5f) * hat_amp;
        
        // 1-pole highpass filter to shave off low rumble
        float hp_alpha = 3000.0f / static_cast<float>(SAMPLE_RATE); // 3kHz cutoff
        hat_filter_out += hp_alpha * (raw_noise - hat_filter_out);
        
        hat_sample = raw_noise - hat_filter_out;
        hat_amp *= 0.970f; // Very fast hi-hat decay (50-80ms)
    } else {
        hat_amp = 0.0f;
    }
    
    // --- 4. Synthesize Pad Drone (Triangle wave) ---
    float drone_sample = 0.0f;
    if (drone_amp > 0.001f) {
        drone_phase += 2.0f * drone_freq / static_cast<float>(SAMPLE_RATE);
        if (drone_phase > 1.0f) drone_phase -= 2.0f;
        
        // Math to convert saw phase into clean triangle wave
        float tri_phase = (drone_phase > 0.0f ? drone_phase : -drone_phase) * 2.0f - 1.0f;
        
        drone_sample = tri_phase * drone_amp;
        drone_amp *= 0.998f; // Slow sustain decay
    } else {
        drone_amp = 0.0f;
    }
    
    // --- 5. Mix all 4 channels & clip to prevent digital clipping ---
    float mixed = (kick_sample * 0.9f) + (bass_sample * 0.4f) + (hat_sample * 0.3f) + (drone_sample * 0.2f);
    
    // Apply soft tanh-like clipping to prevent distortion spikes
    if (mixed > 1.0f) mixed = 1.0f;
    if (mixed < -1.0f) mixed = -1.0f;
    
    // Scale mixed float [-1.0, 1.0] to packed 16-bit stereo PCM
    int16_t pcm_sample = static_cast<int16_t>(mixed * 14000.0f);
    
    // Pack Left (upper 16) and Right (lower 16) stereo samples into a 32-bit word
    uint32_t packed = (static_cast<uint32_t>(pcm_sample) << 16) | (pcm_sample & 0xFFFF);
    
    return packed;
}

void SynthEngine::stream() {
    uint32_t sample = process();
    
    // Direct blocking write into the PIO TX FIFO
    // Natural throttle: if FIFO is full, blocks Core 1 execution until DAC consumes next sample.
    pio_sm_put_blocking(pio0, state_machine, sample);
}
