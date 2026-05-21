#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

// ---- Hardware / Board ----
#define CFG_TUSB_MCU        OPT_MCU_RP2040
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

// ---- Debug ----
#define CFG_TUSB_DEBUG      0

// ---- Device classes ----
#define CFG_TUD_CDC         0
#define CFG_TUD_MSC         0
#define CFG_TUD_HID         0
#define CFG_TUD_MIDI        1
#define CFG_TUD_VENDOR      0

// ---- MIDI buffer sizes ----
#define CFG_TUD_MIDI_RX_BUFSIZE (TUD_OPT_HIGH_SPEED ? 512 : 64)
#define CFG_TUD_MIDI_TX_BUFSIZE (TUD_OPT_HIGH_SPEED ? 512 : 64)

#endif // _TUSB_CONFIG_H_
