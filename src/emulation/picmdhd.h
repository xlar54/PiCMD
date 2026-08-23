// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// Emulates the CMD HD mainboard;-
//  - R65C02 CPU @ 2MHz
//  - U10 6522 VIA (IEC bus and fast serial)
//  - U9  6522 VIA (SCSI data bus and handshaking)
//  - U11 8255A PPI (buttons, SCSI control, memory banking, CMD parallel bus)
//  - U13 PLD (SCSI phase encoding, emulated inside the SCSI module)
//  - U20 latch (LEDs and RAM write protection)
//  - RTC-72421 real time clock
//  - 64K RAM, 16K boot ROM
//  - SCSI hard disk(s) backed by DHD image files
//
// The hardware model follows VICE's cmdhd.c written by Roberto Muscedere.
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

#ifndef PICMDHD_H
#define PICMDHD_H

#include "m65c02.h"
#include "m6522.h"
#include "i8255a.h"
#include "rtc72421.h"
#include "scsi.h"

class PiCMDHD
{
public:
	PiCMDHD();

	void Initialise();
	void Update();		// One 2MHz CPU cycle worth of house keeping.
	void Reset();

	// Attach the DHD image (SCSI ID 0 LUN 0) plus any companion .sXY files.
	bool Insert(const char* filename, bool readOnly);
	void Eject();
	bool IsImageAttached() const { return disk[0].IsAttached(); }
	const char* GetImageName() const { return disk[0].GetFileName(); }

	// Boot ROM. Accepts a 16K image or a 32K image made of two identical halves.
	bool SetROM(const unsigned char* data, unsigned size);
	bool IsRomLoaded() const { return romLoaded; }

	// Progress of a whole-disk scan. Both FindBaseLBA and a SCSI FORMAT walk
	// the image looking for the CMD signature block, and on a large image that
	// takes long enough to look like a hang: the scan runs to completion inside
	// a single emulated memory write, so the 65C02 cannot light the ACTIVITY
	// lamp or move the head position while it happens. These let the display,
	// which runs on the other core, show that something is going on. Written
	// only by the emulation core and read only by the display core; a stale
	// read just shows a slightly old number, so no locking is needed.
	volatile u32 scanSector;
	volatile u32 scanTotal;		// 0 when no scan is running
	bool IsScanning() const { return scanTotal != 0; }

	// The device number the drive is currently answering as. HDOS keeps its
	// bus addresses in zero page: $77 is the listen address (0x20 + device)
	// and $78 the talk address (0x40 + device). This is the only way to know
	// it from outside, because the number is not ours - it comes from the
	// configuration block on the disk, or from the boot ROM in installation
	// and configuration mode, which both use 30. Returns 0 if it does not yet
	// look like a valid address.
	u8 GetDeviceNumber() const
	{
		u8 listen = ram[0x77];
		return (listen >= 0x24 && listen <= 0x3e) ? (u8)(listen - 0x20) : 0;
	}

	// Device 30 is the boot ROM answering rather than HDOS off the disk. That
	// happens for two quite different reasons, and the configuration block
	// tells them apart: if there is one, the drive has HDOS and was put into
	// installation or configuration mode deliberately with the buttons; if
	// there is not, the disk is blank and there is no HDOS to hand over to,
	// whatever the buttons were doing.
	// Which front panel mode the drive was asked for, latched at the reset
	// that started it - exactly what the real panel does, since the buttons
	// are only sampled as the drive comes up. Device 30 alone does not tell
	// you this: a blank image sits on 30 because the boot ROM has nothing to
	// hand over to, without anyone having pressed anything.
	enum FrontPanelMode { PANEL_NONE, PANEL_INSTALL, PANEL_CONFIG };
	FrontPanelMode GetFrontPanelMode() const { return panelMode; }

	bool IsBootRomAnswering() const { return GetDeviceNumber() == 30; }
	bool HasConfigBlock() const { return baselba != INVALID_BASELBA; }
	u32 ScanPercent() const
	{
		u32 t = scanTotal;
		return t ? (u32)(((u64)scanSector * 100) / t) : 0;
	}

	// Front panel buttons (live, momentary).
	void SetWPButton(bool pressed);
	void SetSwap8Button(bool pressed);
	void SetSwap9Button(bool pressed);

	// The CMD HD's six front panel indicators, all driven by the U20 latch and
	// all active low. VICE only models ACTIVITY and ERROR; the rest were
	// identified by driving the drive's own Front Panel Partition Selection
	// mode, whose documented lamp chart (see the manual's "FRONT PANEL
	// SELECTION CHART") acts as an oracle - in FPPS the four lamps show the
	// selected digit in binary as WRITE PROTECT=1, SWAP 9=2, SWAP 8=4, GEOS=8.
	bool IsActivityLEDOn() const { return (LEDs & 0x01) == 0; }	// green, upper
	bool IsErrorLEDOn() const { return (LEDs & 0x02) == 0; }	// red
	bool IsSwap8LEDOn() const { return (LEDs & 0x04) == 0; }
	bool IsSwap9LEDOn() const { return (LEDs & 0x08) == 0; }
	bool IsActivity2LEDOn() const { return (LEDs & 0x10) == 0; }	// lower/SCSI
	bool IsGeosLEDOn() const { return (LEDs & 0x40) == 0; }
	bool IsWriteProtectLEDOn() const { return (LEDs & 0x80) == 0; }
	// bit 5 of the latch is not an indicator; it write enables RAM above $8000.

	// Rough head position for the display, 0-199 like VICE's track indicator.
	unsigned GetHeadPosition() const { return headPosition; }

	// If nonzero the device number stored in the DHD configuration block is
	// patched to this unit number on the fly (VICE behaviour). If zero the
	// image's own device number is respected.
	void SetForcedDeviceID(u8 id) { forcedDeviceID = id; }

	// Memory bus (called by the CPU on every cycle).
	u8 Read(u16 address);
	void Write(u16 address, u8 value);

	M65C02 m65c02;
	m6522 via9;		// SCSI
	m6522 via10;	// IEC
	i8255a_state i8255a;
	RTC72421 rtc;
	scsi_context_t scsi;

	unsigned fastSerialDirection;

	// SCSI glue state (mirrors cmdhd_context_t in VICE)
	u8 LEDs;			// U20 latch
	u8 i8255a_i[3];		// PPI input lines
	u8 i8255a_o[3];		// PPI output latches
	u8 scsi_dir;		// U9 PB3
	u32 imagesize;		// in 512 byte sectors
	u32 baselba;		// LBA of the CMD partition area, UINT32_MAX if unknown
	// Set by any write, cleared by a scan. FindBaseLBA only has to run again
	// if something could have moved the configuration block, and on a blank
	// image the scan is the whole disk - too expensive to repeat per reset.
	bool scanNeeded;

	static const u32 INVALID_BASELBA = 0xffffffff;

private:
	friend void cmdhd_scsiread(scsi_context_t* scsi);
	friend void cmdhd_scsiwrite(scsi_context_t* scsi);
	friend s32 cmdhd_scsiformat(scsi_context_t* scsi);

	void FindBaseLBA();
	void UpdateButtonInputs();

	ScsiImage disk[SCSI_MAX_DISKS];

	unsigned char ram[0x10000];
	unsigned char rom[0x4000];
	bool romLoaded;

	Interrupt via9IRQ;
	Interrupt via10IRQ;

	u8 forcedDeviceID;
	unsigned headPosition;

	// Buttons pressed by the user right now.
	bool buttonWP;
	bool buttonSwap8;
	bool buttonSwap9;
	// Buttons held down virtually (installation mode); released after a while.
	u32 virtualButtonCountdown;
	u8 virtualButtonMask;	// bits to force low in i8255a_i[1]
	FrontPanelMode panelMode;
};

extern u8 read65C02CMDHD(u16 address);
extern void write65C02CMDHD(u16 address, const u8 value);

#endif
