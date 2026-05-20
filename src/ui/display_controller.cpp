#include "display_controller.hpp"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"

// Custom fonts and colors removed for LVGL

DisplayController::DisplayController() {
    // Pico Tracker physical pin mappings from J7 DISPLAY Schematic (SPI1)
    pin_miso = 22; // Display RESET (DP4 -> GP22)
    pin_cs   = 20; // Display CS (DP3 -> GP20)
    pin_sck  = 26; // Display SCK (DP7 -> GP26)
    pin_mosi = 27; // Display MOSI (DP6 -> GP27)
    pin_dc   = 21; // Display D/C (DP5 -> GP21)

    // Landscape Mode standard orientation
    width = 320;
    height = 240;
    rotation = 3;
}

void DisplayController::write_cmd(uint8_t cmd) {
    gpio_put(pin_dc, 0); // DC Low = Command
    gpio_put(pin_cs, 0); // Select Display
    spi_write_blocking(spi1, &cmd, 1);
    gpio_put(pin_cs, 1); // Deselect
}

void DisplayController::write_data(uint8_t data) {
    gpio_put(pin_dc, 1); // DC High = Data
    gpio_put(pin_cs, 0);
    spi_write_blocking(spi1, &data, 1);
    gpio_put(pin_cs, 1);
}

void DisplayController::write_data_buf(const uint8_t* buf, uint32_t len) {
    gpio_put(pin_dc, 1);
    gpio_put(pin_cs, 0);
    spi_write_blocking(spi1, buf, len);
    gpio_put(pin_cs, 1);
}

void DisplayController::reset() {
    // Hardware reset pulse via GP16 (wired to LCD RST)
    gpio_put(pin_miso, 1);
    sleep_ms(5);
    gpio_put(pin_miso, 0);
    sleep_ms(20);
    gpio_put(pin_miso, 1);
    sleep_ms(150);
}

void DisplayController::init() {
    // 1. Initialize SPI1 at 48 MHz (maximum safe speed for ILI9341 on breadboard)
    spi_init(spi1, 48000000);
    
    // 2. Map SPI functions to GPIO pins
    gpio_set_function(pin_sck, GPIO_FUNC_SPI);
    gpio_set_function(pin_mosi, GPIO_FUNC_SPI);
    
    // 3. Initialize control pins as standard outputs
    gpio_init(pin_cs);
    gpio_set_dir(pin_cs, GPIO_OUT);
    gpio_put(pin_cs, 1);

    gpio_init(pin_dc);
    gpio_set_dir(pin_dc, GPIO_OUT);
    gpio_put(pin_dc, 1);
    
    // GP16 is Display Reset
    gpio_init(pin_miso);
    gpio_set_dir(pin_miso, GPIO_OUT);
    gpio_put(pin_miso, 1);

    // 4. Trigger Display Reset
    reset();

    // 5. ILI9341 Initialization Sequence
    write_cmd(0x01); // Software reset
    sleep_ms(10);
    
    write_cmd(0x28); // Display OFF
    
    // Power controls
    write_cmd(0xC0); // Power Control 1
    write_data(0x23);
    
    write_cmd(0xC1); // Power Control 2
    write_data(0x10);
    
    // VCOM controls
    write_cmd(0xC5); // VCOM Control 1
    write_data(0x3E);
    write_data(0x28);
    
    write_cmd(0xC7); // VCOM Control 2
    write_data(0x86);
    
    // Memory Access (Orientation)
    write_cmd(0x36); // MADCTL
    // Set rotation 3 (Landscape inverted, pins on left)
    write_data(0xE8); 

    write_cmd(0x3A); // COLMOD: Pixel Format Set
    write_data(0x55); // 16-bit color (RGB 565)
    
    // Frame Rate
    write_cmd(0xB1);
    write_data(0x00);
    write_data(0x18); // 79 Hz
    
    write_cmd(0xB6); // Display Function Control
    write_data(0x08);
    write_data(0x82);
    write_data(0x27);
    
    write_cmd(0x11); // Exit Sleep
    sleep_ms(120);
    
    write_cmd(0x29); // Display ON
    sleep_ms(20);
}

void DisplayController::set_address_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    write_cmd(0x2A); // Column Address Set
    write_data(x0 >> 8);
    write_data(x0 & 0xFF);
    write_data(x1 >> 8);
    write_data(x1 & 0xFF);
    
    write_cmd(0x2B); // Page Address Set
    write_data(y0 >> 8);
    write_data(y0 & 0xFF);
    write_data(y1 >> 8);
    write_data(y1 & 0xFF);
    
    write_cmd(0x2C); // Memory Write (signals color transfer)
}

void DisplayController::flush_area(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, const uint8_t* color_p) {
    if (x1 >= width || y1 >= height) return;
    if (x2 >= width)  x2 = width - 1;
    if (y2 >= height) y2 = height - 1;
    
    set_address_window(x1, y1, x2, y2);
    
    uint32_t w = x2 - x1 + 1;
    uint32_t h = y2 - y1 + 1;
    uint32_t total_bytes = w * h * 2; // 16-bit color
    
    gpio_put(pin_dc, 1); // Data
    gpio_put(pin_cs, 0); // Select
    
    spi_write_blocking(spi1, color_p, total_bytes);
    
    gpio_put(pin_cs, 1); // Deselect
}

void DisplayController::lv_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
    DisplayController* controller = (DisplayController*)lv_display_get_user_data(disp);
    if (controller) {
        controller->flush_area(area->x1, area->y1, area->x2, area->y2, px_map);
    }
    lv_display_flush_ready(disp);
}
