#pragma once
#include <stdint.h>

class SynthEngine {
private:
    // Audio configuration
    static const uint32_t SAMPLE_RATE = 22050; // 22.05 kHz stereo
    
    // PIO & State Machine details
    uint32_t pio_instance;
    uint32_t state_machine;
    
    // Voice 0 (Kick Drum) states
    float kick_phase;
    float kick_freq;
    float kick_amp;
    
    // Voice 1 (Acid Bass) states
    float bass_phase;
    float bass_freq;
    float bass_amp;
    float bass_cutoff;
    float bass_filter_out;
    
    // Voice 2 (Hi-hat Noise) states
    uint32_t noise_register;
    float hat_amp;
    float hat_filter_out;
    
    // Voice 3 (Hypnotic Drone) states
    float drone_phase;
    float drone_freq;
    float drone_amp;

    // Custom PIO I2S Setup
    void init_pio_i2s();

public:
    SynthEngine();
    
    /**
     * Configures GPIO pins for I2S output and initiates the PIO state machine.
     * GP21 = BCLK, GP22 = LRCK, GP26 = DOUT.
     */
    void init();

    /**
     * Triggers a specific synthesizer voice.
     * 
     * @param voice_id 0 = Kick, 1 = Bass, 2 = Hi-hat, 3 = Drone.
     * @param midi_note MIDI note number (for pitch-dependent voices).
     */
    void trigger(uint8_t voice_id, uint8_t midi_note);

    /**
     * Synthesizes the next stereo audio frame (16-bit Left and 16-bit Right packed).
     * Mixes all 4 DSP voices, applies envelopes/filters, and handles dynamic scaling.
     * 
     * @return A packed 32-bit word containing Left (upper 16) and Right (lower 16) samples.
     */
    uint32_t process();

    /**
     * Streams the synthesized audio sample into the PIO hardware FIFO.
     * Blocks automatically when FIFO is full, naturally throttling Core 1 execution
     * to the physical DAC sample rate with zero CPU jitter.
     */
    void stream();
};
