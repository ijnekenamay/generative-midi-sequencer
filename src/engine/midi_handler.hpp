#pragma once
#include <stdint.h>

class MidiHandler {
public:
    MidiHandler();
    
    /**
     * Initializes the UART0 hardware at the standard MIDI baud rate of 31,250 bps.
     * Configures GP0 as TX and GP1 as RX.
     */
    void init();

    /**
     * Sends a MIDI Note On message.
     * 
     * @param channel MIDI channel (0-15 corresponding to MIDI Ch 1-16)
     * @param note MIDI note number (0-127)
     * @param velocity Key strike speed/volume (0-127)
     */
    void note_on(uint8_t channel, uint8_t note, uint8_t velocity);

    /**
     * Sends a MIDI Note Off message.
     * 
     * @param channel MIDI channel (0-15 corresponding to MIDI Ch 1-16)
     * @param note MIDI note number (0-127)
     * @param velocity Key release speed (0-127)
     */
    void note_off(uint8_t channel, uint8_t note, uint8_t velocity = 0);

    /**
     * Sends a MIDI Timing Clock (0xF8) tick.
     */
    void send_clock();

    /**
     * Sends a MIDI Start (0xFA) command.
     */
    void send_start();

    /**
     * Sends a MIDI Stop (0xFC) command.
     */
    void send_stop();

    /**
     * Enable or disable USB MIDI output preference. When enabled and USB is mounted,
     * MIDI messages will be sent over USB instead of UART.
     */
    void set_usb_preferred(bool en);

    /**
     * Query whether USB is currently preferred for MIDI output.
     */
    bool is_usb_preferred() const;

private:
    void write_raw(const uint8_t* data, uint32_t len);
    bool usb_preferred = false;
};
