#pragma once
#include <stdint.h>
#include <string>

// RGB565 Premium Techno Color Palette
#define COLOR_BLACK       0x0000
#define COLOR_DARK_GREY   0x2104 // Very sleek dark background
#define COLOR_GREY        0x7BEF
#define COLOR_LIGHT_GREY  0xC618
#define COLOR_WHITE       0xFFFF
#define COLOR_GREEN       0x07E0
#define COLOR_CYAN        0x07FF // Neon cyan
#define COLOR_MAGENTA     0xF81F // Cyberpunk pink/magenta
#define COLOR_YELLOW      0xFFE0
#define COLOR_ORANGE      0xFD20 // Warm orange
#define COLOR_RED         0xF800
#define COLOR_PURPLE      0x780F

class DisplayController {
private:
    uint8_t pin_miso; // GP16 (often used as RESET for the display if MISO is not needed)
    uint8_t pin_cs;   // GP17
    uint8_t pin_sck;  // GP18
    uint8_t pin_mosi; // GP19
    uint8_t pin_dc;   // GP20
    
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
     * Clears the screen with a specific RGB565 color.
     */
    void clear(uint16_t color = COLOR_DARK_GREY);

    /**
     * Draws a single pixel.
     */
    void draw_pixel(uint16_t x, uint16_t y, uint16_t color);

    /**
     * Draws a filled rectangle (highly optimized via SPI buffer writes).
     */
    void fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

    /**
     * Draws an empty rectangle outline.
     */
    void draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

    /**
     * Draws a single character using an embedded 8x8 pixel font.
     */
    void draw_char(uint16_t x, uint16_t y, char c, uint16_t color, uint16_t bg_color, uint8_t scale = 1);

    /**
     * Draws a string of text.
     */
    void draw_text(uint16_t x, uint16_t y, const std::string& text, uint16_t color, uint16_t bg_color, uint8_t scale = 1);

    // Getters for dimensions
    uint16_t get_width() const { return width; }
    uint16_t get_height() const { return height; }
};
