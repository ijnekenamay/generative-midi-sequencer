#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "engine/track.hpp"
#include "engine/midi_handler.hpp"
#include "engine/storage_manager.hpp"
#include "ui/input_manager.hpp"
#include "ui/display_controller.hpp"
#include "ui/custom_assets.hpp"

// --- IPC Shared Memory Structures ---

// Spinlock for thread-safe parameter transfers between Core 0 and Core 1
spin_lock_t *shared_params_lock = nullptr;

uint32_t lock_shared_params() {
    return spin_lock_blocking(shared_params_lock);
}
void unlock_shared_params(uint32_t save) {
    spin_unlock(shared_params_lock, save);
}

// Share sequencer settings between Core 0 and Core 1
TrackParams shared_params[4] = {
    {16, 4, 0, 0, 36, SCALE_MINOR_PENTATONIC, 6, 0, 50, false}, // T1 (Kick/Bass)
    {16, 5, 2, 30, 60, SCALE_PHRYGIAN, 6, 0, 50, false},        // T2 (Stochastic Lead)
    {16, 8, 3, 10, 72, SCALE_CHROMATIC, 3, 0, 50, false},       // T3 (Hi-hats/Offbeats)
    {16, 3, 0, 15, 48, SCALE_DORIAN, 12, 0, 50, false}          // T4 (Drone/Chord)
};

// Core 1 to Core 0 feedback (for visual step indicator)
volatile uint8_t shared_current_step[4] = {0, 0, 0, 0};
volatile bool shared_step_hit[4] = {false, false, false, false};
volatile bool sequencer_playing = true;
volatile uint16_t shared_bpm = 120;
volatile uint32_t shared_master_ticks = 0; // Share tick counter for beat synchronization

// Core 1 state & objects
Track tracks[4] = {
    Track(0, 0),
    Track(1, 1),
    Track(2, 2),
    Track(3, 3)
};
MidiHandler midi;

// --- Custom PIO I2S Driver Definitions ---
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

// Background I2S sample generation variables
volatile uint32_t clock_pulse_remaining_samples = 0;
struct repeating_timer i2s_timer;

// 22.05 kHz Background Timer Interrupt Callback
bool i2s_timer_callback(struct repeating_timer *t) {
    uint32_t sample = 0;
    if (clock_pulse_remaining_samples > 0) {
        // Output maximum positive amplitude trigger pulse (Left/Right packed)
        sample = 0x7FFF7FFF;
        clock_pulse_remaining_samples--;
    } else {
        // Output zero (0V ground reference)
        sample = 0x00000000;
    }
    
    // Push the raw sample directly into the PIO I2S SM0 TX FIFO
    if (!pio_sm_is_tx_fifo_full(pio0, 0)) {
        pio_sm_put_raw(pio0, 0, sample);
    }
    return true;
}

// Configures GPIO pins and compiles the I2S state machine on PIO0
void init_pio_i2s() {
    PIO pio = pio0;
    uint state_machine = 0;
    
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
    // Sample Rate = 22,050 Hz, Bits per frame = 32 (16 left + 16 right)
    // Master Clock = 22,050 * 32 = 705,600 Hz. Each bit in PIO loop takes 2 instructions.
    // PIO Target Frequency = 1,411,200 Hz
    float div = (float)clock_get_hz(clk_sys) / 1411200.0f;
    sm_config_set_clkdiv(&c, div);
    
    // 5. Start State Machine
    pio_sm_init(pio, state_machine, offset, &c);
    pio_sm_set_enabled(pio, state_machine, true);
}

// --- Core 0 UI & Control Thread ---
InputManager input;
DisplayController display;

// Navigation States
uint8_t cursor_track = 0; // 0 to 3
uint8_t cursor_col = 0;   // 0 to 8 (SPD, LEN, DEN, SHF, MUT, JIT, GAT, ROT, SCL)

const char* column_headers[8] = {
    "LEN", "DEN", "SHF", "MUT", "JIT", "GAT", "ROT", "SCL"
};

// --- Custom 8x8 Pixel Art Bitmaps ---
const uint8_t icon_play[8] = {
    0b00000000,
    0b01000000,
    0b01100000,
    0b01110000,
    0b01111000,
    0b01110000,
    0b01100000,
    0b01000000
};

const uint8_t icon_stop[8] = {
    0b00000000,
    0b01101100,
    0b01101100,
    0b01101100,
    0b01101100,
    0b01101100,
    0b01101100,
    0b00000000
};

const uint8_t icon_note[8] = {
    0b00001100,
    0b00001110,
    0b00001011,
    0b00001001,
    0b00001000,
    0b00111000,
    0b01111000,
    0b00110000
};

// Universal bitmap drawing helper (renders 1-bit raw glyphs of ANY width and height!)
void draw_bitmap(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t* bitmap, uint16_t fg, uint16_t bg) {
    uint16_t bytes_per_row = (w + 7) / 8;
    for (uint16_t r = 0; r < h; ++r) {
        for (uint16_t b = 0; b < bytes_per_row; ++b) {
            uint8_t byte_val = bitmap[r * bytes_per_row + b];
            for (uint8_t bit = 0; bit < 8; ++bit) {
                uint16_t col = b * 8 + bit;
                if (col >= w) break;
                uint16_t color = (byte_val & (1 << (7 - bit))) ? fg : bg;
                display.fill_rect(x + col, y + r, 1, 1, color);
            }
        }
    }
}

// Render Helper
void draw_ui_dashboard() {
    // Thread-safe parameters copying under spinlock to prevent torn reads during Core 0 drawing
    TrackParams draw_params[4];
    uint32_t lock_save = lock_shared_params();
    for (int i = 0; i < 4; ++i) {
        draw_params[i] = shared_params[i];
    }
    unlock_shared_params(lock_save);

    // ----------------------------------------------------
    // 1. Draw Full-Width Header Area (Y: 0 to 48)
    // ----------------------------------------------------
    display.fill_rect(0, 0, 320, 48, COLOR_BLACK);
    
    // Vertical BPM Label
    display.draw_text(8, 6, "B", COLOR_GREY, COLOR_BLACK, 1);
    display.draw_text(8, 16, "P", COLOR_GREY, COLOR_BLACK, 1);
    display.draw_text(8, 26, "M", COLOR_GREY, COLOR_BLACK, 1);

    // Master Clock Flashing Check
    bool master_flash = sequencer_playing && ((shared_master_ticks / 12) % 2 == 0);
    uint16_t bpm_bg = master_flash ? COLOR_WHITE : COLOR_BLACK;
    uint16_t bpm_fg = master_flash ? COLOR_BLACK : COLOR_WHITE;

    // Draw solid backplate for master clock text to enable smooth inversion flashing
    display.fill_rect(16, 8, 62, 32, bpm_bg);

    // Render large 16x24 bold digits for the actual BPM
    char bpm_str[8];
    sprintf(bpm_str, "%03d", shared_bpm);
    for (int i = 0; i < 3; ++i) {
        uint8_t digit = bpm_str[i] - '0';
        draw_bitmap(20 + i * 18, 12, 16, 24, font_16x24[digit], bpm_fg, bpm_bg);
    }

    // General Settings Panel (X: 115 to 320)
    // Vertical Separator
    display.fill_rect(112, 6, 1, 36, COLOR_DARK_GREY);

    // State 1: Playback State Icon & Text
    draw_bitmap(124, 12, 8, 8, sequencer_playing ? icon_play : icon_stop, COLOR_WHITE, COLOR_BLACK);
    display.draw_text(138, 12, sequencer_playing ? "RUN" : "STOP", 
                      sequencer_playing ? COLOR_WHITE : COLOR_GREY, COLOR_BLACK, 1);

    // State 2: Active Track Scale Mode
    draw_bitmap(180, 12, 8, 8, icon_note, COLOR_WHITE, COLOR_BLACK);
    char scale_lbl[8];
    sprintf(scale_lbl, "SCL:S%d", draw_params[cursor_track].scale_type + 1);
    display.draw_text(194, 12, scale_lbl, COLOR_LIGHT_GREY, COLOR_BLACK, 1);

    // State 3: Storage Safe Command Save using the high-definition 16x16 floppy disk icon
    draw_bitmap(248, 8, 16, 16, icon_save_16x16, COLOR_WHITE, COLOR_BLACK);
    display.draw_text(268, 8, "DISK", COLOR_GREY, COLOR_BLACK, 1);
    display.draw_text(268, 18, "LT+PLY", COLOR_DARK_GREY, COLOR_BLACK, 1);

    // Clean bottom divider line for full-width header
    display.fill_rect(0, 47, 320, 1, COLOR_WHITE);

    // ----------------------------------------------------
    // 2. Draw 4 Track/Channel Strips (Y: 48 to 240, 48px each)
    // ----------------------------------------------------
    for (int trk = 0; trk < 4; ++trk) {
        uint16_t trk_y = 48 + trk * 48;
        bool is_muted = draw_params[trk].is_muted;

        // Bottom divider for each row
        display.fill_rect(0, trk_y + 47, 320, 1, COLOR_DARK_GREY);

        // A. Left Column: CH ID (MUT if muted), Speed Indicator (flashes via black/white inversion!)
        char ch_lbl[8];
        sprintf(ch_lbl, is_muted ? "MUT%d" : "CH%d", trk + 1);
        
        // Draw the text label or the beautiful speaker mute icon if muted
        if (is_muted) {
            display.draw_text(6, trk_y + 2, ch_lbl, COLOR_DARK_GREY, COLOR_BLACK, 1);
            // Draw Speaker Mute 16x16 icon in place of speed or next to it
            draw_bitmap(24, trk_y + 14, 16, 16, icon_mute_16x16, COLOR_DARK_GREY, COLOR_BLACK);
        } else {
            display.draw_text(6, trk_y + 4, ch_lbl, COLOR_GREY, COLOR_BLACK, 1);
        }

        // Format Clock Divide into speed multiplication factor (large bold size 2)
        char spd_str[6];
        uint8_t div = draw_params[trk].clock_divide;
        if (div == 24) sprintf(spd_str, "x1");
        else if (div == 12) sprintf(spd_str, "x2");
        else if (div == 6)  sprintf(spd_str, "x4");
        else if (div == 3)  sprintf(spd_str, "x8");
        else if (div == 8)  sprintf(spd_str, "x3");
        else if (div == 4)  sprintf(spd_str, "x6");
        else if (div == 48) sprintf(spd_str, "/2");
        else sprintf(spd_str, "/%d", div);

        // Inversion logic for the speed multiplier
        bool speed_selected = (cursor_track == trk && cursor_col == 0);
        bool trk_hit = shared_step_hit[trk] && !is_muted;
        
        uint16_t spd_bg, spd_fg;
        if (is_muted) {
            spd_bg = speed_selected ? COLOR_GREY : COLOR_BLACK;
            spd_fg = speed_selected ? COLOR_BLACK : COLOR_DARK_GREY;
        } else {
            bool invert_draw = speed_selected ^ trk_hit; // Flashing inversion toggle
            spd_bg = invert_draw ? COLOR_WHITE : COLOR_BLACK;
            spd_fg = invert_draw ? COLOR_BLACK : COLOR_WHITE;
        }

        // Render Speed in Size 2 (Large) with dynamic inversion (only visible if NOT muted)
        if (!is_muted) {
            display.fill_rect(6, trk_y + 14, 32, 18, spd_bg);
            display.draw_text(10, trk_y + 16, spd_str, spd_fg, spd_bg, 2);
        }

        // Vertical boundary dividing Channel speed and parameter grid
        display.fill_rect(42, trk_y + 4, 1, 38, COLOR_DARK_GREY);

        // B. Right Column: Expanded Parameter Grid Cells (Col 1 to 8 starts at X: 45)
        for (int col = 1; col <= 8; ++col) {
            uint16_t cell_x = 45 + (col - 1) * 34;
            uint16_t cell_y = trk_y + 4;

            bool is_selected = (cursor_track == trk && cursor_col == col);
            uint16_t bg, fg;
            
            if (is_muted) {
                bg = is_selected ? COLOR_GREY : COLOR_BLACK;
                fg = is_selected ? COLOR_BLACK : COLOR_DARK_GREY;
            } else {
                bg = is_selected ? COLOR_WHITE : COLOR_BLACK;
                fg = is_selected ? COLOR_BLACK : COLOR_LIGHT_GREY;
            }

            // Draw parameter cell box (width 30, height 20)
            display.fill_rect(cell_x, cell_y, 30, 20, bg);

            if (!is_selected) {
                display.draw_rect(cell_x, cell_y, 30, 20, COLOR_DARK_GREY);
            }

            char val_str[6] = "";
            switch (col) {
                case 1: sprintf(val_str, "%02d", draw_params[trk].length); break;
                case 2: sprintf(val_str, "%02d", draw_params[trk].density); break;
                case 3: sprintf(val_str, "%02d", draw_params[trk].shift); break;
                case 4: sprintf(val_str, "%02d", draw_params[trk].mutation); break;
                case 5: sprintf(val_str, "%02d", draw_params[trk].jitter); break;
                case 6: sprintf(val_str, "%02d", draw_params[trk].gate); break;
                case 7: sprintf(val_str, "%02d", draw_params[trk].root_note); break;
                case 8: sprintf(val_str, "S%d", draw_params[trk].scale_type + 1); break;
            }
            display.draw_text(cell_x + 5, cell_y + 6, val_str, fg, bg, 1);
            
            // Draw column header label at the top of Channel 1 only
            if (trk == 0) {
                display.draw_text(cell_x + 6, trk_y - 12, column_headers[col - 1], COLOR_GREY, COLOR_BLACK, 1);
            }
        }

        // C. Expanded Visual Step Playhead Strip
        uint16_t steps_y = trk_y + 34;
        display.fill_rect(45, steps_y, 268, 4, COLOR_BLACK);

        uint8_t len = draw_params[trk].length;
        uint8_t curr = shared_current_step[trk];
        bool is_hit = shared_step_hit[trk];

        uint16_t step_w = 264 / len;
        for (uint8_t s = 0; s < len; ++s) {
            uint16_t sx = 45 + s * step_w;
            uint16_t sc;
            
            if (is_muted) {
                sc = (s == curr) ? COLOR_GREY : COLOR_DARK_GREY;
            } else {
                sc = (s == curr) ? (is_hit ? COLOR_WHITE : COLOR_LIGHT_GREY) : COLOR_DARK_GREY;
            }
            display.fill_rect(sx, steps_y, step_w - 1, 2, sc);
        }
    }
}

// Core 1 Entry Point (Realtime MIDI and Generative Engine)
void core1_entry() {
    // Enable multicore lockout capability so Core 0 can safely halt Core 1 during flash erase/write
    multicore_lockout_victim_init();

    // 1. Initialize hardware UART0 MIDI OUT
    midi.init();
    
    // 2. Initialize and configure the I2S state machine on PIO0 for the analog clock sync trigger output
    init_pio_i2s();
    
    // 3. Start a repeating timer on Core 1 for the I2S clock pulse generation (22.05 kHz stereo stream)
    // pico timer with negative interval schedules execution relative to the start of the last callback, maintaining precise frequency
    add_repeating_timer_us(-45, i2s_timer_callback, NULL, &i2s_timer);
    
    // 4. Set initial parameters from shared memory thread-safely
    for (int i = 0; i < 4; ++i) {
        TrackParams local_p;
        uint32_t lock_save = lock_shared_params();
        local_p = shared_params[i];
        unlock_shared_params(lock_save);

        tracks[i].set_params(
            local_p.length,
            local_p.density,
            local_p.shift,
            local_p.mutation,
            local_p.root_note,
            (ScaleType)local_p.scale_type,
            local_p.clock_divide,
            local_p.jitter,
            local_p.gate,
            local_p.is_muted
        );
    }
    
    // Master clock loop running at shared_bpm driving 24 PPQN MIDI clocks
    // Using a precise microsecond sleep timer based on the RP2040 system clock
    uint32_t target_tick_us = (60 * 1000000) / (shared_bpm * 24);
    uint64_t next_tick_time = time_us_64() + target_tick_us;
    
    while (true) {
        // Continuous polling for scheduled MIDI events (Jitter note-on delay and Gate length note-off)
        for (int i = 0; i < 4; ++i) {
            tracks[i].update_scheduled_events(shared_bpm, midi);
        }

        if (sequencer_playing) {
            uint64_t now = time_us_64();
            
            // Re-calculate target tick microsecond duration dynamically based on potentially adjusted real-time BPM
            target_tick_us = (60 * 1000000) / (shared_bpm * 24);

            if (now >= next_tick_time) {
                // A. Send standard serial MIDI clock tick over UART0 TX (GP0)
                midi.send_clock();
                
                // B. Trigger a physical 5ms analog clock sync pulse via I2S DAC (GP26 Data, GP21 BCLK, GP22 LRCK)
                // 110 samples at 22.05 kHz sample rate yields exactly a 5.0ms high-amplitude square wave pulse
                clock_pulse_remaining_samples = 110;
                
                // C. Tick all 4 tracks to advance their playheads and generate generative MIDI notes
                for (int i = 0; i < 4; ++i) {
                    // Update track parameters live and thread-safely from Core 0 shared memory
                    TrackParams local_p;
                    uint32_t lock_save = lock_shared_params();
                    local_p = shared_params[i];
                    unlock_shared_params(lock_save);

                    tracks[i].set_params(
                        local_p.length,
                        local_p.density,
                        local_p.shift,
                        local_p.mutation,
                        local_p.root_note,
                        (ScaleType)local_p.scale_type,
                        local_p.clock_divide,
                        local_p.jitter,
                        local_p.gate,
                        local_p.is_muted
                    );
                    
                    // Tick the track and capture if a note was fired
                    bool hit = tracks[i].tick(shared_master_ticks, shared_bpm, midi);
                    
                    // Share playhead/hit indicators back to Core 0 UI thread using volatile memory
                    shared_current_step[i] = tracks[i].get_current_step();
                    shared_step_hit[i] = hit;
                }
                
                shared_master_ticks++;
                next_tick_time += target_tick_us;
            }
        }
        
        tight_loop_contents();
    }
}

int main() {
    stdio_init_all();
    sleep_ms(2000); // Settling delay for USB debug terminal
    
    printf("[Core 0] Initializing Peripherals...\n");
    input.init();
    display.init();
    display.clear(COLOR_BLACK);

    // Initialize the spinlock for safe parameter copies between Core 0 and Core 1
    shared_params_lock = spin_lock_init(spin_lock_claim_unused(true));

    // Instantiate and attempt loading settings from internal Flash
    StorageManager storage;
    storage.load(shared_params);

    printf("[Core 0] Launching Core 1 Realtime Engine...\n");
    multicore_launch_core1(core1_entry);
    
    bool force_redraw = true;
    uint32_t last_time = to_ms_since_boot(get_absolute_time());

    while (true) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        uint32_t dt = current_time - last_time;
        last_time = current_time;

        // Scan inputs
        input.update(dt);

        bool value_changed = false;

        // Handle navigation D-pad
        if (input.is_pressed(KEY_UP)) {
            if (input.is_shift_active()) {
                // Shift + UP: Increment global BPM
                shared_bpm = (shared_bpm + 5 <= 250) ? shared_bpm + 5 : 250;
                value_changed = true;
            } else {
                if (cursor_track > 0) { cursor_track--; value_changed = true; }
            }
        }
        if (input.is_pressed(KEY_DOWN)) {
            if (input.is_shift_active()) {
                // Shift + DOWN: Decrement global BPM
                shared_bpm = (shared_bpm - 5 >= 40) ? shared_bpm - 5 : 40;
                value_changed = true;
            } else {
                if (cursor_track < 3) { cursor_track++; value_changed = true; }
            }
        }
        if (input.is_pressed(KEY_LEFT)) {
            if (cursor_col > 0) { cursor_col--; value_changed = true; }
        }
        if (input.is_pressed(KEY_RIGHT)) {
            if (cursor_col < 8) { cursor_col++; value_changed = true; }
        }

        // Handle MUTE toggle (RT button toggles mute on currently selected track)
        if (input.is_pressed(KEY_RT)) {
            uint32_t lock_save = lock_shared_params();
            shared_params[cursor_track].is_muted = !shared_params[cursor_track].is_muted;
            unlock_shared_params(lock_save);
            value_changed = true;
        }

        // Handle Play/Stop toggle (and Shift + Play to save to flash)
        if (input.is_pressed(KEY_PLAY)) {
            if (input.is_shift_active()) {
                // Halts Core 1 immediately and blocks any memory access during write using Pico Lockout API
                multicore_lockout_start_blocking();
                
                storage.save(shared_params);
                
                multicore_lockout_end_blocking();
                
                // Visual confirmation: Invert title bar and display save message
                display.fill_rect(0, 0, 320, 48, COLOR_WHITE);
                display.draw_text(30, 16, "SETTINGS SAVED TO FLASH", COLOR_BLACK, COLOR_WHITE, 2);
                sleep_ms(800); // Keep message on screen for ergonomic feedback
                
                force_redraw = true;
            } else {
                sequencer_playing = !sequencer_playing;
                value_changed = true;
                if (!sequencer_playing) {
                    // Shut off all playing notes immediately on pause
                    for (int i = 0; i < 4; ++i) {
                        tracks[i].silence(midi);
                    }
                    midi.send_stop();
                } else {
                    midi.send_start();
                }
            }
        }

        // Handle parameter modifications (A: INC, B: DEC)
        int8_t step_size = input.is_shift_active() ? 10 : 1;

        if (input.is_pressed(KEY_A) || input.is_long_pressed(KEY_A)) {
            value_changed = true;
            
            uint32_t lock_save = lock_shared_params();
            TrackParams p = shared_params[cursor_track];
            
            switch (cursor_col) {
                case 0: { // Clock Divide / Speed select
                    // Standard divisions: 3, 4, 6, 8, 12, 24, 48
                    if (p.clock_divide == 3) p.clock_divide = 4;
                    else if (p.clock_divide == 4) p.clock_divide = 6;
                    else if (p.clock_divide == 6) p.clock_divide = 8;
                    else if (p.clock_divide == 8) p.clock_divide = 12;
                    else if (p.clock_divide == 12) p.clock_divide = 24;
                    else if (p.clock_divide == 24) p.clock_divide = 48;
                    else p.clock_divide = 3;
                    break;
                }
                case 1: p.length = (p.length + step_size <= 32) ? p.length + step_size : 32; break;
                case 2: p.density = (p.density + step_size <= p.length) ? p.density + step_size : p.length; break;
                case 3: p.shift = (p.shift + step_size <= 32) ? p.shift + step_size : 32; break;
                case 4: p.mutation = (p.mutation + step_size <= 100) ? p.mutation + step_size : 100; break;
                case 5: p.jitter = (p.jitter + step_size <= 100) ? p.jitter + step_size : 100; break;
                case 6: p.gate = (p.gate + step_size <= 100) ? p.gate + step_size : 100; break;
                case 7: p.root_note = (p.root_note + step_size <= 127) ? p.root_note + step_size : 127; break;
                case 8: p.scale_type = (p.scale_type + 1 < SCALE_COUNT) ? p.scale_type + 1 : 0; break;
            }
            
            shared_params[cursor_track] = p;
            unlock_shared_params(lock_save);
        }

        if (input.is_pressed(KEY_B) || input.is_long_pressed(KEY_B)) {
            value_changed = true;
            
            uint32_t lock_save = lock_shared_params();
            TrackParams p = shared_params[cursor_track];
            
            switch (cursor_col) {
                case 0: { // Clock Divide / Speed select
                    if (p.clock_divide == 48) p.clock_divide = 24;
                    else if (p.clock_divide == 24) p.clock_divide = 12;
                    else if (p.clock_divide == 12) p.clock_divide = 8;
                    else if (p.clock_divide == 8) p.clock_divide = 6;
                    else if (p.clock_divide == 6) p.clock_divide = 4;
                    else if (p.clock_divide == 4) p.clock_divide = 3;
                    else p.clock_divide = 48;
                    break;
                }
                case 1: p.length = (p.length - step_size >= 1) ? p.length - step_size : 1; break;
                case 2: p.density = (p.density - step_size >= 0) ? p.density - step_size : 0; break;
                case 3: p.shift = (p.shift - step_size >= 0) ? p.shift - step_size : 0; break;
                case 4: p.mutation = (p.mutation - step_size >= 0) ? p.mutation - step_size : 0; break;
                case 5: p.jitter = (p.jitter - step_size >= 0) ? p.jitter - step_size : 0; break;
                case 6: p.gate = (p.gate - step_size >= 10) ? p.gate - step_size : 10; break;
                case 7: p.root_note = (p.root_note - step_size >= 0) ? p.root_note - step_size : 0; break;
                case 8: p.scale_type = (p.scale_type > 0) ? p.scale_type - 1 : SCALE_COUNT - 1; break;
            }
            
            shared_params[cursor_track] = p;
            unlock_shared_params(lock_save);
        }

        // Draw UI dashboard
        if (value_changed || force_redraw || sequencer_playing) {
            draw_ui_dashboard();
            force_redraw = false;
        }

        sleep_ms(16); // ~60 Hz UI update evaluation
    }
    
    return 0;
}
