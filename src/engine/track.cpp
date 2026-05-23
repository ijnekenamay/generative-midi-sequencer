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
    prob_rate = 100; // Default: always fire (0 = never, 100 = always)
    gate_rate = 50;
    clock_ticks = 0;
    is_muted = false;
    
    has_pending_note = false;
    has_pending_note_off = false;
    pending_note_on_time = 0;
    pending_note_off_time = 0;
    pending_velocity = 100;
}

void Track::randomize_pattern() {
    pitch.randomize_seed();
}

void Track::set_params(uint8_t len, uint8_t dens, uint8_t shf, uint8_t mut, uint8_t root, ScaleType scale, uint8_t divide, uint8_t prob, uint8_t gt, bool muted) {
    length = (len > 0) ? len : 1;
    density = (dens <= length) ? dens : length;
    shift = shf;
    mutation_rate = (mut <= 100) ? mut : 100;
    root_note = root;
    scale_type = scale;
    clock_divide = (divide > 0) ? divide : 1;
    prob_rate = (prob <= 100) ? prob : 100;
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
        
        // Step the Turing Machine EVERY step to keep its loop perfectly synchronized with the rhythm
        bool mutated = false;
        uint8_t raw_cv = pitch.step(mutation_rate, length, &mutated);
        if (mutated) {
            shared_track_mutated[track_id] = true;
        }

        if (triggered) {
            // --- Probability gate (Elektron-style) ---
            // Randomly skip this Euclidean hit based on prob_rate.
            // The Turing Machine still advances every step regardless, so the
            // pitch sequence stays consistent — only the note-on is suppressed.
            bool fire = (prob_rate >= 100) ||
                        ((get_rand_32() % 100) < prob_rate);

            if (fire) {
                // Quantize pitch to the track's scale and root note
                uint8_t note = NoteGenerator::quantize(raw_cv, root_note, scale_type);

                // --- Dynamic Velocity Engine ---
                // Base velocity: derived from Turing Machine raw_cv bits.
                // raw_cv naturally repeats at low mutation (locked groove) and randomizes
                // at high mutation, so velocity organically follows the same pattern.
                // Range: 50-115 before accent boost.
                uint8_t vel = 50 + ((raw_cv >> 1) & 0x3F); // 50..113

                // Metric accent: boost velocity at musically important positions.
                // Step 0 is the downbeat — always the strongest accent.
                // Quarter-note positions (length/4 multiples) get a lighter accent.
                bool is_downbeat = (current_step == 0);
                bool is_quarter  = (length >= 4) && (current_step > 0) &&
                                   ((current_step % (length / 4)) == 0);

                if (is_downbeat) {
                    vel = (vel + 14 > 127) ? 127 : vel + 14;  // strong downbeat, 64..127
                } else if (is_quarter) {
                    vel = (vel + 7  > 118) ? 118 : vel + 7;   // quarter accent, 57..118
                }
                // Unaccented off-beats stay in the 50-113 range — quieter, sitting back in
                // the mix and adding natural perceived depth without any new UI parameter.
                pending_velocity = vel;
                // --------------------------------

                // Schedule the Note-On event with no jitter delay
                pending_note_on_time = time_us_64();
                pending_note = note;
                has_pending_note = true;
            }
        }
        
        // Advance to the next step
        current_step = (current_step + 1) % length;
        // Return true only when a note was actually scheduled
        // (Euclidean hit AND probability gate both passed)
        return has_pending_note;
    }
    return false;
}

void Track::update_scheduled_events(uint32_t bpm, MidiHandler& midi) {
    uint64_t now = time_us_64();
    
    // 1. Dispatch scheduled Note-On
    if (has_pending_note && now >= pending_note_on_time) {
        // Double-check: turn off previous active note (mono-legato override)
        silence(midi);
        
        midi.note_on(midi_channel, pending_note, pending_velocity);
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
