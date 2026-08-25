// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// One-shot DHCP, ARP and SNTP client for raw Ethernet-frame transports.
//
// Used by USB Ethernet and BCM43430 WiFi. All operations are bounded by the
// caller's timeout and return false without changing the RTC on failure.
//
// This file is part of Pi1541.
//
// Pi1541 is free software : you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Pi1541 is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Pi1541. If not, see <http://www.gnu.org/licenses/>.

#ifndef NTPCLIENT_H
#define NTPCLIENT_H

#include "types.h"

class PiCMDHD;
typedef bool (*NetSendFrameFn)(const u8* frame, u32 length);
typedef bool (*NetReceiveFrameFn)(u8* frame, u32 capacity, u32* outLength);
struct NetFrameIO { NetSendFrameFn send; NetReceiveFrameFn receive; };

// serverIPText must be a dotted IPv4 address. utcOffsetMinutes is added to UTC.
// Returns true after setting the RTC.
bool NtpClient_FetchAndSetTime(PiCMDHD& piCMDHD, const char* serverIPText, s32 utcOffsetMinutes, u32 timeoutMs);
// Runs the same client over the supplied raw Ethernet-frame transport.
bool NtpClient_FetchAndSetTimeOverFrames(PiCMDHD& piCMDHD, const NetFrameIO& io, const u8 mac[6], const char* serverIPText, s32 utcOffsetMinutes, u32 timeoutMs);

#endif
