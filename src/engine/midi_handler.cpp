#include "midi_handler.hpp"
#include "hardware/uart.h"
#include "hardware/gpio.h"

MidiHandler::MidiHandler() {
    // Constructor
}

void MidiHandler::init() {
    // 1. Initialize UART0 at 31,250 bps (Standard MIDI Baud Rate)
    uart_init(uart0, 31250);
    
    // 2. Set GPIO functions for UART0 TX (GP0) and RX (GP1)
    gpio_set_function(0, GPIO_FUNC_UART);
    gpio_set_function(1, GPIO_FUNC_UART);
    
    // 3. Configure UART formats (8 data bits, no parity, 1 stop bit - standard 8N1)
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    
    // 4. Disable hardware flow control
    uart_set_hw_flow(uart0, false, false);
}

void MidiHandler::note_on(uint8_t channel, uint8_t note, uint8_t velocity) {
    uint8_t msg[3];
    msg[0] = 0x90 | (channel & 0x0F);   // Status byte (Note On + Channel)
    msg[1] = note & 0x7F;              // Data byte 1 (Note number)
    msg[2] = velocity & 0x7F;          // Data byte 2 (Velocity)
    
    write_raw(msg, 3);
}

void MidiHandler::note_off(uint8_t channel, uint8_t note, uint8_t velocity) {
    uint8_t msg[3];
    msg[0] = 0x80 | (channel & 0x0F);   // Status byte (Note Off + Channel)
    msg[1] = note & 0x7F;              // Data byte 1 (Note number)
    msg[2] = velocity & 0x7F;          // Data byte 2 (Velocity)
    
    write_raw(msg, 3);
}

void MidiHandler::send_clock() {
    uint8_t clock_byte = 0xF8;         // MIDI Timing Clock status byte
    write_raw(&clock_byte, 1);
}

void MidiHandler::send_start() {
    uint8_t start_byte = 0xFA;         // MIDI Start status byte
    write_raw(&start_byte, 1);
}

void MidiHandler::send_stop() {
    uint8_t stop_byte = 0xFC;          // MIDI Stop status byte
    write_raw(&stop_byte, 1);
}

void MidiHandler::write_raw(const uint8_t* data, uint32_t len) {
    // Transmit data blocking through the UART FIFO
    uart_write_blocking(uart0, data, len);
}
