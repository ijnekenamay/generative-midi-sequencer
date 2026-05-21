#include "midi_handler.hpp"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "tusb.h"

// Minimal TinyUSB device API declarations (avoid dependency on pico_enable_tinyusb macro).
extern "C" {
    bool tud_midi_n_mounted(uint8_t itf);
    void tud_task(void);
    // packet write API: write 4-byte USB-MIDI packet
    bool tud_midi_n_packet_write(uint8_t itf, const uint8_t packet[4]);
}

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
    // If TinyUSB is mounted and USB is preferred, send over USB-MIDI
    #if 1
    if (usb_preferred && tud_midi_n_mounted(0)) {
        // Build a 4-byte USB-MIDI packet (Cable 0)
        uint8_t cin = 0x0;
        uint8_t status = data[0] & 0xF0;
        if ((data[0] & 0xF0) == 0x90) cin = 0x9; // Note On
        else if ((data[0] & 0xF0) == 0x80) cin = 0x8; // Note Off
        else if (data[0] == 0xF8) cin = 0xF; // Timing Clock (single byte)
        else cin = 0x4;

        uint8_t packet[4] = { (uint8_t)((0 << 4) | (cin & 0x0F)), 0, 0, 0 };
        if (len > 0) packet[1] = data[0];
        if (len > 1) packet[2] = data[1];
        if (len > 2) packet[3] = data[2];

        // Write packet via TinyUSB MIDI packet API
        tud_midi_n_packet_write(0, packet);
        return;
    }
    #endif

    // Fallback: Transmit data blocking through the UART FIFO
    uart_write_blocking(uart0, data, len);
}

void MidiHandler::set_usb_preferred(bool en) {
    usb_preferred = en;
}

bool MidiHandler::is_usb_preferred() const {
    return usb_preferred;
}
