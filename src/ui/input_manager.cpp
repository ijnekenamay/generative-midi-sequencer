#include "input_manager.hpp"
#include "pico/stdlib.h"
#include "hardware/gpio.h"

InputManager::InputManager() {
    // Map indices to physical Pico GPIO pins as specified in the schematic
    keys[KEY_LEFT].gpio_pin = 8;
    keys[KEY_DOWN].gpio_pin = 9;
    keys[KEY_RIGHT].gpio_pin = 10;
    keys[KEY_UP].gpio_pin = 11;
    keys[KEY_RT].gpio_pin = 12;
    keys[KEY_B].gpio_pin = 13;
    keys[KEY_A].gpio_pin = 14;
    keys[KEY_LT].gpio_pin = 15;
    keys[KEY_PLAY].gpio_pin = 16;
    
    // Initialize states to zero/defaults
    for (int i = 0; i < KEY_COUNT; ++i) {
        keys[i].current_state = false;
        keys[i].last_state = false;
        keys[i].just_pressed = false;
        keys[i].just_released = false;
        keys[i].press_duration_ms = 0;
        keys[i].long_pressed_fired = false;
        keys[i].debounce_counter = 0;
        keys[i].raw_state = false;
    }
}

void InputManager::init() {
    for (int i = 0; i < KEY_COUNT; ++i) {
        uint8_t pin = keys[i].gpio_pin;
        
        // 1. Initialize GPIO pin
        gpio_init(pin);
        
        // 2. Set as INPUT
        gpio_set_dir(pin, GPIO_IN);
        
        // 3. Enable internal pull-up resistor
        // When switch is pressed, it connects the pin to GND (low voltage)
        gpio_pull_up(pin);
    }
}

void InputManager::update(uint32_t delta_time_ms) {
    for (int i = 0; i < KEY_COUNT; ++i) {
        KeyState& key = keys[i];
        
        // Read physical pin state (inverted: true = pressed / LOW)
        bool physical_pressed = !gpio_get(key.gpio_pin);
        
        // --- High-Speed Responsive Direct Input Scan (Zero-Lag Edge Detection) ---
        bool next_state = physical_pressed;
        
        // --- State Machine Events ---
        key.just_pressed = (next_state && !key.current_state);
        key.just_released = (!next_state && key.current_state);
        
        key.current_state = next_state;
        
        // Duration counting & Long press detection
        if (key.current_state) {
            key.press_duration_ms += delta_time_ms;
        } else {
            key.press_duration_ms = 0;
            key.long_pressed_fired = false;
        }
        
        // Update history
        key.last_state = key.current_state;
    }
}

bool InputManager::is_pressed(KeyIndex key) const {
    if (key >= KEY_COUNT) return false;
    bool pressed = keys[key].just_pressed;
    keys[key].just_pressed = false; // Event consumed (Read-Once One-Shot design)
    return pressed;
}

bool InputManager::is_released(KeyIndex key) const {
    if (key >= KEY_COUNT) return false;
    bool released = keys[key].just_released;
    keys[key].just_released = false; // Event consumed (Read-Once One-Shot design)
    return released;
}

bool InputManager::is_held(KeyIndex key) const {
    if (key >= KEY_COUNT) return false;
    return keys[key].current_state;
}

bool InputManager::is_long_pressed(KeyIndex key) const {
    if (key >= KEY_COUNT) return false;
    // Return true only once when the hold threshold is crossed
    const KeyState& k = keys[key];
    if (k.current_state && k.press_duration_ms >= LONG_PRESS_THRESHOLD_MS && !k.long_pressed_fired) {
        k.long_pressed_fired = true;
        return true;
    }
    return false;
}

void InputManager::lv_keypad_read_cb(lv_indev_t * indev, lv_indev_data_t * data) {
    InputManager* manager = (InputManager*)lv_indev_get_user_data(indev);
    if (!manager) return;

    data->state = LV_INDEV_STATE_RELEASED;

    if (manager->is_held(KEY_UP)) {
        data->key = LV_KEY_UP;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (manager->is_held(KEY_DOWN)) {
        data->key = LV_KEY_DOWN;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (manager->is_held(KEY_LEFT)) {
        data->key = LV_KEY_LEFT;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (manager->is_held(KEY_RIGHT)) {
        data->key = LV_KEY_RIGHT;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (manager->is_held(KEY_A)) {
        data->key = LV_KEY_ENTER;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (manager->is_held(KEY_B)) {
        data->key = LV_KEY_ESC;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (manager->is_held(KEY_RT)) {
        data->key = LV_KEY_NEXT;
        data->state = LV_INDEV_STATE_PRESSED;
    } else if (manager->is_held(KEY_LT)) {
        data->key = LV_KEY_PREV;
        data->state = LV_INDEV_STATE_PRESSED;
    }
}
