#pragma once
#include <stdint.h>
#include "lvgl.h"

enum KeyIndex {
    KEY_LEFT = 0,   // GP2  (Cursor Left)
    KEY_DOWN,       // GP3  (Cursor Down)
    KEY_RIGHT,      // GP4  (Cursor Right)
    KEY_UP,         // GP5  (Cursor Up)
    KEY_RT,         // GP6  (Right Trigger / PAGE)
    KEY_B,          // GP7  (B button / DEC)
    KEY_A,          // GP8  (A button / INC)
    KEY_LT,         // GP9  (Left Trigger / SHIFT modifier)
    KEY_PLAY,       // GP10 (Play / Stop)
    KEY_COUNT
};

struct KeyState {
    uint8_t gpio_pin;
    bool current_state;       // debounced state (true = pressed/LOW, false = released/HIGH)
    bool last_state;          // state in the previous frame
    mutable bool just_pressed;// true only on the frame the button was pressed
    mutable bool just_released;// true only on the frame the button was released
    uint32_t press_duration_ms;// how long the key has been held down
    mutable bool long_pressed_fired;  // flags if the long press event was already dispatched
    
    // Debounce filter state
    uint8_t debounce_counter;
    bool raw_state;
};

class InputManager {
private:
    KeyState keys[KEY_COUNT];
    static const uint8_t DEBOUNCE_THRESHOLD = 3; // 3 ticks (approx 30ms at 10ms loop rate)
    static const uint32_t LONG_PRESS_THRESHOLD_MS = 600; // 600ms hold for long press

public:
    InputManager();
    
    /**
     * Initializes all 9 GPIO pins with internal pull-up resistors.
     */
    void init();

    /**
     * Updates the debounce filter and key event state machine.
     * Should be called periodically (e.g., every 10ms in the Core 0 main loop).
     * 
     * @param delta_time_ms Time elapsed since last update (typically 10).
     */
    void update(uint32_t delta_time_ms);

    // Event Query API
    bool is_pressed(KeyIndex key) const;      // True on the exact frame the key was pressed
    bool is_released(KeyIndex key) const;     // True on the exact frame the key was released
    bool is_held(KeyIndex key) const;         // True continuously as long as the key is down
    bool is_long_pressed(KeyIndex key) const;  // True when key is held down for more than 600ms
    
    // Modifier helper
    bool is_shift_active() const { return is_held(KEY_LT); }

    /**
     * LVGL Input Device read callback.
     */
    static void lv_keypad_read_cb(lv_indev_t * indev, lv_indev_data_t * data);
};
