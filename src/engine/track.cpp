#include "track.hpp"
#include "pico/rand.h"
#include "pico/time.h"

extern volatile bool shared_track_mutated[4];

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
    jitter_rate = 0;
    gate_rate = 50;
    clock_ticks = 0;
    is_muted = false;
    
    has_pending_note = false;
    has_pending_note_off = false;
    pending_note_on_time = 0;
    pending_note_off_time = 0;
}

void Track::randomize_pattern() {
    pitch.randomize_seed();
}

void Track::set_params(uint8_t len, uint8_t dens, uint8_t shf, uint8_t mut, uint8_t root, ScaleType scale, uint8_t divide, uint8_t jit, uint8_t gt, bool muted) {
    length = (len > 0) ? len : 1;
    density = (dens <= length) ? dens : length;
    shift = shf;
    mutation_rate = (mut <= 100) ? mut : 100;
    root_note = root;
    scale_type = scale;
    clock_divide = (divide > 0) ? divide : 1;
    jitter_rate = (jit <= 100) ? jit : 100;
    gate_rate = (gt == 0 || (gt >= 10 && gt <= 100)) ? gt : 50;
    is_muted = muted;
}

bool Track::tick(uint32_t master_tick, uint32_t bpm, MidiHandler& midi) {
    // Only step the sequencer when the master tick aligns with this track's divisor
    if (master_tick % clock_divide == 0) {
        // If there was a pending note-off from the last step, clear it immediately
        if (has_pending_note_off) {
            silence(midi);
            has_pending_note_off = false;
        }
        
        if (is_muted) {
            // Keep playhead moving for visual feedback, but block note execution
            current_step = (current_step + 1) % length;
            return false;
        }
        
        // Calculate if the current step is a Euclidean hit
        bool triggered = rhythm.calculate_step(current_step, length, density, shift);
        
        if (triggered) {
            // Step the Turing Machine to fetch a new pitch
            bool mutated = false;
            uint8_t raw_cv = pitch.step(mutation_rate, &mutated);
            if (mutated) {
                shared_track_mutated[track_id] = true;
            }
            
            // Quantize pitch to the track's scale and root note
            uint8_t note = NoteGenerator::quantize(raw_cv, root_note, scale_type);
            
            // Calculate stochastic microtiming jitter delay
            uint64_t jitter_us = 0;
            if (jitter_rate > 0) {
                // Up to (jitter_rate * 150) microseconds of delay
                jitter_us = get_rand_32() % (static_cast<uint64_t>(jitter_rate) * 150ULL);
            }
            
            // Schedule the Note-On event asynchronously
            pending_note_on_time = time_us_64() + jitter_us;
            pending_note = note;
            has_pending_note = true;
        }
        
        // Advance to the next step
        current_step = (current_step + 1) % length;
        return triggered;
    }
    return false;
}

void Track::update_scheduled_events(uint32_t bpm, MidiHandler& midi) {
    uint64_t now = time_us_64();
    
    // 1. Dispatch scheduled Note-On (after Jitter delay)
    if (has_pending_note && now >= pending_note_on_time) {
        // Double-check: turn off previous active note (mono-legato override)
        silence(midi);
        
        midi.note_on(midi_channel, pending_note, 100);
        last_played_note = pending_note;
        is_note_active = true;
        has_pending_note = false;
        
        // Calculate precise step duration for gate calculation
        uint64_t step_duration_us = (60ULL * 1000000ULL * clock_divide) / (bpm * 24ULL);
        
        // Calculate base gate duration based on gate percentage
        uint8_t actual_gate = gate_rate;
        if (gate_rate == 0) {
            // SL mode: dynamically randomize between Staccato (15) and Legato (95)
            actual_gate = (get_rand_32() % 2 == 0) ? 15 : 95;
        }
        uint64_t base_gate_us = (step_duration_us * actual_gate) / 100ULL;
        
        // Add random gate length variation (up to 30% of the base gate length)
        uint64_t max_var = base_gate_us / 3ULL;
        uint64_t variation = (max_var > 0) ? (get_rand_32() % max_var) : 0ULL;
        uint64_t gate_duration_us = base_gate_us + variation - (max_var / 2ULL);
        if (gate_duration_us < 5000ULL) {
            gate_duration_us = 5000ULL; // Ensure at least 5ms to trigger synth voice
        }
        
        // Schedule Note-Off
        pending_note_off_time = time_us_64() + gate_duration_us;
        has_pending_note_off = true;
    }
    
    // 2. Dispatch scheduled Note-Off (after Gate duration expiry)
    if (has_pending_note_off && now >= pending_note_off_time) {
        silence(midi);
        has_pending_note_off = false;
    }
}

void Track::silence(MidiHandler& midi) {
    if (is_note_active && last_played_note != 0xFF) {
        midi.note_off(midi_channel, last_played_note, 0);
        is_note_active = false;
    }
}
