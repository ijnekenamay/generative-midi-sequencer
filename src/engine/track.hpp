#pragma once
#include "euclidean.hpp"
#include "note_generator.hpp"
#include "midi_handler.hpp"

// Shared parameter structure representing all editable track parameters
struct TrackParams {
    volatile uint8_t length;
    volatile uint8_t density;
    volatile uint8_t shift;
    volatile uint8_t mutation;
    volatile uint8_t root_note;
    volatile uint8_t scale_type;
    volatile uint8_t clock_divide;
    volatile bool is_muted;
};

class Track {
private:
    uint8_t track_id;
    uint8_t midi_channel;
    
    // Engine components
    EuclideanGenerator rhythm;
    NoteGenerator pitch;
    
    // Sequence states
    uint8_t current_step;
    uint8_t last_played_note;
    bool is_note_active;

    // Track parameters (normally updated from Core 0 shared memory)
    uint8_t length;         // 1 to 32 steps
    uint8_t density;        // 0 to length (pulses)
    uint8_t shift;          // 0 to length (offset)
    uint8_t mutation_rate;  // 0 to 100
    uint8_t root_note;      // MIDI root note (e.g. 36 = C1)
    ScaleType scale_type;   // Chromatic, Phrygian, etc.
    uint8_t clock_divide;   // Clock division factor (1, 2, 4, 8, etc.)
    uint32_t clock_ticks;   // Master clock tick counter
    bool is_muted;

public:
    Track(uint8_t id, uint8_t channel);
    
    void reset();

    /**
     * Updates the track's parameters.
     */
    void set_params(uint8_t len, uint8_t dens, uint8_t shf, uint8_t mut, uint8_t root, ScaleType scale, uint8_t divide, bool muted);

    /**
     * Triggered on every master clock pulse (e.g. 24 ticks per quarter note).
     * 
     * @param master_tick Cumulative clock tick count.
     * @param midi Reference to the MIDI transmitter.
     * @return true if a step trigger/note-on occurred on this tick.
     */
    bool tick(uint32_t master_tick, MidiHandler& midi);

    /**
     * Forcefully shuts off any currently playing MIDI note on this track.
     */
    void silence(MidiHandler& midi);

    /**
     * Returns the current playhead step index.
     */
    uint8_t get_current_step() const { return current_step; }
};
