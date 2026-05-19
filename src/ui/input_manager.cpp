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
        
        // --- Debounce Filter ---
        if (physical_pressed == key.raw_state) {
            if (key.debounce_counter < DEBOUNCE_THRESHOLD) {
                key.debounce_counter++;
                if (key.debounce_counter >= DEBOUNCE_THRESHOLD) {
                    key.current_state = physical_pressed;
                }
            }
        } else {
            // State changed physically, reset debounce counter
            key.raw_state = physical_pressed;
            key.debounce_counter = 0;
        }
        
        // --- State Machine Events ---
        // Single frame triggers
        key.just_pressed = (key.current_state && !key.last_state);
        key.just_released = (!key.current_state && key.last_state);
        
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
    return keys[key].just_pressed;
}

bool InputManager::is_released(KeyIndex key) const {
    if (key >= KEY_COUNT) return false;
    return keys[key].just_released;
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
