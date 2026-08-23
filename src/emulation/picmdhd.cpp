// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// Emulates the CMD HD mainboard. The hardware model follows VICE's cmdhd.c
// written by Roberto Muscedere.
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

#include <string.h>
#include <stdio.h>
#include "picmdhd.h"
#include "iec_bus.h"
#include "debug.h"

extern PiCMDHD piCMDHD;

// U10 (IEC VIA) port B, wired like a 1541's $1800 VIA;-
// PB0 data in, PB1 data out, PB2 clock in, PB3 clock out, PB4 ATNA,
// PB5 fast serial direction, PB7 (and CA1) ATN in.
// The shift register carries fast serial data; CB1 is the fast serial clock.
#define VIA10_PINS_FAST_SER_DIR 0x20

// U9 (SCSI VIA);-
// PA0-7 SCSI data bus
// PB out: PB4 SEL, PB3 data direction (U13)
// PB in:  PB7 REQ, PB6 ACK, PB5 SEL&BSY, PB4 SEL, PB3 direction, PB2-0 phase
// Accessing PRA generates the SCSI ACK handshake.

// U11 (i8255A PPI);-
// PA CMD parallel bus data (not connected on the Pi)
// PB7 PATN, PB6 PCLK, PB3 /WP button, PB2 /SWAP9, PB1 /SWAP8, PB0 PREADY
// PC7 PREADY drive, PC6 /PCLK, PC5 /PEXT, PC4 SCSI BSY, PC3 SCSI RST,
// PC2 SCSI ATN, PC1 RAM map, PC0 ROM enable

// SCSI states are setup to reflect the register values coming out of U13 (see VICE cmdhd.c)
#define CMD_STATE_DATAOUT    0x00
#define CMD_STATE_COMMAND    0x01
#define CMD_STATE_DATAIN     0x04
#define CMD_STATE_STATUS     0x05
#define CMD_STATE_MESSAGEOUT 0x03
#define CMD_STATE_MESSAGEIN  0x07

enum
{
	FAST_SERIAL_DIR_IN,
	FAST_SERIAL_DIR_OUT
};

// VIA register indices (see m6522.h)
#define VIA_REG_ORB    0
#define VIA_REG_ORA    1
#define VIA_REG_ORA_NH 15

///////////////////////////////////////////////////////////////////////////////
// CPU bus functions
///////////////////////////////////////////////////////////////////////////////

u8 read65C02CMDHD(u16 address)
{
	return piCMDHD.Read(address);
}

void write65C02CMDHD(u16 address, const u8 value)
{
	piCMDHD.Write(address, value);
}

///////////////////////////////////////////////////////////////////////////////
// helpers
///////////////////////////////////////////////////////////////////////////////

// returns 0 if the 256 byte block has the CMD HD signature at its end
static int cmdhd_has_sig(const unsigned char* buf)
{
	static const unsigned char hdmagic[16] = { 0x43, 0x4d, 0x44, 0x20, 0x48, 0x44, 0x20, 0x20,
		0x8d, 0x03, 0x88, 0x8e, 0x02, 0x88, 0xea, 0x60 };
	return memcmp(&(buf[0xf0]), hdmagic, 16);
}

///////////////////////////////////////////////////////////////////////////////
// SCSI user hooks (device number fixing, head position display)
///////////////////////////////////////////////////////////////////////////////

void cmdhd_scsiread(scsi_context_t* scsi)
{
	PiCMDHD* hd = (PiCMDHD*)(scsi->p);
	unsigned track;

	// update the head position indicator; never 100 or above
	track = (unsigned)(((u64)scsi->address * 200) / (hd->imagesize + 1));
	if (track >= 200)
	{
		track = 199;
	}
	hd->headPosition = track;

	// leave if we are not the first disk
	if (scsi->target != 0 || scsi->lun != 0)
	{
		return;
	}

	// correct the device number on the fly since it is stored on the HD,
	// not by a switch or EEPROM (only when the user asks us to force it)
	if (hd->forcedDeviceID && hd->baselba != PiCMDHD::INVALID_BASELBA &&
		scsi->address == hd->baselba + 2)
	{
		// make sure it has the CMD signature first
		if (!cmdhd_has_sig(&(scsi->data_buf[256])))
		{
			if ((scsi->data_buf[0x1e1] != hd->forcedDeviceID) ||
				(scsi->data_buf[0x1e4] != hd->forcedDeviceID))
			{
				DEBUG_LOG("CMDHD: drive number is now %d; was %d in config block\r\n",
					hd->forcedDeviceID, (int)scsi->data_buf[0x1e1]);
				scsi->data_buf[0x1e1] = hd->forcedDeviceID;
				scsi->data_buf[0x1e4] = hd->forcedDeviceID;
			}
		}
		else
		{
			// block we had on record no longer has the signature, invalidate it
			hd->baselba = PiCMDHD::INVALID_BASELBA;
		}
	}
}

void cmdhd_scsiwrite(scsi_context_t* scsi)
{
	PiCMDHD* hd = (PiCMDHD*)(scsi->p);
	u32 temp;

	// A write may have created or moved the configuration block, so the next
	// reset has to look for it again.
	hd->scanNeeded = true;

	// keep track of the maximum lba written to
	temp = scsi->address + 1;
	if (temp > hd->imagesize)
	{
		hd->imagesize = temp;
	}

	hd->headPosition = (unsigned)(((u64)scsi->address * 200) / (hd->imagesize + 1));
}

// We don't actually format the disk, we just remove the 16 byte CMD signature
void cmdhd_scsiformat(scsi_context_t* scsi)
{
	PiCMDHD* hd = (PiCMDHD*)(scsi->p);
	int i;

	// leave if we are not the first disk
	if (scsi->target != 0 || scsi->lun != 0)
	{
		return;
	}

	// figure out where to start looking
	if (hd->baselba != PiCMDHD::INVALID_BASELBA)
	{
		if (hd->baselba < hd->imagesize)
		{
			scsi->address = hd->baselba + 2;
		}
		else
		{
			hd->baselba = PiCMDHD::INVALID_BASELBA;
			scsi->address = 2;
		}
	}
	else
	{
		scsi->address = 2;
	}
	hd->scanSector = 0;
	hd->scanTotal = hd->imagesize;
	// start searching, every 128 LBAs starting from 2 or known base
	while (scsi->address < hd->imagesize)
	{
		hd->scanSector = scsi->address;
		// stop if we hit the end of the file
		if (scsi_image_read_uncached(scsi) < 0)
		{
			break;
		}
		// check for the CMD sig
		if (!cmdhd_has_sig(&(scsi->data_buf[256])))
		{
			hd->baselba = scsi->address - 2;
			// we found it, zero it out
			for (i = 0; i < 16; i++)
			{
				scsi->data_buf[0x1f0 + i] = 0;
			}
			// write it back
			scsi_image_write(scsi);
			break;
		}
		// otherwise, keep looking
		scsi->address += 128;
	}
	hd->scanTotal = 0;
}

///////////////////////////////////////////////////////////////////////////////
// U11 i8255a hooks
///////////////////////////////////////////////////////////////////////////////

// Port A is the CMD parallel bus data; there is no CMD parallel bus (RAMLink)
// on a Pi so the lines are simply pulled up.
static void i8255a_set_pa(i8255a_state* ctx, u8 byte, s8 reg)
{
}

static u8 i8255a_get_pa(i8255a_state* ctx, s8 reg)
{
	return 0xff;
}

// Port B carries the CMD parallel bus handshake lines and the buttons.
// With no CMD parallel bus, PATN(PB7)/PCLK(PB6)/PREADY(PB0) read as 0
// (the bus lines are pulled up and these inputs see them inverted).
static void i8255a_set_pb(i8255a_state* ctx, u8 byte, s8 reg)
{
	PiCMDHD* hd = (PiCMDHD*)(ctx->p);

	hd->i8255a_o[1] = byte;
}

static u8 i8255a_get_pb(i8255a_state* ctx, s8 reg)
{
	PiCMDHD* hd = (PiCMDHD*)(ctx->p);

	return hd->i8255a_i[1] & 0x3e;
}

// Port C is for CMD Parallel bus, SCSI, and memory control (output only):
//  PC7 is for used for driving PREADY
//  PC6 is /PCLK
//  PC5 is /PEXT
//  PC4 is SCSI BSY
//  PC3 is SCSI RST
//  PC2 is SCSI ATN
//  PC1 is RAM mapping
//  PC0 is ROM control
static void i8255a_set_pc(i8255a_state* ctx, u8 byte, s8 reg)
{
	PiCMDHD* hd = (PiCMDHD*)(ctx->p);
	scsi_context_t* scsi = &hd->scsi;

	hd->i8255a_o[2] = byte;
	scsi->atn = ((byte & 4) != 0);
	scsi->rst = ((byte & 8) != 0);
	scsi->bsyi = ((byte & 16) != 0);
	scsi_process_noack(scsi);
}

static u8 i8255a_get_pc(i8255a_state* ctx, s8 reg)
{
	PiCMDHD* hd = (PiCMDHD*)(ctx->p);

	return hd->i8255a_i[2];
}

///////////////////////////////////////////////////////////////////////////////
// VIA port callbacks
///////////////////////////////////////////////////////////////////////////////

// U9 port A drives the SCSI data bus. Changes via the output register or the
// DDR propagate to the bus (the handshake itself happens on PRA access, in
// PiCMDHD::Read/Write).
static void Via9PortA_OnPortOut(void* pUserData, unsigned char status)
{
	PiCMDHD* hd = (PiCMDHD*)pUserData;

	scsi_set_bus(&hd->scsi, hd->via9.GetPortA()->GetOutput());
	scsi_process_noack(&hd->scsi);
}

// U9 port B outputs: PB4 = SEL, PB3 = data direction
static void Via9PortB_OnPortOut(void* pUserData, unsigned char status)
{
	PiCMDHD* hd = (PiCMDHD*)pUserData;

	hd->scsi.sel = (status & 0x10) ? 1 : 0;
	hd->scsi_dir = (status & 0x08) ? 1 : 0;
	scsi_process_noack(&hd->scsi);
}

// U10 port B drives the IEC bus (like a 1541's $1800 VIA) and the fast serial
// direction buffer.
static void Via10PortB_OnPortOut(void* pUserData, unsigned char status)
{
	PiCMDHD* hd = (PiCMDHD*)pUserData;

	if (status & VIA10_PINS_FAST_SER_DIR)
		hd->fastSerialDirection = FAST_SERIAL_DIR_OUT;
	else
		hd->fastSerialDirection = FAST_SERIAL_DIR_IN;

	IEC_Bus::PortB_OnPortOut(0, status);
}

///////////////////////////////////////////////////////////////////////////////
// PiCMDHD
///////////////////////////////////////////////////////////////////////////////

PiCMDHD::PiCMDHD()
{
	Initialise();
}

void PiCMDHD::Initialise()
{
	romLoaded = false;
	scanNeeded = true;
	LEDs = 0;
	forcedDeviceID = 0;
	headPosition = 0;
	imagesize = 0;
	baselba = INVALID_BASELBA;
	buttonWP = false;
	buttonSwap8 = false;
	buttonSwap9 = false;
	virtualButtonCountdown = 0;
	virtualButtonMask = 0;
	fastSerialDirection = FAST_SERIAL_DIR_IN;

	memset(ram, 0, sizeof(ram));
	memset(&scsi, 0, sizeof(scsi));
	scsi.p = this;

	memset(&i8255a, 0, sizeof(i8255a));
	i8255a.p = this;
	i8255a.set_pa = i8255a_set_pa;
	i8255a.set_pb = i8255a_set_pb;
	i8255a.set_pc = i8255a_set_pc;
	i8255a.get_pa = i8255a_get_pa;
	i8255a.get_pb = i8255a_get_pb;
	i8255a.get_pc = i8255a_get_pc;

	// Each VIA gets its own interrupt line; they are wire-ORed onto the CPU
	// IRQ in Update(). (Sharing one Interrupt object would let one VIA release
	// the other's assertion.)
	via9.ConnectIRQ(&via9IRQ);
	via10.ConnectIRQ(&via10IRQ);

	via9.GetPortA()->SetPortOut(this, Via9PortA_OnPortOut);
	via9.GetPortB()->SetPortOut(this, Via9PortB_OnPortOut);
	via10.GetPortB()->SetPortOut(this, Via10PortB_OnPortOut);

	// Unconnected U10 port B inputs read high.
	via10.GetPortB()->SetInput(0x40, true);

	scsi_reset(&scsi);
	// don't allow more than a 24-bit value -2 to be returned on disk query
	// as this will cause problems for CMDHD tools
	scsi.limit_imagesize = 0xfffffe;
	scsi.user_format = cmdhd_scsiformat;
	scsi.user_read = cmdhd_scsiread;
	scsi.user_write = cmdhd_scsiwrite;
}

bool PiCMDHD::SetROM(const unsigned char* data, unsigned size)
{
	romLoaded = false;

	if (size == 0x4000)
	{
		memcpy(rom, data, 0x4000);
		romLoaded = true;
	}
	else if (size == 0x8000)
	{
		// 32K dumps carry the 16K image twice; use the lower half.
		memcpy(rom, data, 0x4000);
		romLoaded = true;
	}
	return romLoaded;
}

void PiCMDHD::UpdateButtonInputs()
{
	u8 v = 0x7f;

	if (buttonWP) v &= ~0x08;
	if (buttonSwap8) v &= ~0x02;
	if (buttonSwap9) v &= ~0x04;
	v &= ~virtualButtonMask;

	i8255a_i[1] = v;
}

void PiCMDHD::SetWPButton(bool pressed)
{
	if (buttonWP != pressed)
	{
		buttonWP = pressed;
		UpdateButtonInputs();
	}
}

void PiCMDHD::SetSwap8Button(bool pressed)
{
	if (buttonSwap8 != pressed)
	{
		buttonSwap8 = pressed;
		UpdateButtonInputs();
	}
}

void PiCMDHD::SetSwap9Button(bool pressed)
{
	if (buttonSwap9 != pressed)
	{
		buttonSwap9 = pressed;
		UpdateButtonInputs();
	}
}

void PiCMDHD::Reset()
{
	int units;
	int i;

	// Write back a bounded amount before the drive restarts.
	//
	// Reset is a good moment to get dirty chunks onto the card - the machine
	// is usually about to be powered off or reconfigured, and the cache is the
	// only copy of anything acknowledged but not yet written. It is not a free
	// moment though: this path is reached from the IEC RESET line as well as
	// from the front panel button, and WaitUntilReset() returns as soon as the
	// host releases RESET. The computer is therefore already running and about
	// to poll a drive that cannot answer ATN while it is talking to the card.
	//
	// A full flush is unbounded - 32MB of cache is 8192 chunks, and at the 35ms
	// per access measured on this hardware that is minutes. So take a slice and
	// leave the rest to the idle path, which runs when the bus is genuinely
	// quiet. The slice is a quarter second of real time, which the host will
	// not notice on top of its own reset.
	ScsiImage::FlushSome(ScsiImage::RESET_FLUSH_MICROS);

	via9.Reset();
	via10.Reset();

	// setup default inputs to U11 (pullups/downs)
	i8255a_i[0] = 0xff;
	i8255a_i[1] = 0x7f;
	i8255a_i[2] = 0xe3;
	scsi_dir = 0;
	fastSerialDirection = FAST_SERIAL_DIR_IN;
	virtualButtonMask = 0;
	virtualButtonCountdown = 0;

	// The HD ROM does a series of hardware checks on reset if it doesn't
	// find a signature in memory; otherwise it skips to the boot loader.
	// Any virtually held buttons are released after this window.
	bool coldBoot = cmdhd_has_sig(&ram[0x9000]) != 0;

	// Look for the base lba again only if something has written to the disk
	// since the last look - that is the only way it can move. On a blank image
	// the scan reads the entire disk, so repeating it on every reset made
	// installing HDOS pay for it several times over.
	if (scanNeeded)
	{
		FindBaseLBA();
	}

	// count the number of connected drives
	units = 0;
	for (i = 0; i < SCSI_MAX_DISKS; i++)
	{
		if (disk[i].IsAttached())
		{
			units++;
		}
	}

	// if the image size is too small, put the drive in installation mode
	// (virtually hold SWAP8+SWAP9); but if there is more than one drive
	// connected, go to normal mode
	if (disk[0].IsAttached() && imagesize < 144)
	{
		if (units == 1)
		{
			virtualButtonMask = 0x06;
			virtualButtonCountdown = coldBoot ? 16000000 : 1000000;	// 2MHz cycles
			DEBUG_LOG("CMDHD: Image size too small, starting up in installation mode.\r\n");
		}
		else
		{
			// remove scsi ID 0
			disk[0].Detach();
		}
	}

	// Latch the mode the front panel asked for. The buttons are sampled as
	// the drive comes up, so this is the moment that decides it - and the
	// virtually held pair counts, since that is installation mode too.
	if ((buttonSwap8 && buttonSwap9) || virtualButtonMask == 0x06)
		panelMode = PANEL_INSTALL;
	else if (buttonWP)
		panelMode = PANEL_CONFIG;
	else
		panelMode = PANEL_NONE;

	UpdateButtonInputs();

	// propagate inputs to output
	i8255a_reset(&i8255a);

	m65c02.Reset();

	IEC_Bus::Reset();

	// On a real drive the outputs look like they are being pulled high (when
	// set to inputs) (taking an input from the front end of an inverter).
	IOPort* portB = via10.GetPortB();
	portB->SetInput(VIAPORTPINS_DATAOUT, true);
	portB->SetInput(VIAPORTPINS_CLOCKOUT, true);
	portB->SetInput(VIAPORTPINS_ATNAOUT, true);

	// CA1 (ATN, opposite polarity to the 1541) idles high while ATN is
	// released; initialise it so the first assertion is a falling edge.
	via10.InputCA1(true);
}

void PiCMDHD::FindBaseLBA()
{
	static u8 buf[512];
	u32 i;

	baselba = INVALID_BASELBA;

	if (!disk[0].IsAttached())
	{
		return;
	}

	// look for the configuration block, every 128 LBAs starting from 2
	i = 2;
	scanSector = 0;
	scanTotal = imagesize;
	while (i < imagesize)
	{
		scanSector = i;
		if (disk[0].ReadSectorUncached(i, buf) != 0)
		{
			break;
		}
		// the configuration block signature lives in the second half
		if (!cmdhd_has_sig(&buf[256]))
		{
			baselba = i - 2;
			break;
		}
		i += 128;
	}
	scanTotal = 0;
	scanNeeded = false;

	DEBUG_LOG("CMDHD: findbaselba=%u\r\n", baselba);
}

bool PiCMDHD::Insert(const char* filename, bool readOnly)
{
	Eject();

	if (!disk[0].Attach(filename, readOnly))
	{
		return false;
	}

	imagesize = disk[0].SizeInSectors();
	scanNeeded = true;
	FindBaseLBA();

	// look to see if there are more files with the same base name, but
	// s<ID><LUN> extensions: s01, ..., s10, ..., s67 (VICE convention)
	int len = (int)strlen(filename);
	if (len > 4 && len < 250 &&
		(filename[len - 1] == 'd' || filename[len - 1] == 'D') &&
		(filename[len - 2] == 'h' || filename[len - 2] == 'H') &&
		(filename[len - 3] == 'd' || filename[len - 3] == 'D') &&
		filename[len - 4] == '.')
	{
		char testname[256];
		int id, lun;

		for (id = 0; id < 7; id++)
		{
			// skip the first disk as it has the DHD extension
			for (lun = (id == 0) ? 1 : 0; lun < 8; lun++)
			{
				snprintf(testname, sizeof(testname), "%.*s%c%d%d",
					len - 3, filename,
					(filename[len - 3] == 'D') ? 'S' : 's', id, lun);
				if (disk[(id << 3) | lun].Attach(testname, readOnly))
				{
					DEBUG_LOG("CMDHD: attached %s as SCSI ID %d LUN %d\r\n", testname, id, lun);
				}
			}
		}
	}

	// hand the images to the scsi module
	for (int i = 0; i < SCSI_MAX_DISKS; i++)
	{
		scsi.file[i] = disk[i].IsAttached() ? &disk[i] : 0;
	}

	return true;
}

void PiCMDHD::Eject()
{
	for (int i = 0; i < SCSI_MAX_DISKS; i++)
	{
		disk[i].Detach();
		scsi.file[i] = 0;
	}
	imagesize = 0;
	baselba = INVALID_BASELBA;
	scanNeeded = true;
}

///////////////////////////////////////////////////////////////////////////////
// Memory map (see VICE cmdhd.c cmdhd_read/cmdhd_store)
///////////////////////////////////////////////////////////////////////////////

u8 PiCMDHD::Read(u16 address)
{
	u8 value;
	u8 reg;

	// Decode bits 15-12
	switch ((address >> 12) & 15)
	{
	case 0x0:
	case 0x1:
	case 0x2:
	case 0x3:
		return ram[address];
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
		// Since the ROM wants to read/write the RAM at 0xC000-0xFFFF,
		// when PC1 = 0, RAM from 0xC000-0xFFFF maps to 0x4000-0x7FFF
		if (i8255a_o[2] & 2)
		{
			return ram[(address & 0x3fff) | 0x4000];
		}
		else
		{
			return ram[(address & 0x3fff) | 0xC000];
		}
	case 0x8:
		// Decode bits 11-8
		switch ((address >> 8) & 15)
		{
		case 0x0: // 0x80xx U10
		case 0x1: // 0x81xx U10
			reg = address & 15;
			if (reg == VIA_REG_ORB)
				IEC_Bus::SampleIECInsNow();	// event-driven DATA/CLOCK sample, see iec_bus.h
			return via10.Read(reg);
		case 0x4: // 0x84xx U9
		case 0x5: // 0x85xx U9
			reg = address & 15;
			if (reg == VIA_REG_ORA || reg == VIA_REG_ORA_NH)
			{
				// Reading the SCSI data bus; PRA accesses generate ACK.
				via9.GetPortA()->SetInput(scsi_get_bus(&scsi));
				value = via9.Read(reg);
				if (scsi.state != SCSI_STATE_BUSFREE && reg == VIA_REG_ORA)
				{
					scsi_process_ack(&scsi);
				}
				else
				{
					scsi_process_noack(&scsi);
				}
				return value;
			}
			else if (reg == VIA_REG_ORB)
			{
				// Build the status byte presented by U13.
				u8 temp, state;

				if ((via9.GetFCR() & 0xf0) == 0xf0)
				{
					temp = scsi.sel & scsi.bsyo;
				}
				else
				{
					temp = (!scsi.sel) & scsi.bsyo;
				}

				// mask scsi state to cmd specific pld value
				switch (scsi.state)
				{
					case SCSI_STATE_DATAOUT:
						state = CMD_STATE_DATAOUT;
						break;
					case SCSI_STATE_DATAIN:
						state = CMD_STATE_DATAIN;
						break;
					case SCSI_STATE_COMMAND:
						state = CMD_STATE_COMMAND;
						break;
					case SCSI_STATE_STATUS:
						state = CMD_STATE_STATUS;
						break;
					case SCSI_STATE_MESSAGEOUT:
						state = CMD_STATE_MESSAGEOUT;
						break;
					case SCSI_STATE_MESSAGEIN:
						state = CMD_STATE_MESSAGEIN;
						break;
					default:
						state = CMD_STATE_STATUS;
				}

				via9.GetPortB()->SetInput((u8)((scsi.req << 7) | (scsi.ack << 6) | (temp << 5) |
					(scsi.sel << 4) | (scsi_dir << 3) | (state & 7)));
				value = via9.Read(reg);
				scsi_process_noack(&scsi);
				return value;
			}
			return via9.Read(reg);
		case 0x8: // 0x88xx U11
		case 0x9: // 0x89xx U11
			return i8255a_read(&i8255a, address & 3);
		case 0xc: // 0x8cxx RTC
		case 0xd: // 0x8dxx RTC
			return rtc.Read(address & 15);
		default:
			return ram[address];
		}
	case 0x9:
	case 0xa:
	case 0xb:
		return ram[address];
	case 0xc:
	case 0xd:
	case 0xe:
	case 0xf:
	default:
		// ROM is enabled when PC0 is 1, else RAM
		if (i8255a_o[2] & 1)
		{
			return rom[address & 0x3fff];
		}
		else
		{
			return ram[address];
		}
	}
	return 0xff;
}

void PiCMDHD::Write(u16 address, u8 value)
{
	u8 reg;

	// Decode bits 15-12
	switch ((address >> 12) & 15)
	{
	case 0x0:
	case 0x1:
	case 0x2:
	case 0x3:
		ram[address] = value;
		break;
	case 0x4:
	case 0x5:
	case 0x6:
	case 0x7:
		// Since the ROM wants to read/write the RAM at 0xC000-0xFFFF,
		// when PC1 = 0, RAM from 0xC000-0xFFFF maps to 0x4000-0x7FFF
		if (i8255a_o[2] & 2)
		{
			ram[(address & 0x3fff) | 0x4000] = value;
		}
		else
		{
			ram[(address & 0x3fff) | 0xC000] = value;
		}
		break;
	case 0x8:
		// Since the kernel is loaded into RAM from the HD on startup, if
		// U20 bit 5 = 0 then the memory (not IO) above 0x8000 is protected
		// from being written to
		switch ((address >> 8) & 15)
		{
		case 0x0: // 0x80xx U10
		case 0x1: // 0x81xx U10
			via10.Write(address & 15, value);
			break;
		case 0x4: // 0x84xx U9
		case 0x5: // 0x85xx U9
			reg = address & 15;
			via9.Write(reg, value);
			// PRA accesses generate the SCSI ACK handshake.
			// (The data bus itself is updated in the port A callback.)
			if (reg == VIA_REG_ORA)
			{
				if (scsi.state != SCSI_STATE_BUSFREE)
				{
					scsi_process_ack(&scsi);
				}
				else
				{
					scsi_process_noack(&scsi);
				}
			}
			break;
		case 0x8: // 0x88xx U11
		case 0x9: // 0x89xx U11
			i8255a_store(&i8255a, address & 3, value);
			break;
		case 0xc: // 0x8cxx RTC
		case 0xd: // 0x8dxx RTC
			rtc.Write(address & 15, value);
			break;
		case 0xf: // 0x8fxx U20
			// Although page 0x8F is RAM, all writes also go to U20.
			// The OS often reads from 0x8f00 to get past values to OR/AND them.
			LEDs = value;
			ram[(address & 255) | 0x8f00] = value;
			break;
		case 0xe: // 0x8exx unprotected RAM
			ram[(address & 255) | 0x8e00] = value;
			break;
		default:
			// Everything else writes to RAM if the write switch is on
			if (LEDs & 32)
			{
				ram[address] = value;
			}
			break;
		}
		break;
	case 0x9:
	case 0xa:
	case 0xb:
	case 0xc:
	case 0xd:
	case 0xe:
	case 0xf:
		if (LEDs & 32)
		{
			ram[address] = value;
		}
		break;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Per cycle update
///////////////////////////////////////////////////////////////////////////////

void PiCMDHD::Update()
{
	via9.Execute();
	via10.Execute();

	// Wire-OR the two VIA IRQ outputs onto the CPU IRQ line.
	if (via9IRQ.IsAsserted() || via10IRQ.IsAsserted())
		m65c02.IRQ.Assert();
	else
		m65c02.IRQ.Release();

	// Fast serial via U10's shift register. PB5 sets the buffer direction.
	if (fastSerialDirection == FAST_SERIAL_DIR_OUT)
	{
		// CB2 (data) is sent to DATA, CB1 (shift clock) is sent to SRQ.
		IEC_Bus::SetFastSerialData(!via10.GetShiftDataOut());	// Communication on fast serial is done after the inverter.
		IEC_Bus::SetFastSerialSRQ(via10.GetShiftClockOut());
	}
	else
	{
		// DATA is sent to CB2, SRQ is sent to CB1.
		via10.InputCB2(!IEC_Bus::GetPI_Data());	// Communication on fast serial is done before the inverter.
		via10.InputCB1(IEC_Bus::GetPI_SRQ());
	}

	// Installation mode holds SWAP8+SWAP9 virtually for a while after reset.
	if (virtualButtonCountdown)
	{
		if (--virtualButtonCountdown == 0)
		{
			virtualButtonMask = 0;
			UpdateButtonInputs();
		}
	}
}
