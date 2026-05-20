#pragma once
#include <stdint.h>

enum ScaleType {
    SCALE_CHROMATIC = 0,
    SCALE_NATURAL_MINOR,
    SCALE_PHRYGIAN,
    SCALE_DORIAN,
    SCALE_MINOR_PENTATONIC,
    SCALE_COUNT
};

struct Scale {
    const char* name;
    uint8_t size;
    uint8_t intervals[12]; // Semitone offsets from root
};

class NoteGenerator {
private:
    uint16_t shift_register; // 16-bit shift register for Turing Machine S&H
    
    // Static scales definitions
    static const Scale SCALES[SCALE_COUNT];

public:
    NoteGenerator();
    
    /**
     * Resets the shift register to a default non-zero value.
     */
    void reset();

    /**
     * Randomizes the shift register seed using hardware random generator.
     */
    void randomize_seed();

    /**
     * Steps the Turing Machine shift register.
     * Shifts bits right, and with a probability defined by mutation_rate,
     * either loops the bit or flips it.
     * 
     * @param mutation_rate Probability of mutating the looping bit (0 to 100).
     *                      0 = perfectly locked loop
     *                      100 = completely random sequence
     * @param mutated Optional pointer to boolean set to true if a random mutation occurs.
     * @return The raw 8-bit DAC-like value extracted from the register.
     */
    uint8_t step(uint8_t mutation_rate, bool* mutated = nullptr);

    /**
     * Quantizes a raw 8-bit value to a specified scale and root note.
     * 
     * @param raw_val The 8-bit value (0-255) to quantize.
     * @param root_note The MIDI root note (e.g. 36 for C1, 48 for C2).
     * @param scale_type The ScaleType enum.
     * @param octave_range The span of notes (e.g. 2 octaves).
     * @return A MIDI note number (0-127).
     */
    static uint8_t quantize(uint8_t raw_val, uint8_t root_note, ScaleType scale_type, uint8_t octave_range = 4);

    /**
     * Gets the name of a scale.
     */
    static const char* get_scale_name(ScaleType scale_type);
};
