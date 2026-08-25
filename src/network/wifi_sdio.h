// Pi-CMD BCM43430 SDIO transport. The Pi 3 shares one EMMC controller between
// the SD card (GPIO 48-53) and WLAN (GPIO 34-39); callers must restore and
// reinitialise the SD card after using this module.
#ifndef WIFI_SDIO_H
#define WIFI_SDIO_H

#include "types.h"

// Select and identify the onboard SDIO device and enable function 1.
// All waits are bounded. Safe to call SwitchBackToSd after a failed call.
bool WifiSdio_SwitchToWlan();
void WifiSdio_SwitchBackToSd();
bool WifiSdio_IsOnWlanRoute();
const char* WifiSdio_LastError();
u16 WifiSdio_Rca();

// SDIO direct/extended I/O. CMD53 only supports 4-byte aligned transfers of
// 4..508 bytes; callers must split larger backplane/FIFO operations.
bool WifiSdio_Cmd52(bool write, u8 function, u32 address, u8 writeValue, u8* readValue);
bool WifiSdio_Cmd53(bool write, u8 function, u32 address, bool incrementingAddress, u8* buffer, u32 length);
bool WifiSdio_EnableFunction(u8 function, u32 timeoutUs);

#endif
