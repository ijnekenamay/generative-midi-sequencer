#include <stdio.h>
#include <cmath>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include "pico/rand.h"
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
        pio_sm_put(pio0, 0, sample);
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
uint8_t cursor_col = 0;   // 0 to 9 (SPD, LEN, DEN, SHF, MUT, JIT, GAT, ROT, SCL, RND)
volatile uint32_t cursor_move_time = 0;
volatile uint32_t screen_glitch_time = 0; // Trigger for screen-wide analog glitch flash

// --- Premium Interactive UX State Variables ---
volatile int32_t elastic_anim_timer = 0; // Elastic pop effect for cursor corner markers
float track_hit_env[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // Real-time peak envelope for steps hit (envelope decay)
volatile bool shared_track_mutated[4] = {false, false, false, false}; // Multi-core mutation indicator
float track_mut_env[4] = {0.0f, 0.0f, 0.0f, 0.0f}; // Decaying mutation visualization envelope
int disk_save_state = 0; // 0: Idle, 1: Writing, 2: Perform Flash Save, 3: Success flashing
int32_t disk_save_timer = 0; // Duration for floppy disk save state animation

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

// L-shaped shooting focus corner markers helper
void draw_corner_markers(uint16_t cell_x, uint16_t cell_y, uint16_t cell_w, uint16_t cell_h, float scale, uint16_t color) {
    float cx = (float)cell_x + (float)cell_w / 2.0f;
    float cy = (float)cell_y + (float)cell_h / 2.0f;
    float off_x = ((float)cell_w / 2.0f + 2.0f) * scale;
    float off_y = ((float)cell_h / 2.0f + 2.0f) * scale;

    int16_t lx = (int16_t)(cx - off_x + 0.5f);
    int16_t rx = (int16_t)(cx + off_x + 0.5f);
    int16_t ly = (int16_t)(cy - off_y + 0.5f);
    int16_t by = (int16_t)(cy + off_y + 0.5f);

    // Top-Left corner marker
    display.fill_rect(lx, ly, 4, 1, color);
    display.fill_rect(lx, ly, 1, 4, color);

    // Top-Right corner marker
    display.fill_rect(rx - 3, ly, 4, 1, color);
    display.fill_rect(rx, ly, 1, 4, color);

    // Bottom-Left corner marker
    display.fill_rect(lx, by, 4, 1, color);
    display.fill_rect(lx, by - 3, 1, 4, color);

    // Bottom-Right corner marker
    display.fill_rect(rx - 3, by, 4, 1, color);
    display.fill_rect(rx, by - 3, 1, 4, color);
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

    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint32_t elapsed = now - cursor_move_time;

    // Compute dynamic interactive color-to-monochrome cursor fade (Cyan -> White over 5s)
    uint16_t cursor_color = COLOR_CYAN;
    if (elapsed > 1000) {
        if (elapsed >= 5000) {
            cursor_color = COLOR_WHITE;
        } else {
            float cyan_factor = 1.0f - (float)(elapsed - 1000) / 4000.0f;
            uint8_t red = (uint8_t)(31.0f * (1.0f - cyan_factor));
            cursor_color = (red << 11) | (63 << 5) | 31;
        }
    }

    // Real-time visual decay engine (ticks, mutations, disks, elastic timers)
    static uint32_t last_draw_time = 0;
    uint32_t dt = now - last_draw_time;
    if (dt > 100) dt = 16; // clamp first run or spikes
    last_draw_time = now;

    if (elastic_anim_timer > 0) {
        elastic_anim_timer -= dt;
        if (elastic_anim_timer < 0) elastic_anim_timer = 0;
    }

    for (int i = 0; i < 4; ++i) {
        // Read and consume step hit flags from multicore
        if (shared_step_hit[i]) {
            track_hit_env[i] = 1.0f;
            shared_step_hit[i] = false; // consume
        } else {
            // Buttery-smooth exponential decay (~125ms half-life scaled by dt!)
            float decay = powf(0.80f, (float)dt / 16.0f);
            track_hit_env[i] *= decay;
            if (track_hit_env[i] < 0.01f) track_hit_env[i] = 0.0f;
        }

        // Read and consume mutation flags from multicore
        if (shared_track_mutated[i]) {
            track_mut_env[i] = 1.0f;
            shared_track_mutated[i] = false; // consume
        } else {
            // Slower buttery-smooth exponential decay (~200ms half-life!)
            float decay = powf(0.88f, (float)dt / 16.0f);
            track_mut_env[i] *= decay;
            if (track_mut_env[i] < 0.01f) track_mut_env[i] = 0.0f;
        }
    }

    if (disk_save_state == 1) {
        disk_save_timer -= dt;
        if (disk_save_timer <= 0) {
            disk_save_state = 2; // proceed to actual flash write next loop
        }
    } else if (disk_save_state == 3) {
        disk_save_timer -= dt;
        if (disk_save_timer <= 0) {
            disk_save_state = 0; // return to idle
        }
    }

    // Calculate highly authentic real-time CPU engine load representation
    float density_sum = 0;
    for (int i = 0; i < 4; ++i) {
        density_sum += draw_params[i].density;
    }
    
    // Slewing / Interpolation (1-pole low pass filter) for buttery smooth CPU scope needles!
    static float filtered_engine_load = 15.0f;
    static float filtered_ui_load = 22.0f;

    float target_engine = (density_sum / 64.0f) * 60.0f + 15.0f + (get_rand_32() % 6);
    float target_ui = 20.0f + (get_rand_32() % 10);

    filtered_engine_load += (target_engine - filtered_engine_load) * 0.15f;
    filtered_ui_load += (target_ui - filtered_ui_load) * 0.15f;

    float engine_load = filtered_engine_load;
    if (engine_load > 95.0f) engine_load = 95.0f;

    float ui_load = filtered_ui_load;
    if (ui_load < 15.0f) ui_load = 15.0f;
    if (ui_load > 45.0f) ui_load = 45.0f;

    bool glitch_active = false;
    uint16_t glitch_bg = COLOR_BLACK;
    int16_t glitch_x_offset = 0;
    float marker_scale = 1.0f;
    bool show_markers = false;

    if (elapsed < 70) {
        glitch_active = true;
        glitch_bg = COLOR_MAGENTA;
        glitch_x_offset = -3;
    } else if (elapsed < 120) {
        glitch_active = true;
        glitch_bg = COLOR_CYAN;
        glitch_x_offset = 2;
    } else if (elapsed < 180) {
        glitch_active = true;
        glitch_bg = COLOR_MAGENTA;
        glitch_x_offset = -1;
    } else if (elapsed < 230) {
        glitch_active = true;
        glitch_bg = COLOR_CYAN;
        glitch_x_offset = 0;
    } else {
        glitch_active = false;
        show_markers = true;
        if (elapsed < 350) {
            float progress = (float)(elapsed - 230) / 120.0f;
            // Buttery-smooth cubic ease-out!
            float eased = 1.0f - (1.0f - progress) * (1.0f - progress) * (1.0f - progress);
            marker_scale = 1.6f - 0.6f * eased;
        } else {
            marker_scale = 1.0f;
        }
    }

    // Apply Elastic Value Pop scaling factor (Organic Damped Cosine Spring Bounce!)
    if (elastic_anim_timer > 0) {
        float pop_progress = 1.0f - ((float)elastic_anim_timer / 150.0f); // 0.0 when popped, 1.0 when settled
        float spring = 0.4f * cosf(pop_progress * 3.14159f * 2.5f) * (1.0f - pop_progress) * (1.0f - pop_progress);
        marker_scale += spring;
        show_markers = true; // force corner markers visibility during value change
    }

    // Compute screen-wide glitch animation state variables
    uint32_t screen_elapsed = now - screen_glitch_time;
    bool screen_glitch_active = false;
    uint16_t screen_glitch_bg = COLOR_BLACK;
    int16_t screen_glitch_x_offset = 0;

    if (screen_elapsed < 70) {
        screen_glitch_active = true;
        screen_glitch_bg = COLOR_MAGENTA;
        screen_glitch_x_offset = -3;
    } else if (screen_elapsed < 120) {
        screen_glitch_active = true;
        screen_glitch_bg = COLOR_CYAN;
        screen_glitch_x_offset = 2;
    } else if (screen_elapsed < 180) {
        screen_glitch_active = true;
        screen_glitch_bg = COLOR_MAGENTA;
        screen_glitch_x_offset = -1;
    } else if (screen_elapsed < 230) {
        screen_glitch_active = true;
        screen_glitch_bg = COLOR_CYAN;
        screen_glitch_x_offset = 0;
    }

    // ----------------------------------------------------
    // 1. Draw Full-Width Header Area (Y: 0 to 48)
    // ----------------------------------------------------
    int16_t header_x_shift = screen_glitch_active ? screen_glitch_x_offset : 0;
    uint16_t header_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK;
    uint16_t header_fg = screen_glitch_active ? COLOR_BLACK : COLOR_WHITE;
    uint16_t header_lbl_color = screen_glitch_active ? COLOR_BLACK : COLOR_GREY;
    uint16_t header_sub_color = screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY;

    display.fill_rect(0, 0, 320, 48, header_bg);
    
    // Vertical BPM Label (Vaporwave Faux-Bold!)
    display.draw_text(8 + header_x_shift, 6, "B", header_lbl_color, header_bg, 1);
    display.draw_text(8 + header_x_shift + 1, 6, "B", header_lbl_color, header_bg, 1);
    display.draw_text(8 + header_x_shift, 16, "P", header_lbl_color, header_bg, 1);
    display.draw_text(8 + header_x_shift + 1, 16, "P", header_lbl_color, header_bg, 1);
    display.draw_text(8 + header_x_shift, 26, "M", header_lbl_color, header_bg, 1);
    display.draw_text(8 + header_x_shift + 1, 26, "M", header_lbl_color, header_bg, 1);

    // Master Clock Flashing Check
    bool master_flash = sequencer_playing && ((shared_master_ticks / 12) % 2 == 0);
    uint16_t bpm_bg = screen_glitch_active ? screen_glitch_bg : (master_flash ? COLOR_WHITE : COLOR_BLACK);
    uint16_t bpm_fg = screen_glitch_active ? COLOR_BLACK : (master_flash ? COLOR_BLACK : COLOR_WHITE);

    // Draw solid backplate for master clock text to enable smooth inversion flashing
    display.fill_rect(16 + header_x_shift, 8, 62, 32, bpm_bg);

    // Render large 16x24 bold digits for the actual BPM
    char bpm_str[8];
    sprintf(bpm_str, "%03d", shared_bpm);
    for (int i = 0; i < 3; ++i) {
        uint8_t digit = bpm_str[i] - '0';
        draw_bitmap(20 + i * 18 + header_x_shift, 12, 16, 24, font_16x24[digit], bpm_fg, bpm_bg);
    }

    // Draw Dual-Core CPU Load scope (X: 82 to 108)
    if (!screen_glitch_active) {
        display.draw_text(82 + header_x_shift, 6, "CPU", COLOR_DARK_GREY, header_bg, 1);
        
        // C0 Frame & Fill (Core 0 UI)
        display.draw_rect(82 + header_x_shift, 15, 26, 4, COLOR_DARK_GREY);
        int c0_len = (int)(24 * (ui_load / 100.0f));
        display.fill_rect(83 + header_x_shift, 16, c0_len, 2, COLOR_GREY);

        // C1 Frame & Fill (Core 1 Engine)
        display.draw_rect(82 + header_x_shift, 23, 26, 4, COLOR_DARK_GREY);
        int c1_len = (int)(24 * (engine_load / 100.0f));
        display.fill_rect(83 + header_x_shift, 24, c1_len, 2, COLOR_CYAN); // Neon Cyan for Real-Time DSP!
    }

    // General Settings Panel (X: 115 to 320)
    // Vertical Separator
    display.fill_rect(112 + header_x_shift, 6, 1, 36, screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY);

    // State 1: Playback State Capsule/Pill Container (OLED-style Neon Green pop when active)
    uint16_t pill_bg = screen_glitch_active ? screen_glitch_bg : (sequencer_playing ? 0x03E0 : COLOR_BLACK); 
    uint16_t pill_fg = screen_glitch_active ? COLOR_BLACK : (sequencer_playing ? COLOR_BLACK : COLOR_LIGHT_GREY);
    display.fill_rect(120 + header_x_shift, 10, 48, 16, pill_bg);
    if (!screen_glitch_active && !sequencer_playing) {
        display.draw_rect(120 + header_x_shift, 10, 48, 16, COLOR_DARK_GREY); // grey border
    }
    
    draw_bitmap(124 + header_x_shift, 14, 8, 8, sequencer_playing ? icon_play : icon_stop, pill_fg, pill_bg);
    display.draw_text(136 + header_x_shift, 14, sequencer_playing ? "RUN" : "STOP", pill_fg, pill_bg, 1);

    // State 2: Active Track Scale Mode Capsule (Monochrome OLED)
    const char* scale_names[] = {"CHROM", "MINOR", "PHRYG", "DORIN", "PENTA"};
    uint8_t current_scale_idx = draw_params[cursor_track].scale_type;
    if (current_scale_idx >= 5) current_scale_idx = 0;
    char scale_lbl[16];
    sprintf(scale_lbl, "SCL:%s", scale_names[current_scale_idx]);
    
    uint16_t scale_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK; 
    display.fill_rect(176 + header_x_shift, 10, 62, 16, scale_bg);
    if (!screen_glitch_active) {
        display.draw_rect(176 + header_x_shift, 10, 62, 16, COLOR_DARK_GREY); // Clean grey border
    }
    
    draw_bitmap(180 + header_x_shift, 14, 8, 8, icon_note, screen_glitch_active ? COLOR_BLACK : COLOR_LIGHT_GREY, scale_bg);
    display.draw_text(192 + header_x_shift, 14, scale_lbl, screen_glitch_active ? COLOR_BLACK : COLOR_WHITE, scale_bg, 1);

    // State 3: Storage Safe Command Save Capsule using floppy disk (Monochrome OLED)
    uint16_t disk_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK;
    uint8_t disk_y_offset = (disk_save_state > 0) ? 1 : 0; // slide down slightly on write!
    display.fill_rect(244 + header_x_shift, 6, 70, 24, disk_bg);
    
    if (!screen_glitch_active) {
        // Flash border when writing or saved!
        uint16_t current_disk_border = COLOR_DARK_GREY;
        if (disk_save_state == 1 || disk_save_state == 2) {
            current_disk_border = COLOR_MAGENTA; // Flashing Magenta write outline!
        } else if (disk_save_state == 3) {
            current_disk_border = COLOR_CYAN;    // Glowing Cyan save success outline!
        }
        display.draw_rect(244 + header_x_shift, 6, 70, 24, current_disk_border);
    }
    
    uint16_t disk_icon_fg = COLOR_LIGHT_GREY;
    if (disk_save_state == 1 || disk_save_state == 2) {
        disk_icon_fg = COLOR_MAGENTA;
    } else if (disk_save_state == 3) {
        disk_icon_fg = COLOR_CYAN;
    }
    
    draw_bitmap(248 + header_x_shift, 10 + disk_y_offset, 16, 16, icon_save_16x16, screen_glitch_active ? COLOR_BLACK : disk_icon_fg, disk_bg);
    
    if (disk_save_state == 0) {
        display.draw_text(268 + header_x_shift, 10, "DISK", screen_glitch_active ? COLOR_BLACK : COLOR_GREY, disk_bg, 1);
        display.draw_text(268 + header_x_shift, 19, "LT+PLY", header_sub_color, disk_bg, 1);
    } else if (disk_save_state == 1 || disk_save_state == 2) {
        display.draw_text(268 + header_x_shift, 10, "WRITE", screen_glitch_active ? COLOR_BLACK : COLOR_MAGENTA, disk_bg, 1);
        display.draw_text(268 + header_x_shift + 1, 10, "WRITE", screen_glitch_active ? COLOR_BLACK : COLOR_MAGENTA, disk_bg, 1); // Faux-bold
        display.draw_text(268 + header_x_shift, 19, "FLASH", screen_glitch_active ? COLOR_BLACK : COLOR_GREY, disk_bg, 1);
    } else if (disk_save_state == 3) {
        display.draw_text(268 + header_x_shift, 10, "SAVE", screen_glitch_active ? COLOR_BLACK : COLOR_CYAN, disk_bg, 1);
        display.draw_text(268 + header_x_shift + 1, 10, "SAVE", screen_glitch_active ? COLOR_BLACK : COLOR_CYAN, disk_bg, 1); // Faux-bold
        display.draw_text(268 + header_x_shift, 19, "OK!", screen_glitch_active ? COLOR_BLACK : COLOR_WHITE, disk_bg, 1);
    }

    // Clean bottom divider line for full-width header
    display.fill_rect(0, 47, 320, 1, screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY);

    // ----------------------------------------------------
    // 2. Draw 4 Track/Channel Strips (Y: 48 to 240, 48px each)
    // ----------------------------------------------------
    for (int trk = 0; trk < 4; ++trk) {
        uint16_t trk_y = 48 + trk * 48;
        bool is_muted = draw_params[trk].is_muted;
        bool is_active_track = (cursor_track == trk);

        int16_t row_x_shift = screen_glitch_active ? screen_glitch_x_offset : 0;
        uint16_t track_bg = COLOR_BLACK; // pure OLED black contrast!
        uint16_t row_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK;

        // Bottom divider for each row
        display.fill_rect(0, trk_y + 47, 320, 1, screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY);

        // Elektron-style Horizontal Framed highlights for active track row
        if (is_active_track && !screen_glitch_active) {
            display.fill_rect(0, trk_y + 3, 320, 1, COLOR_GREY);   // top framing line
            display.fill_rect(0, trk_y + 45, 320, 1, COLOR_GREY);  // bottom framing line
        }

        // A. Left Column: CH ID and Speed Indicator (Large bold Elektron OLED track digit!)
        display.draw_text(6 + row_x_shift, trk_y + 4, is_muted ? "MUT" : "CH", screen_glitch_active ? COLOR_BLACK : (is_muted ? COLOR_DARK_GREY : COLOR_GREY), track_bg, 1);
        
        char trk_num_str[2] = { (char)('1' + trk), '\0' };
        uint16_t trk_num_color = screen_glitch_active ? COLOR_BLACK : (is_muted ? COLOR_DARK_GREY : COLOR_WHITE);
        display.draw_text(20 + row_x_shift, trk_y + 2, trk_num_str, trk_num_color, track_bg, 2);
        display.draw_text(20 + row_x_shift + 1, trk_y + 2, trk_num_str, trk_num_color, track_bg, 2); // faux bold

        // Draw Step Hit Visual Envelope (horizontal neon cyan peak envelope line!)
        if (!screen_glitch_active && !is_muted && track_hit_env[trk] > 0.0f) {
            int line_w = (int)(12 * track_hit_env[trk]);
            display.fill_rect(28 + row_x_shift, trk_y + 8, line_w, 2, COLOR_CYAN); // Glowing Neon Cyan pulse!
        }

        if (is_muted) {
            // Draw Speaker Mute 16x16 icon in place of speed or next to it
            draw_bitmap(24 + row_x_shift, trk_y + 14, 16, 16, icon_mute_16x16, screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY, track_bg);
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

        // Inversion/flash logic for the speed multiplier
        bool speed_selected = (cursor_track == trk && cursor_col == 0);
        bool trk_hit = shared_step_hit[trk] && !is_muted;
        
        uint16_t spd_bg = track_bg;
        uint16_t spd_fg = is_muted ? COLOR_DARK_GREY : COLOR_WHITE;
        int16_t spd_draw_x = 6 + row_x_shift;

        if (screen_glitch_active) {
            spd_bg = screen_glitch_bg;
            spd_fg = COLOR_BLACK;
        } else if (speed_selected) {
            if (glitch_active) {
                spd_bg = glitch_bg;
                spd_fg = COLOR_BLACK;
                spd_draw_x = 6 + glitch_x_offset;
            } else {
                // Normal selection: trigger hit flashes white, otherwise black background
                if (trk_hit) {
                    spd_bg = COLOR_WHITE;
                    spd_fg = COLOR_BLACK;
                } else {
                    spd_bg = COLOR_BLACK;
                    spd_fg = is_muted ? COLOR_GREY : COLOR_WHITE;
                }
            }
        } else {
            // Unselected: trigger hit flashes white, otherwise standard track background
            if (trk_hit) {
                spd_bg = COLOR_WHITE;
                spd_fg = COLOR_BLACK;
            } else {
                spd_bg = track_bg;
                spd_fg = is_muted ? COLOR_DARK_GREY : COLOR_LIGHT_GREY;
            }
        }

        // Render Speed in Size 2 (Large)
        display.fill_rect(spd_draw_x, trk_y + 14, 32, 18, spd_bg);
        display.draw_text(spd_draw_x + 4, trk_y + 16, spd_str, spd_fg, spd_bg, 2);

        // Draw corner markers if speed cell is selected and glitch is done
        if (speed_selected && show_markers && !screen_glitch_active) {
            draw_corner_markers(6, trk_y + 14, 32, 18, marker_scale, cursor_color);
        }

        // Vertical boundary dividing Channel speed and parameter grid
        display.fill_rect(42 + row_x_shift, trk_y + 4, 1, 38, screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY);

        // B. Right Column: Expanded Parameter Grid Cells (Col 1 to 9 starts at X: 44)
        for (int col = 1; col <= 9; ++col) {
            uint16_t cell_x = 44 + (col - 1) * 30;
            uint16_t cell_y = trk_y + 4;

            bool is_selected = (cursor_track == trk && cursor_col == col);
            uint16_t bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK; // pure OLED black cell background!
            uint16_t fg = is_muted ? COLOR_DARK_GREY : COLOR_LIGHT_GREY;
            uint16_t border_color = screen_glitch_active ? COLOR_BLACK : (is_selected ? cursor_color : COLOR_DARK_GREY);
            int16_t draw_x = cell_x + row_x_shift;

            if (screen_glitch_active) {
                bg = screen_glitch_bg;
                fg = COLOR_BLACK;
            } else if (is_selected) {
                if (glitch_active) {
                    bg = glitch_bg;
                    fg = COLOR_BLACK;
                    draw_x = (int16_t)cell_x + glitch_x_offset;
                } else {
                    bg = COLOR_BLACK;
                    fg = is_muted ? COLOR_GREY : COLOR_WHITE;
                }
            } else {
                // Stochastic mutation breathing animation flash for MUT and RND columns!
                if (!is_muted && (col == 4 || col == 9) && track_mut_env[trk] > 0.0f) {
                    if (track_mut_env[trk] > 0.7f) {
                        bg = COLOR_WHITE;
                        fg = COLOR_BLACK;
                    } else if (track_mut_env[trk] > 0.4f) {
                        bg = COLOR_LIGHT_GREY;
                        fg = COLOR_BLACK;
                    } else {
                        bg = COLOR_DARK_GREY;
                        fg = COLOR_WHITE;
                    }
                }
            }

            // Draw parameter cell box (width 26, height 20) with clean monochrome borders
            display.fill_rect(draw_x, cell_y, 26, 20, bg);
            display.draw_rect(draw_x, cell_y, 26, 20, border_color);

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
                case 9: sprintf(val_str, "RND"); break;
            }

            // High-Contrast Monochrome hierarchy with Faux-Bold highlights and sudden Neon Cyan pop
            bool is_bold = false;
            if (screen_glitch_active) {
                fg = COLOR_BLACK;
            } else if (is_selected) {
                fg = is_muted ? COLOR_GREY : COLOR_WHITE;
                is_bold = true; // Focused cell value is always bold white!
            } else {
                switch (col) {
                    case 1: // LEN (High importance)
                    case 2: // DEN (High importance)
                    case 4: // MUT (High importance)
                        fg = is_muted ? COLOR_DARK_GREY : COLOR_WHITE; // Bold stark white!
                        is_bold = true;
                        break;
                    case 9: // RND (Sudden performative neon pop!)
                        fg = COLOR_CYAN; // Blazing Neon Cyan!
                        is_bold = true;
                        break;
                    default: // SHF, JIT, GAT, ROT, SCL (Secondary modifiers in monochrome grays)
                        fg = is_muted ? COLOR_DARK_GREY : COLOR_LIGHT_GREY;
                        is_bold = false;
                        break;
                }
            }

            // Render value with or without faux-bolding weight
            display.draw_text(draw_x + (col == 9 ? 4 : 5), cell_y + 6, val_str, fg, bg, 1);
            if (is_bold && !screen_glitch_active) {
                display.draw_text(draw_x + (col == 9 ? 4 : 5) + 1, cell_y + 6, val_str, fg, bg, 1); // Faux-bold overlay
            }
            
            // Draw corner markers if selected and glitch is done
            if (is_selected && show_markers && !screen_glitch_active) {
                draw_corner_markers(cell_x, cell_y, 26, 20, marker_scale, cursor_color);
            }

            // Draw column header label/icon at the top of Channel 1 only
            if (trk == 0) {
                const uint8_t* icon_ptr = nullptr;
                switch (col) {
                    case 1: icon_ptr = icon_len_8x8; break;
                    case 2: icon_ptr = icon_den_8x8; break;
                    case 3: icon_ptr = icon_shf_8x8; break;
                    case 4: icon_ptr = icon_mut_8x8; break;
                    case 5: icon_ptr = icon_jit_8x8; break;
                    case 6: icon_ptr = icon_gat_8x8; break;
                    case 7: icon_ptr = icon_rot_8x8; break;
                    case 8: icon_ptr = icon_scl_8x8; break;
                    case 9: icon_ptr = icon_rnd_8x8; break;
                }
                
                uint16_t header_icon_fg = screen_glitch_active ? COLOR_BLACK : COLOR_GREY;
                if (col == 9 && !screen_glitch_active) {
                    header_icon_fg = COLOR_CYAN; // Let RND icon shine in Cyan!
                }
                
                if (icon_ptr) {
                    draw_bitmap(cell_x + 9 + row_x_shift, trk_y - 12, 8, 8, icon_ptr, header_icon_fg, row_bg);
                }
            }
        }

        // C. Expanded Visual Step Playhead Strip (HD 3D-feeling sequencer track)
        uint16_t steps_y = trk_y + 34;
        display.fill_rect(44 + row_x_shift, steps_y, 266, 4, row_bg);
        
        // Draw slot track lane
        if (!screen_glitch_active) {
            display.fill_rect(44 + row_x_shift, steps_y + 1, 266, 2, COLOR_DARK_GREY); // monochrome OLED slot lane
        }

        uint8_t len = draw_params[trk].length;
        uint8_t curr = shared_current_step[trk];
        bool is_hit = shared_step_hit[trk];

        uint16_t step_w = 262 / len;
        for (uint8_t s = 0; s < len; ++s) {
            uint16_t sx = 44 + s * step_w + row_x_shift;
            uint16_t sc;
            
            if (screen_glitch_active) {
                sc = COLOR_BLACK;
            } else if (is_muted) {
                sc = (s == curr) ? COLOR_GREY : COLOR_DARK_GREY;
            } else {
                sc = (s == curr) ? (is_hit ? COLOR_WHITE : COLOR_LIGHT_GREY) : COLOR_DARK_GREY;
            }
            
            // Render active steps as 3D taller blocks to create dynamic feel
            uint16_t sh = 2; // step height
            uint16_t sy = steps_y + 1;
            if (s == curr && !screen_glitch_active) {
                sh = 4; // active step block pops up!
                sy = steps_y;
            }
            display.fill_rect(sx, sy, step_w - 1, sh, sc);
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

    // Premium Elektron-style Boot Splash Sequence
    draw_bitmap(144, 70, 32, 32, icon_splash_logo_32x32, COLOR_WHITE, COLOR_BLACK);
    display.draw_text(96, 120, "GEN-MIDI", COLOR_WHITE, COLOR_BLACK, 2);
    display.draw_text(97, 120, "GEN-MIDI", COLOR_WHITE, COLOR_BLACK, 2); // Faux-bold overlay
    display.draw_text(136, 145, "V1.0.0", COLOR_GREY, COLOR_BLACK, 1);
    display.draw_text(72, 175, "INITIALIZING ENGINE...", COLOR_DARK_GREY, COLOR_BLACK, 1);
    
    // Premium boot delay for player anticipation
    sleep_ms(1500);
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

        // Perform safe blocking QSPI Flash save when progress timer demands it
        if (disk_save_state == 2) {
            multicore_lockout_start_blocking();
            storage.save(shared_params);
            multicore_lockout_end_blocking();

            disk_save_state = 3;
            disk_save_timer = 500; // Glowing success capsule hold time
            force_redraw = true;
        }

        // Scan inputs
        input.update(dt);

        bool value_changed = false;

        uint8_t old_cursor_track = cursor_track;
        uint8_t old_cursor_col = cursor_col;

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
            if (cursor_col < 9) { cursor_col++; value_changed = true; }
        }

        if (cursor_track != old_cursor_track || cursor_col != old_cursor_col) {
            cursor_move_time = to_ms_since_boot(get_absolute_time());
            value_changed = true;
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
                // Trigger the beautiful interactive floppy progress write animation sequence
                disk_save_state = 1;
                disk_save_timer = 300; 
                value_changed = true;
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
                case 9: { // RND button - Mutate rhythm & pitch pattern!
                    uint8_t len_options[] = {8, 12, 16, 24, 32};
                    p.length = len_options[get_rand_32() % 5];
                    p.density = static_cast<uint8_t>((get_rand_32() % (p.length / 2)) + 2);
                    p.shift = static_cast<uint8_t>(get_rand_32() % p.length);
                    p.mutation = static_cast<uint8_t>(get_rand_32() % 80);
                    uint8_t spd_options[] = {3, 4, 6, 8, 12, 24};
                    p.clock_divide = spd_options[get_rand_32() % 6];
                    p.jitter = static_cast<uint8_t>(get_rand_32() % 40);
                    p.gate = static_cast<uint8_t>(30 + (get_rand_32() % 60));
                    tracks[cursor_track].randomize_pattern();
                    screen_glitch_time = to_ms_since_boot(get_absolute_time());
                    break;
                }
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
                case 9: { // RND button - Mutate rhythm & pitch pattern!
                    uint8_t len_options[] = {8, 12, 16, 24, 32};
                    p.length = len_options[get_rand_32() % 5];
                    p.density = static_cast<uint8_t>((get_rand_32() % (p.length / 2)) + 2);
                    p.shift = static_cast<uint8_t>(get_rand_32() % p.length);
                    p.mutation = static_cast<uint8_t>(get_rand_32() % 80);
                    uint8_t spd_options[] = {3, 4, 6, 8, 12, 24};
                    p.clock_divide = spd_options[get_rand_32() % 6];
                    p.jitter = static_cast<uint8_t>(get_rand_32() % 40);
                    p.gate = static_cast<uint8_t>(30 + (get_rand_32() % 60));
                    tracks[cursor_track].randomize_pattern();
                    screen_glitch_time = to_ms_since_boot(get_absolute_time());
                    break;
                }
            }
            
            shared_params[cursor_track] = p;
            unlock_shared_params(lock_save);
        }

        // Trigger Elastic Value Pop corner markers spring bounce on any value adjustment
        if (value_changed && cursor_track == old_cursor_track && cursor_col == old_cursor_col) {
            elastic_anim_timer = 150;
        }

        // Reset cursor color active timer on any changes (movement or edit) to wake up Cyan
        if (value_changed) {
            cursor_move_time = current_time;
        }

        // Keep redrawing during cursor movement animations for smooth 60fps glitch/snap-in playback
        if (current_time - cursor_move_time < 350) {
            force_redraw = true;
        }

        // Keep redrawing during screen-wide glitch flashes
        if (current_time - screen_glitch_time < 230) {
            force_redraw = true;
        }

        // Keep redrawing during the 5-second interactive cursor color fade out
        if (current_time - cursor_move_time < 5000) {
            force_redraw = true;
        }

        // Keep redrawing during elastic value pops, active triggers, and floppy progress animations
        if (elastic_anim_timer > 0 || disk_save_state > 0) {
            force_redraw = true;
        }
        for (int i = 0; i < 4; ++i) {
            if (track_hit_env[i] > 0.0f || track_mut_env[i] > 0.0f) {
                force_redraw = true;
            }
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
