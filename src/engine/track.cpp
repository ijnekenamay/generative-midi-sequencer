#include "track.hpp"

Track::Track(uint8_t id, uint8_t channel) 
    : track_id(id), midi_channel(channel) {
    reset();
}

void Track::reset() {
    current_step = 0;
    last_played_note = 0xFF;
    is_note_active = false;
    
    // Default sensible settings
    length = 16;
    density = 4; // 4-on-the-floor kick pattern by default
    shift = 0;
    mutation_rate = 0;
    root_note = 36; // C1
    scale_type = SCALE_CHROMATIC;
    clock_divide = 6; // At 24 PPQN, divide by 6 = 16th notes
    clock_ticks = 0;
    is_muted = false;
}

void Track::set_params(uint8_t len, uint8_t dens, uint8_t shf, uint8_t mut, uint8_t root, ScaleType scale, uint8_t divide, bool muted) {
    length = (len > 0) ? len : 1;
    density = (dens <= length) ? dens : length;
    shift = shf;
    mutation_rate = (mut <= 100) ? mut : 100;
    root_note = root;
    scale_type = scale;
    clock_divide = (divide > 0) ? divide : 1;
    is_muted = muted;
}

void Track::tick(uint32_t master_tick, MidiHandler& midi) {
    // Only step the sequencer when the master tick aligns with this track's divisor
    if (master_tick % clock_divide == 0) {
        // 1. Turn off previous note before stepping (mono-legato style for techno)
        silence(midi);
        
        if (is_muted) {
            // Keep playhead moving for visual feedback, but block note execution
            current_step = (current_step + 1) % length;
            return;
        }
        
        // 2. Calculate if the current step is a Euclidean hit
        bool triggered = rhythm.calculate_step(current_step, length, density, shift);
        
        if (triggered) {
            // Step the Turing Machine to fetch a new pitch
            uint8_t raw_cv = pitch.step(mutation_rate);
            
            // Quantize pitch to the track's scale and root note
            uint8_t note = NoteGenerator::quantize(raw_cv, root_note, scale_type);
            
            // Fire MIDI note
            midi.note_on(midi_channel, note, 100);
            
            last_played_note = note;
            is_note_active = true;
        }
        
        // 3. Advance to the next step
        current_step = (current_step + 1) % length;
    }
}

void Track::silence(MidiHandler& midi) {
    if (is_note_active && last_played_note != 0xFF) {
        midi.note_off(midi_channel, last_played_note, 0);
        is_note_active = false;
    }
}
