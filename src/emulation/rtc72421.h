// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// Epson RTC-72421 emulation.
// Register semantics follow VICE's rtc-72421.c. Since the Raspberry Pi has no
// battery backed clock, the emulated RTC free runs from the BCM system timer
// starting at a default date. HDOS and GEOS can set the time (T-W commands)
// and the emulated clock will keep it from then on.
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

#ifndef RTC72421_H
#define RTC72421_H

#include "types.h"

// Register map (each register is 4 bits):
//  0 seconds            1 10 seconds
//  2 minutes            3 10 minutes
//  4 hours              5 10 hours (+ am/pm + 12/24 flags)
//  6 day of month       7 10 day of month
//  8 month              9 10 month
//  A year               B 10 year
//  C weekday
//  D control D          E control E          F control F (24h/12h, hold)

class RTC72421
{
public:
	RTC72421();

	void Reset();

	u8 Read(u8 address);
	void Write(u8 address, u8 data);

	// Bring the calendar up to date with the system timer. Reads and writes do
	// this themselves; the caller also has to do it every so often, because the
	// elapsed time is worked out from a 32 bit microsecond counter and a gap
	// longer than its 71.6 minute wrap is indistinguishable from a short one.
	void Tick();

private:
	void IncrementSecond();

	u8 seconds, minutes, hours;	// binary, hours always kept in 24h form internally
	u8 day, month;				// 1 based
	u8 year;					// 0-99
	u8 weekday;					// 0-6
	bool hour24;				// reg F bit 2: 24 hour mode
	bool stop;					// hold/stop bit
	u8 ctrl[3];					// control registers D, E, F

	u32 lastMicros;				// BCM system timer value at last Tick
	u32 microsAccumulated;
};

#endif
