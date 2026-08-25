// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// Epson RTC-72421 emulation.
// Register semantics follow VICE's rtc-72421.c (written by Marco van den
// Heuvel), reimplemented over a free running calendar clocked from the BCM
// system timer.
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

#include "rtc72421.h"
#include "rpihardware.h"

#define LIMIT_9(x) (((x) > 9) ? 9 : (x))

static const u8 daysInMonth[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

static u8 DaysIn(u8 month, u8 year)
{
	if (month == 2)
	{
		// year is 00-99, treat as 2000-2099; 2000 was a leap year.
		if ((year & 3) == 0)
			return 29;
	}
	if (month >= 1 && month <= 12)
		return daysInMonth[month - 1];
	return 31;
}

RTC72421::RTC72421()
{
	Reset();
}

void RTC72421::Reset()
{
	// Default power on time: Wednesday 2026-01-01 00:00:00
	// (HDOS or GEOS can set the correct time via the T-W commands.)
	seconds = 0;
	minutes = 0;
	hours = 0;
	day = 1;
	month = 1;
	year = 26;
	weekday = 4;	// Thursday
	hour24 = true;
	stop = false;
	ctrl[0] = 0;
	ctrl[1] = 0;
	ctrl[2] = 0;
	lastMicros = read32(ARM_SYSTIMER_CLO);
	microsAccumulated = 0;
}

void RTC72421::SetDateTime(u8 year2digit, u8 month, u8 day, u8 weekday, u8 hours, u8 minutes, u8 seconds)
{
	this->seconds = seconds > 59 ? 59 : seconds;
	this->minutes = minutes > 59 ? 59 : minutes;
	this->hours = hours > 23 ? 23 : hours;
	this->day = day < 1 ? 1 : day;
	this->month = (month < 1) ? 1 : ((month > 12) ? 12 : month);
	this->year = year2digit % 100;
	this->weekday = weekday > 6 ? 6 : weekday;
	stop = false;
	lastMicros = read32(ARM_SYSTIMER_CLO);
	microsAccumulated = 0;
}

void RTC72421::IncrementSecond()
{
	if (++seconds < 60) return;
	seconds = 0;
	if (++minutes < 60) return;
	minutes = 0;
	if (++hours < 24) return;
	hours = 0;
	weekday = (weekday + 1) % 7;
	if (++day <= DaysIn(month, year)) return;
	day = 1;
	if (++month <= 12) return;
	month = 1;
	year = (year + 1) % 100;
}

void RTC72421::Tick()
{
	u32 now = read32(ARM_SYSTIMER_CLO);
	u32 elapsed = now - lastMicros;	// unsigned arithmetic handles wrap
	lastMicros = now;

	if (stop)
		return;

	microsAccumulated += elapsed;
	while (microsAccumulated >= 1000000)
	{
		microsAccumulated -= 1000000;
		IncrementSecond();
	}
}

u8 RTC72421::Read(u8 address)
{
	u8 retval = 0;

	Tick();

	switch (address & 0xf)
	{
		case 0x0:	// seconds
			retval = seconds % 10;
			break;
		case 0x1:	// 10 seconds
			retval = seconds / 10;
			break;
		case 0x2:	// minutes
			retval = minutes % 10;
			break;
		case 0x3:	// 10 minutes
			retval = minutes / 10;
			break;
		case 0x4:	// hours
			if (hour24)
			{
				retval = hours % 10;
			}
			else
			{
				u8 h = hours % 12;
				if (h == 0) h = 12;
				retval = h % 10;
			}
			break;
		case 0x5:	// 10 hours (12h mode: bit 2 = PM)
			if (hour24)
			{
				retval = hours / 10;
			}
			else
			{
				u8 h = hours % 12;
				if (h == 0) h = 12;
				retval = h / 10;
				if (hours >= 12)
					retval |= 4;
			}
			break;
		case 0x6:	// day of month
			retval = day % 10;
			break;
		case 0x7:	// 10 day of month
			retval = day / 10;
			break;
		case 0x8:	// month
			retval = month % 10;
			break;
		case 0x9:	// 10 month
			retval = month / 10;
			break;
		case 0xa:	// year
			retval = year % 10;
			break;
		case 0xb:	// 10 year
			retval = year / 10;
			break;
		case 0xc:	// weekday
			retval = weekday;
			break;
		case 0xe:	// RAMLINK writes/reads this register to detect the RTC
			retval = ctrl[1];
			break;
		case 0xf:
			// These have to sit where Write puts them - bit 2 for 24 hour
			// mode, bit 1 for stop, as the header says. The read reported them
			// one place lower, in bits 1 and 0, so anything that set the mode
			// and read it back got a different answer than it had written.
			// The two bits that are not modelled (TEST and RESET) are returned
			// as last written rather than dropped.
			retval = ctrl[2] & 0x9;
			retval |= hour24 ? 4 : 0;
			retval |= stop ? 2 : 0;
			break;
	}
	return retval;
}

void RTC72421::Write(u8 address, u8 data)
{
	u8 real_data = data & 0xf;

	Tick();

	switch (address & 0xf)
	{
		case 0x0:	// seconds
			seconds = (seconds / 10) * 10 + LIMIT_9(real_data);
			if (seconds > 59) seconds = 59;
			microsAccumulated = 0;
			break;
		case 0x1:	// 10 seconds
			seconds = (seconds % 10) + (real_data & 7) * 10;
			if (seconds > 59) seconds = 59;
			microsAccumulated = 0;
			break;
		case 0x2:	// minutes
			minutes = (minutes / 10) * 10 + LIMIT_9(real_data);
			if (minutes > 59) minutes = 59;
			break;
		case 0x3:	// 10 minutes
			minutes = (minutes % 10) + (real_data & 7) * 10;
			if (minutes > 59) minutes = 59;
			break;
		case 0x4:	// hours
			if (hour24)
			{
				hours = (hours / 10) * 10 + LIMIT_9(real_data);
			}
			else
			{
				bool pm = hours >= 12;
				u8 h = hours % 12;
				if (h == 0) h = 12;
				h = (h / 10) * 10 + LIMIT_9(real_data);
				hours = (h % 12) + (pm ? 12 : 0);
			}
			if (hours > 23) hours = 23;
			break;
		case 0x5:	// 10 hours; bit 3 = 24h mode, in 12h mode bit 2 = PM
			if (real_data & 8)
			{
				hour24 = true;
				hours = (hours % 10) + (real_data & 3) * 10;
			}
			else
			{
				bool pm = (real_data & 4) != 0;
				u8 h = hours % 12;
				if (h == 0) h = 12;
				h = (h % 10) + (real_data & 3) * 10;
				hour24 = false;
				hours = (h % 12) + (pm ? 12 : 0);
			}
			if (hours > 23) hours = 23;
			break;
		case 0x6:	// day of month
			day = (day / 10) * 10 + LIMIT_9(real_data);
			if (day < 1) day = 1;
			break;
		case 0x7:	// 10 day of month
			day = (day % 10) + (real_data & 3) * 10;
			if (day < 1) day = 1;
			break;
		case 0x8:	// month
			month = (month / 10) * 10 + LIMIT_9(real_data);
			if (month < 1) month = 1;
			if (month > 12) month = 12;
			break;
		case 0x9:	// 10 month
			month = (month % 10) + (real_data & 1) * 10;
			if (month < 1) month = 1;
			if (month > 12) month = 12;
			break;
		case 0xa:	// year
			year = (year / 10) * 10 + LIMIT_9(real_data);
			break;
		case 0xb:	// 10 year
			year = (year % 10) + LIMIT_9(real_data) * 10;
			break;
		case 0xc:	// weekday
			weekday = (real_data > 6) ? 6 : real_data;
			break;
		case 0xd:
			ctrl[0] = real_data;
			break;
		case 0xe:	// RAMLINK writes/reads this register to detect the RTC
			ctrl[1] = real_data;
			break;
		case 0xf:
			ctrl[2] = real_data;
			hour24 = (real_data & 4) != 0;
			stop = (real_data & 2) != 0;
			break;
	}
}
