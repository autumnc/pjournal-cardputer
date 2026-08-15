#pragma once

// USB Drive (export) mode: expose the SD card to a host PC over USB Mass
// Storage so journals can be copied off. Entered by holding 'E' at power-on
// (see main.cpp); the normal app + FAT mount are NOT started in this mode, so
// the firmware never touches the SD while the host owns it.

#ifdef __cplusplus
extern "C" {
#endif

// Init SD card in raw mode + start TinyUSB MSC. Returns false if no card.
bool usb_drive_begin(void);

// Show the status screen and loop forever (never returns).
void usb_drive_run(void);

#ifdef __cplusplus
}
#endif
