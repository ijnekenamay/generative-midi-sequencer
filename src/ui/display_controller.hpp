#pragma once
#include <stdint.h>
#include "lvgl.h"

class DisplayController {
private:
    uint8_t pin_miso; // GP22 (RESET / DP4)
    uint8_t pin_cs;   // GP20 (CS / DP3)
    uint8_t pin_sck;  // GP26 (SCK / DP7)
    uint8_t pin_mosi; // GP27 (MOSI / DP6)
    uint8_t pin_dc;   // GP21 (D/C / DP5)
    
    uint16_t width;
    uint16_t height;
    uint8_t rotation;

    // Helper communication methods
    void write_cmd(uint8_t cmd);
    void write_data(uint8_t data);
    void write_data_buf(const uint8_t* buf, uint32_t len);
    void set_address_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

public:
    DisplayController();
    
    /**
     * Initializes the SPI0 peripheral at 48MHz (very high refresh rate)
     * and sends the ILI9341 power/display configuration commands.
     */
    void init();

    /**
     * Resets the display via the hardware reset pin (GP16).
     */
    void reset();

    /**
     * Flushes a rendered area from LVGL to the display.
     */
    void flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint8_t* color_p);

    /**
     * Static callback for LVGL.
     */
    static void lv_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map);

    // Getters for dimensions
    uint16_t get_width() const { return width; }
    uint16_t get_height() const { return height; }
};
