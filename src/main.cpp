#include <stdio.h>
#include <cmath>
#include <functional>
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
volatile bool shared_track_mutated[4] = {false, false, false, false};
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
    
    // 2. Set GP17 (Data) as PIO Output Pin (GY-PCM5102 DIN)
    pio_gpio_init(pio, 17);
    
    // 3. Set GP18 (BCLK) and GP19 (LRCK) as PIO Side-Set Pins (GY-PCM5102 BCK & LRCK)
    pio_gpio_init(pio, 18);
    pio_gpio_init(pio, 19);
    
    // Configure pin directions
    pio_sm_set_consecutive_pindirs(pio, state_machine, 17, 1, true); // Data Out
    pio_sm_set_consecutive_pindirs(pio, state_machine, 18, 2, true); // BCLK & LRCK Out
    
    // 4. Configure State Machine settings
    pio_sm_config c = pio_get_default_sm_config();
    sm_config_set_wrap(&c, offset + 0, offset + 7);
    
    // Map side-set pins (GP18, 2 bits)
    sm_config_set_sideset_pins(&c, 18);
    
    // Map data out pin (GP17, 1 bit)
    sm_config_set_out_pins(&c, 17, 1);
    
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
volatile bool force_redraw = true;       // Global UI force redraw flag

// --- Interactive Control State Variables (Cleaned) ---
int disk_save_state = 0; // 0: Idle, 1: Writing, 2: Perform Flash Save, 3: Success flashing

// --- Custom 32x32 High-Definition Pixel Art Bitmaps ---
const uint8_t icon_play_32x32[128] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x0C, 0x00, 0x00,
    0x00, 0x1C, 0x00, 0x00,
    0x00, 0x3C, 0x00, 0x00,
    0x00, 0x7C, 0x00, 0x00,
    0x00, 0xFC, 0x00, 0x00,
    0x01, 0xFC, 0x00, 0x00,
    0x03, 0xFC, 0x00, 0x00,
    0x07, 0xFC, 0x00, 0x00,
    0x0F, 0xFC, 0x00, 0x00,
    0x1F, 0xFC, 0x00, 0x00,
    0x3F, 0xFC, 0x00, 0x00,
    0x7F, 0xFC, 0x00, 0x00,
    0xFF, 0xFC, 0x00, 0x00,
    0xFF, 0xFC, 0x00, 0x00,
    0xFF, 0xFC, 0x00, 0x00,
    0xFF, 0xFC, 0x00, 0x00,
    0x7F, 0xFC, 0x00, 0x00,
    0x3F, 0xFC, 0x00, 0x00,
    0x1F, 0xFC, 0x00, 0x00,
    0x0F, 0xFC, 0x00, 0x00,
    0x07, 0xFC, 0x00, 0x00,
    0x03, 0xFC, 0x00, 0x00,
    0x01, 0xFC, 0x00, 0x00,
    0x00, 0xFC, 0x00, 0x00,
    0x00, 0x7C, 0x00, 0x00,
    0x00, 0x3C, 0x00, 0x00,
    0x00, 0x1C, 0x00, 0x00,
    0x00, 0x0C, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

const uint8_t icon_stop_32x32[128] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0x00,
    0x00, 0xFF, 0xFF, 0x00,
    0x01, 0xFF, 0xFF, 0x80,
    0x03, 0xFF, 0xFF, 0xC0,
    0x07, 0xFF, 0xFF, 0xE0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x0F, 0xFF, 0xFF, 0xF0,
    0x07, 0xFF, 0xFF, 0xE0,
    0x03, 0xFF, 0xFF, 0xC0,
    0x01, 0xFF, 0xFF, 0x80,
    0x00, 0xFF, 0xFF, 0x00,
    0x00, 0xFF, 0xFF, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

const uint8_t icon_note_32x32[128] = {
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x03, 0xFF,
    0x00, 0x00, 0x03, 0xFF,
    0x00, 0x00, 0x03, 0xFF,
    0x00, 0x00, 0x03, 0xCD,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x00, 0x03, 0xC8,
    0x00, 0x03, 0x83, 0xC8,
    0x00, 0x07, 0xC3, 0xC8,
    0x00, 0x0F, 0xE3, 0xC8,
    0x00, 0x1F, 0xE3, 0xC8,
    0x00, 0x1F, 0xE3, 0xC8,
    0x00, 0x0F, 0xC3, 0xC0,
    0x00, 0x00, 0x03, 0xC0,
    0x03, 0x80, 0x00, 0x00,
    0x07, 0xC0, 0x00, 0x00,
    0x0F, 0xE0, 0x00, 0x00,
    0x1F, 0xE0, 0x00, 0x00,
    0x1F, 0xE0, 0x00, 0x00,
    0x0F, 0xC0, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
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

    uint32_t now = to_ms_since_boot(get_absolute_time());
    uint16_t cursor_color = COLOR_CYAN;

    // Static clean system loads
    float engine_load = 18.0f;
    float ui_load = 22.0f;

    bool glitch_active = false;
    uint16_t glitch_bg = COLOR_BLACK;
    int16_t glitch_x_offset = 0;
    float marker_scale = 1.0f;
    bool show_markers = true;

    bool screen_glitch_active = false;
    uint16_t screen_glitch_bg = COLOR_BLACK;
    int16_t screen_glitch_x_offset = 0;

    // ----------------------------------------------------
    // 1. Draw Full-Width Header Area (Y: 0 to 48)
    // ----------------------------------------------------
    int16_t header_x_shift = screen_glitch_active ? screen_glitch_x_offset : 0;
    uint16_t header_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK;
    uint16_t header_fg = screen_glitch_active ? COLOR_BLACK : COLOR_WHITE;
    uint16_t header_lbl_color = screen_glitch_active ? COLOR_BLACK : COLOR_GREY;
    uint16_t header_sub_color = screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY;

    // --- Header Flicker Optimization & Partial Updates ---
    static bool last_master_flash = false;
    static int last_disk_save_state = -1;
    static bool last_sequencer_playing = false;
    static uint8_t last_scale_idx = 255;
    static uint16_t last_bpm = 0;
    static uint8_t last_cursor_track = 255;
    static uint32_t last_cpu_draw_time = 0;

    bool master_flash = sequencer_playing && ((shared_master_ticks / 12) % 2 == 0);
    uint8_t current_scale_idx = draw_params[cursor_track].scale_type;
    if (current_scale_idx >= 5) current_scale_idx = 0;

    bool header_needs_update = force_redraw || screen_glitch_active ||
                               (master_flash != last_master_flash) ||
                               (disk_save_state != last_disk_save_state) ||
                               (sequencer_playing != last_sequencer_playing) ||
                               (current_scale_idx != last_scale_idx) ||
                               (shared_bpm != last_bpm) ||
                               (cursor_track != last_cursor_track) ||
                               (now - last_cpu_draw_time > 1000);

    if (header_needs_update) {
        last_master_flash = master_flash;
        last_disk_save_state = disk_save_state;
        last_sequencer_playing = sequencer_playing;
        last_scale_idx = current_scale_idx;
        last_bpm = shared_bpm;
        last_cursor_track = cursor_track;
        if (now - last_cpu_draw_time > 1000) last_cpu_draw_time = now;

        // Only fill the entire header background during glitches or forced/initial redraws
        // to prevent heavy SPI bus flooding and extreme human-visible screen flickering
        if (screen_glitch_active || force_redraw) {
            display.fill_rect(0, 0, 320, 48, header_bg);
        }
        
        // Vertical BPM Label (Vaporwave Faux-Bold!) - Realigned Downward
        display.draw_text(8 + header_x_shift, 11, "B", header_lbl_color, header_bg, 1);
        display.draw_text(8 + header_x_shift + 1, 11, "B", header_lbl_color, header_bg, 1);
        display.draw_text(8 + header_x_shift, 21, "P", header_lbl_color, header_bg, 1);
        display.draw_text(8 + header_x_shift + 1, 21, "P", header_lbl_color, header_bg, 1);
        display.draw_text(8 + header_x_shift, 31, "M", header_lbl_color, header_bg, 1);
        display.draw_text(8 + header_x_shift + 1, 31, "M", header_lbl_color, header_bg, 1);

        // Master Clock Flashing Check
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

        // ----------------------------------------------------
        // General Settings Panel (Left-Shifted Layout after removing CPU/Debug)
        // ----------------------------------------------------
        
        // Vertical Separator (Moved left to X: 90)
        display.fill_rect(90 + header_x_shift, 6, 1, 36, screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY);

        // State 1: Playback State Capsule/Pill Container (OLED-style Neon Green pop when active)
        uint16_t pill_bg = screen_glitch_active ? screen_glitch_bg : (sequencer_playing ? 0x03E0 : COLOR_BLACK); 
        uint16_t pill_fg = screen_glitch_active ? COLOR_BLACK : (sequencer_playing ? COLOR_BLACK : COLOR_LIGHT_GREY);
        display.fill_rect(96 + header_x_shift, 12, 58, 24, pill_bg);
        if (!screen_glitch_active && !sequencer_playing) {
            display.draw_rect(96 + header_x_shift, 12, 58, 24, COLOR_DARK_GREY); // grey border
        }
        
        draw_bitmap(98 + header_x_shift, 16, 16, 16, icon_play_pause_16x16, pill_fg, pill_bg);
        display.draw_text(118 + header_x_shift, 20, sequencer_playing ? "RUN" : "STOP", pill_fg, pill_bg, 1);

        // State 2: Active Track Scale Mode Capsule (Monochrome OLED)
        const char* scale_names[] = {"CHROM", "MINOR", "PHRYG", "DORIN", "PENTA"};
        char scale_lbl[16];
        sprintf(scale_lbl, "SCL:%s", scale_names[current_scale_idx]);
        
        uint16_t scale_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK; 
        display.fill_rect(162 + header_x_shift, 12, 112, 24, scale_bg);
        if (!screen_glitch_active) {
            display.draw_rect(162 + header_x_shift, 12, 112, 24, COLOR_DARK_GREY); // Clean grey border
        }
        
        draw_bitmap(168 + header_x_shift, 16, 16, 16, icon_note_16x16, screen_glitch_active ? COLOR_BLACK : COLOR_LIGHT_GREY, scale_bg);
        display.draw_text(188 + header_x_shift, 20, scale_lbl, screen_glitch_active ? COLOR_BLACK : COLOR_WHITE, scale_bg, 1);

        // State 3: Storage Safe Command Save Capsule using floppy disk (Icon-Only Mini-Capsule!)
        uint16_t disk_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK;
        uint8_t disk_y_offset = (disk_save_state > 0) ? 1 : 0; // slide down slightly on write!
        display.fill_rect(284 + header_x_shift, 12, 30, 24, disk_bg);
        
        if (!screen_glitch_active) {
            // Flash border when writing or saved!
            uint16_t current_disk_border = COLOR_DARK_GREY;
            if (disk_save_state == 1 || disk_save_state == 2) {
                current_disk_border = COLOR_MAGENTA; // Flashing Magenta write outline!
            } else if (disk_save_state == 3) {
                current_disk_border = COLOR_CYAN;    // Glowing Cyan save success outline!
            }
            display.draw_rect(284 + header_x_shift, 12, 30, 24, current_disk_border);
        }
        
        uint16_t disk_icon_fg = COLOR_LIGHT_GREY;
        if (disk_save_state == 1 || disk_save_state == 2) {
            disk_icon_fg = COLOR_MAGENTA;
        } else if (disk_save_state == 3) {
            disk_icon_fg = COLOR_CYAN;
        }
        
        draw_bitmap(291 + header_x_shift, 16 + disk_y_offset, 16, 16, icon_save_16x16, screen_glitch_active ? COLOR_BLACK : disk_icon_fg, disk_bg);

        // Clean bottom divider line for full-width header
        display.fill_rect(0, 47, 320, 1, screen_glitch_active ? COLOR_BLACK : COLOR_DARK_GREY);
    }

    // ----------------------------------------------------
    // 2. Draw 4 Track/Channel Strips (Y: 66 to 234, 42px each)
    // ----------------------------------------------------
    for (int trk = 0; trk < 4; ++trk) {
        uint16_t trk_y = 66 + trk * 42;
        bool is_muted = draw_params[trk].is_muted;
        bool is_active_track = (cursor_track == trk);

        int16_t row_x_shift = screen_glitch_active ? screen_glitch_x_offset : 0;
        uint16_t track_bg = COLOR_BLACK; // pure OLED black contrast!
        uint16_t row_bg = screen_glitch_active ? screen_glitch_bg : COLOR_BLACK;

        // Elektron-style active track indication (No explicit lines, keeping pure borderless OLED layout!)

        // A. Left Column: CH ID and Speed Indicator (Large bold Elektron OLED track digit!)
        display.draw_text(6 + row_x_shift, trk_y + 6, is_muted ? "MUT" : "CH", screen_glitch_active ? COLOR_BLACK : (is_muted ? COLOR_DARK_GREY : COLOR_GREY), track_bg, 1);
        
        char trk_num_str[2] = { (char)('1' + trk), '\0' };
        uint16_t trk_num_color = screen_glitch_active ? COLOR_BLACK : (is_muted ? COLOR_DARK_GREY : COLOR_WHITE);
        display.draw_text(24 + row_x_shift, trk_y + 6, trk_num_str, trk_num_color, track_bg, 1);
        display.draw_text(24 + row_x_shift + 1, trk_y + 6, trk_num_str, trk_num_color, track_bg, 1); // faux bold



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

        // Render Speed in Size 2 (Large bold geometric view - Maximum Left-Aligned!)
        display.fill_rect(spd_draw_x, trk_y + 14, 32, 18, spd_bg);
        display.draw_text(spd_draw_x, trk_y + 15, spd_str, spd_fg, spd_bg, 2);

        // Render pure monochrome cursor border if speed cell is active
        if (speed_selected) {
            display.draw_rect(spd_draw_x, trk_y + 14, 32, 18, cursor_color);
        }

        // Vertical boundary dividing Channel speed and parameter grid (Removed for pure borderless flat OLED look!)

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
                case 9: sprintf(val_str, "RN"); break;
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

            // Render value with or without faux-bolding weight (Precision Centering unified at X offset: 4)
            uint8_t text_x_offset = 4;
            display.draw_text(draw_x + text_x_offset, cell_y + 6, val_str, fg, bg, 1);
            if (is_bold && !screen_glitch_active) {
                display.draw_text(draw_x + text_x_offset + 1, cell_y + 6, val_str, fg, bg, 1); // Faux-bold overlay
            }
            
            // Simple border highlight already drawn in line 517

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
                    draw_bitmap(cell_x + 5 + row_x_shift, trk_y - 14, 16, 16, icon_ptr, header_icon_fg, row_bg);
                }
            }
        }

        // C. Expanded Visual Step Playhead Strip (HD 3D-feeling sequencer track)
        uint16_t steps_y = trk_y + 32;
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
            
            EuclideanGenerator rhythm_calc;
            bool is_active_beat = rhythm_calc.calculate_step(s, len, draw_params[trk].density, draw_params[trk].shift);

            if (screen_glitch_active) {
                sc = COLOR_BLACK;
            } else if (s == curr) {
                sc = is_muted ? COLOR_GREY : (is_hit ? COLOR_WHITE : COLOR_LIGHT_GREY);
            } else {
                if (is_active_beat) {
                    sc = is_muted ? COLOR_DARK_GREY : COLOR_CYAN; // Glow in beautiful Neon Cyan!
                } else {
                    sc = COLOR_DARK_GREY;
                }
            }
            
            // Render active steps as 3D taller blocks to create dynamic rhythm hills
            uint16_t sh = 2; // step height
            uint16_t sy = steps_y + 1;
            if (s == curr && !screen_glitch_active) {
                sh = 4; // active playhead block pops up!
                sy = steps_y;
            } else if (is_active_beat && !screen_glitch_active) {
                sh = 3; // scheduled beat block is slightly elevated!
                sy = steps_y + 1;
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
    
    // Thread-safe parameters cache to completely eliminate spinlock contention delay during critical ticks
    TrackParams local_params_cache[4];
    for (int i = 0; i < 4; ++i) {
        local_params_cache[i] = shared_params[i]; // safe initial load
    }
    uint32_t last_cache_update_ms = 0;

    while (true) {
        // Safe parameter cache update at a relaxed rate (approx 4ms) to completely shield real-time ticks
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - last_cache_update_ms >= 4) {
            uint32_t lock_save = lock_shared_params();
            for (int i = 0; i < 4; ++i) {
                local_params_cache[i] = shared_params[i];
            }
            unlock_shared_params(lock_save);
            last_cache_update_ms = now_ms;
        }

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
                clock_pulse_remaining_samples = 110;
                
                // C. Tick all 4 tracks to advance their playheads using spinlock-free cached parameters
                for (int i = 0; i < 4; ++i) {
                    TrackParams local_p = local_params_cache[i];

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

// Static state for physical key auto-repeat
uint32_t key_repeat_timers[KEY_COUNT] = {0};
bool key_was_held[KEY_COUNT] = {false};

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
    
    force_redraw = true;
    uint32_t last_time = to_ms_since_boot(get_absolute_time());

    while (true) {
        uint32_t current_time = to_ms_since_boot(get_absolute_time());
        uint32_t dt = current_time - last_time;
        last_time = current_time;

        // Perform safe blocking QSPI Flash save instantly
        if (disk_save_state == 2) {
            multicore_lockout_start_blocking();
            storage.save(shared_params);
            multicore_lockout_end_blocking();

            disk_save_state = 0; // Immediately return to idle
            force_redraw = true;
        }

        // Scan inputs
        input.update(dt);

        bool value_changed = false;

        uint8_t old_cursor_track = cursor_track;
        uint8_t old_cursor_col = cursor_col;

        // High-Speed Autorepeat Navigation Handler
        auto handle_nav_key = [&](KeyIndex key, std::function<void()> press_action) {
            if (input.is_held(key)) {
                uint32_t now = to_ms_since_boot(get_absolute_time());
                if (!key_was_held[key]) {
                    // Initial instant trigger!
                    press_action();
                    key_repeat_timers[key] = now + 180; // Sharp 180ms hold delay!
                    key_was_held[key] = true;
                } else if (now >= key_repeat_timers[key]) {
                    // Repeat trigger!
                    press_action();
                    key_repeat_timers[key] = now + 50; // Super-fast 50ms repeat rate!
                }
            } else {
                key_was_held[key] = false;
            }
        };

        // Handle navigation D-pad with responsive auto-repeat
        handle_nav_key(KEY_UP, [&]() {
            if (input.is_shift_active()) {
                shared_bpm = (shared_bpm + 5 <= 250) ? shared_bpm + 5 : 250;
                value_changed = true;
            } else {
                if (cursor_track > 0) { cursor_track--; value_changed = true; }
            }
        });

        handle_nav_key(KEY_DOWN, [&]() {
            if (input.is_shift_active()) {
                shared_bpm = (shared_bpm - 5 >= 40) ? shared_bpm - 5 : 40;
                value_changed = true;
            } else {
                if (cursor_track < 3) { cursor_track++; value_changed = true; }
            }
        });

        handle_nav_key(KEY_LEFT, [&]() {
            if (cursor_col > 0) { cursor_col--; value_changed = true; }
        });

        handle_nav_key(KEY_RIGHT, [&]() {
            if (cursor_col < 9) { cursor_col++; value_changed = true; }
        });

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
                // Instantly trigger flash save sequence (bypassing animation sequence)
                disk_save_state = 2;
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
                    break;
                }
            }
            
            shared_params[cursor_track] = p;
            unlock_shared_params(lock_save);
        }

        // Draw UI dashboard
        if (value_changed || force_redraw || sequencer_playing) {
            draw_ui_dashboard();
            force_redraw = false;
        } else {
            sleep_ms(10); // Sleep only on idle frames to keep inputs ultra-responsive and latency-free!
        }
    }
    
    return 0;
}
