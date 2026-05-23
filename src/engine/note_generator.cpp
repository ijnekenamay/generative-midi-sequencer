#include "note_generator.hpp"
#include "pico/rand.h"

// Define Scale intervals
const Scale NoteGenerator::SCALES[SCALE_COUNT] = {
    // Chromatic
    {"Chromatic",        12, {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    // Natural Minor (Aeolian)
    {"Natural Minor",     7, {0, 2, 3, 5, 7, 8, 10,  0,  0,  0,  0,  0}},
    // Phrygian (techno classic)
    {"Phrygian",          7, {0, 1, 3, 5, 7, 8, 10,  0,  0,  0,  0,  0}},
    // Dorian
    {"Dorian",            7, {0, 2, 3, 5, 7, 9, 10,  0,  0,  0,  0,  0}},
    // Minor Pentatonic (groove-friendly)
    {"Minor Pentatonic",  5, {0, 3, 5, 7, 10,  0,  0,  0,  0,  0,  0,  0}},
    // Hungarian Minor — raises the 4th and 7th: dark, exotic, cinematic
    {"Hungarian Minor",   7, {0, 2, 3, 6, 7, 8, 11,  0,  0,  0,  0,  0}},
    // Whole Tone — symmetric, dreamy, no tonal centre
    {"Whole Tone",        6, {0, 2, 4, 6, 8, 10,  0,  0,  0,  0,  0,  0}},
    // Blues — minor pentatonic + b5 blue note
    {"Blues",             6, {0, 3, 5, 6, 7, 10,  0,  0,  0,  0,  0,  0}},
    // Major Pentatonic — bright, open, East-Asian flavour
    {"Major Pentatonic",  5, {0, 2, 4, 7,  9,  0,  0,  0,  0,  0,  0,  0}},
};

NoteGenerator::NoteGenerator() {
    reset();
}

void NoteGenerator::reset() {
    // Initialize shift register with a non-zero pattern (classic pseudo-random start)
    shift_register = 0x9E3779B9; 
}

void NoteGenerator::randomize_seed() {
    // Generate a random 16-bit seed using the hardware random number generator
    shift_register = get_rand_32();
    if (shift_register == 0) {
        shift_register = 0x9E3779B9; // Avoid all zeros which lock the shift register
    }
}

uint8_t NoteGenerator::step(uint8_t mutation_rate, uint8_t length, bool* mutated) {
    if (mutated) {
        *mutated = false;
    }
    // Get the LSB (last bit of the shift register)
    bool feedback_bit = (shift_register & 0x0001) != 0;
    
    // Check if we should mutate (randomize/flip)
    // pico/rand.h: get_rand_32() returns a hardware-backed random 32-bit int
    uint32_t r = get_rand_32() % 100;
    if (r < mutation_rate) {
        feedback_bit = !feedback_bit;
        if (mutated) {
            *mutated = true;
        }
    }
    
    // Shift right and inject feedback bit into MSB (bit 15)
    // Ensure length is valid (1 to 32)
    uint8_t bit_pos = (length > 0 && length <= 32) ? (length - 1) : 15;
    
    // Shift right
    shift_register = (shift_register >> 1);
    
    // Clear the injected bit position just in case, then inject feedback bit
    shift_register &= ~(1UL << bit_pos);
    if (feedback_bit) {
        shift_register |= (1UL << bit_pos);
    }
    
    // Return the lower 8 bits of the register as a raw CV-like value
    return static_cast<uint8_t>(shift_register & 0x00FF);
}

uint8_t NoteGenerator::quantize(uint8_t raw_val, uint8_t root_note, ScaleType scale_type, uint8_t octave_range) {
    if (scale_type >= SCALE_COUNT) {
        scale_type = SCALE_CHROMATIC;
    }
    
    const Scale& scale = SCALES[scale_type];
    
    // Total possible notes within the chosen octave range
    uint16_t total_notes = scale.size * octave_range;
    
    // Map raw 8-bit (0-255) value to scale index
    uint16_t index = (static_cast<uint16_t>(raw_val) * total_notes) / 256;
    
    // Decompose index into octave and scale-degree offsets
    uint8_t octave_offset = index / scale.size;
    uint8_t scale_degree = index % scale.size;
    
    // Compute final MIDI note
    int16_t midi_note = root_note + (octave_offset * 12) + scale.intervals[scale_degree];
    
    // Clamp to valid MIDI note range
    if (midi_note < 0) return 0;
    if (midi_note > 127) return 127;
    
    return static_cast<uint8_t>(midi_note);
}

const char* NoteGenerator::get_scale_name(ScaleType scale_type) {
    if (scale_type >= SCALE_COUNT) return "Unknown";
    return SCALES[scale_type].name;
}
