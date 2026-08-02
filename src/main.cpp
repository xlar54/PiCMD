// Pi1541 - A Commodore 1541 disk drive emulator
// Copyright(C) 2018 Stephen White
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

#include "defs.h"
#include <string.h>
#include <strings.h>
#include "types.h"
#include "timer.h"
#include "stb_image.h"
extern "C"
{
#include "rpi-aux.h"
#include "rpi-i2c.h"
#include "rpi-gpio.h"
#include "startup.h"
#include "cache.h"
#include "rpi-mailbox-interface.h"
#include "interrupt.h"
#include <uspi.h>
#include "rpi-mailbox.h"
}
#include "inputmappings.h"
#include "options.h"
#include "iec_bus.h"
#include "diskio.h"
#include "emmc.h"
#include "picmdhd.h"
#include "scsi.h"
#include "screen.h"
#include "filebrowser.h"
#include "screenlcd.h"
#include "spinlock.h"

#include "logo.h"
#include "ssd_logo.h"

unsigned versionMajor = 1;
unsigned versionMinor = 24;

#define COLOUR_BLACK RGBA(0, 0, 0, 0xff)
#define COLOUR_WHITE RGBA(0xff, 0xff, 0xff, 0xff)
#define COLOUR_RED RGBA(0xff, 0, 0, 0xff)
#define COLOUR_GREEN RGBA(0, 0xff, 0, 0xff)
#define COLOUR_CYAN RGBA(0, 0xff, 0xff, 0xff)
#define COLOUR_MAGENTA RGBA(0xff, 0, 0xff, 0xff)
#define COLOUR_YELLOW RGBA(0xff, 0xff, 0x00, 0xff)

enum EmulatingMode
{
	IEC_COMMANDS,
	EMULATING_CMDHD
};

volatile EmulatingMode emulating;

typedef void(*func_ptr)();

const long int tempBufferSize = 1024;
char tempBuffer[tempBufferSize];
// Filename the CMD HD boot ROM was loaded from, shown by the browser next to
// the device number. The ROM itself lives in PiCMDHD.
char cmdhdRomName[256] = { 0 };

const long int CBMFont_size = 4096;
unsigned char CBMFontData[4096];
unsigned char* CBMFont = 0;

#define LCD_LOGO_MAX_SIZE 1024
u8 LcdLogoFile[LCD_LOGO_MAX_SIZE];

u8 s_u8Memory[0xc000];

int numberOfUSBMassStorageDevices = 0;
PiCMDHD piCMDHD;
CEMMCDevice	m_EMMC;
Screen screen;
ScreenLCD* screenLCD = 0;
Options options;
const char* fileBrowserSelectedName;
InputMappings* inputMappings;
#if not defined(EXPERIMENTALZERO)
Keyboard* keyboard;
#endif
bool USBKeyboardDetected = false;
//bool resetWhileEmulating = false;
u16 pc;
#if defined(RPI2)
u32 clockCycles1MHz;
#endif

#if not defined(EXPERIMENTALZERO)
SpinLock core0RefreshingScreen;
#endif
unsigned int screenWidth = 1024;
unsigned int screenHeight = 768;

const char* termainalTextRed = "\E[31m";
const char* termainalTextNormal = "\E[0m";

// Hooks required for USPi library
extern "C"
{
	void LogWrite(const char *pSource, unsigned Severity, const char *pMessage, ...)
	{
		va_list args;
		va_start(args, pMessage);
		vprintf(pMessage, args);
		va_end(args);
	}

	int GetMACAddress(unsigned char Buffer[6])
	{
		rpi_mailbox_property_t* mp;

		RPI_PropertyInit();
		RPI_PropertyAddTag(TAG_GET_BOARD_MAC_ADDRESS);
		RPI_PropertyProcess();

		if ((mp = RPI_PropertyGet(TAG_GET_BOARD_MAC_ADDRESS)))
		{
			for (int i = 0; i < 6; ++i)
			{
				Buffer[i] = mp->data.buffer_8[i];
			}
			return 1;
		}

		return 0;
	}

	int SetPowerStateOn(unsigned id)
	{
		volatile u32* mailbox;
		u32 result;

		mailbox = (u32*)(PERIPHERAL_BASE | 0xB880);
		while (mailbox[6] & 0x80000000);
		mailbox[8] = 0x80;
		do {
			while (mailbox[6] & 0x40000000);
		} while (((result = mailbox[0]) & 0xf) != 0);
		return result == 0x80;
	}

	int GetTemperature(unsigned& value)
	{
		rpi_mailbox_property_t* mp;

		RPI_PropertyInit();
		RPI_PropertyAddTag(TAG_GET_TEMPERATURE);
		RPI_PropertyProcess();

		value = 0;
		if ((mp = RPI_PropertyGet(TAG_GET_TEMPERATURE)))
		{
			value = mp->data.buffer_32[1];
			return 1;
		}

		return 0;
	}

	void usDelay(unsigned nMicroSeconds)
	{
		unsigned before;
		unsigned after;
		for (u32 count = 0; count < nMicroSeconds; ++count)
		{
			before = read32(ARM_SYSTIMER_CLO);
			do
			{
				after = read32(ARM_SYSTIMER_CLO);
			} while (after == before);
		}
	}

	void MsDelay(unsigned nMilliSeconds)
	{
		usDelay(nMilliSeconds * 1000);
	}

	unsigned StartKernelTimer(unsigned nHzDelay, TKernelTimerHandler* pHandler, void* pParam, void* pContext)
	{
		return TimerStartKernelTimer(nHzDelay, pHandler, pParam, pContext);
	}

	void CancelKernelTimer(unsigned hTimer)
	{
		TimerCancelKernelTimer(hTimer);
	}

	typedef void TInterruptHandler(void* pParam);

	// USPi uses USB IRQ 9
	void ConnectInterrupt(unsigned nIRQ, TInterruptHandler* pHandler, void *pParam)
	{
		InterruptSystemConnectIRQ(nIRQ, pHandler, pParam);
	}
}

// Hooks for FatFs
DWORD get_fattime() { return 0; }	// If you have hardware RTC return a correct value here. THis can then be reflected in file modification times/dates.

void InitialiseHardware()
{
#if defined(RPI3)
	RPI_GpioVirtInit();
	RPI_TouchInit();
#endif

#if not defined(EXPERIMENTALZERO)
	screen.Open(screenWidth, screenHeight, 16);
#endif
	RPI_PropertyInit();
	RPI_PropertyAddTag(TAG_GET_MAX_CLOCK_RATE, ARM_CLK_ID);
	RPI_PropertyProcess();

	rpi_mailbox_property_t* mp;
	u32 MaxClk = 0;
	if ((mp = RPI_PropertyGet(TAG_GET_MAX_CLOCK_RATE)))
	{
		MaxClk = mp->data.buffer_32[1];
	}
	RPI_PropertyInit();
	RPI_PropertyAddTag(TAG_SET_CLOCK_RATE, ARM_CLK_ID, MaxClk);
	RPI_PropertyProcess();

#if defined(RPI2)
	// Enable clock cycle counter
	asm volatile ("mcr p15,0,%0,c9,c12,0" :: "r" (0b0001));
	asm volatile ("mcr p15,0,%0,c9,c12,1" :: "r" ((1 << 31)));

	clockCycles1MHz = MaxClk / 1000000;
#endif
}

void InitialiseLCD()
{
	FILINFO filLcdIcon;

	int i2cBusMaster = options.I2CBusMaster();
	int i2cLcdAddress = options.I2CLcdAddress();
	int i2cLcdFlip = options.I2CLcdFlip();
	int i2cLcdOnContrast = options.I2CLcdOnContrast();
	int i2cLcdDimContrast = options.I2CLcdDimContrast();
	int i2cLcdDimTime = options.I2CLcdDimTime();
	int i2cLcdUseCBMChar = options.I2cLcdUseCBMChar();
	LCD_MODEL i2cLcdModel = options.I2CLcdModel();

	if (i2cLcdModel)
	{
		int width = 128;
		int height = 64;
		if (i2cLcdModel == LCD_1306_128x32)
			height = 32;
		screenLCD = new ScreenLCD();
		screenLCD->Open(width, height, 1, i2cBusMaster, i2cLcdAddress, i2cLcdFlip, i2cLcdModel, i2cLcdUseCBMChar);
		screenLCD->SetContrast(i2cLcdOnContrast);
		screenLCD->ClearInit(0); // sh1106 needs this

		bool logo_done = false;
		if ( (height == 64) && (strcasecmp(options.GetLcdLogoName(), "1541ii") == 0) )
		{
			screenLCD->PlotRawImage(logo_ssd_1541ii, 0, 0, width, height);
			snprintf(tempBuffer, tempBufferSize, "Pi-CMD V%d.%02d", versionMajor, versionMinor);
			screenLCD->PrintText(false, 16, 0, tempBuffer, 0xffffffff);
			logo_done = true;
		}
		else if (( height == 64) && (strcasecmp(options.GetLcdLogoName(), "1541classic") == 0) )
		{
			screenLCD->PlotRawImage(logo_ssd_1541classic, 0, 0, width, height);
			logo_done = true;
		}
		else if (f_stat(options.GetLcdLogoName(), &filLcdIcon) == FR_OK && filLcdIcon.fsize <= LCD_LOGO_MAX_SIZE)
		{
			FIL fp;
			FRESULT res;

			res = f_open(&fp, filLcdIcon.fname, FA_READ);
			if (res == FR_OK)
			{
				u32 bytesRead;
				f_read(&fp, LcdLogoFile, LCD_LOGO_MAX_SIZE, &bytesRead);
				f_close(&fp);
				screenLCD->PlotRawImage(LcdLogoFile, 0, 0, width, height);
				logo_done = true;
			}
		}

		if (!logo_done)
		{
			snprintf(tempBuffer, tempBufferSize, "Pi-CMD V%d.%02d", versionMajor, versionMinor);
			int x = (width - 8*strlen(tempBuffer) ) /2;
			int y = (height-16)/2;
			screenLCD->PrintText(false, x, y, tempBuffer, 0x0);
		}
		screenLCD->RefreshScreen();
	}
	else
	{
		screenLCD = 0;
	}
}

// Which LCD text row the track/temperature line uses. The CMD HD's front panel
// lamps take the rows above it, so this moves down out of their way.
static u32 lcdTrackRow = 0;

void UpdateLCD(const char* track, unsigned temperature)
{
	if (screenLCD)
	{
#if not defined(EXPERIMENTALZERO)
		core0RefreshingScreen.Acquire();
#endif

		IEC_Bus::WaitMicroSeconds(100);

		// A whole-disk scan blocks the emulated CPU, so the lamps and the track
		// number cannot change while one runs. Show its progress instead, or the
		// drive looks hung for as long as it takes.
		if (piCMDHD.IsScanning())
		{
			snprintf(tempBuffer, tempBufferSize, "SCANNING %2d%%", piCMDHD.ScanPercent());
		}
		else if (piCMDHD.IsBootRomAnswering())
		{
			// Device 30: the boot ROM has the bus rather than HDOS. Which of
			// the three that is depends on what the front panel asked for at
			// reset, not on the device number.
			switch (piCMDHD.GetFrontPanelMode())
			{
				case PiCMDHD::PANEL_INSTALL:
					snprintf(tempBuffer, tempBufferSize, "INSTALL MODE"); break;
				case PiCMDHD::PANEL_CONFIG:
					snprintf(tempBuffer, tempBufferSize, "CONFIG MODE"); break;
				default:
					snprintf(tempBuffer, tempBufferSize, "NO INSTALL"); break;
			}
		}
		else if (options.DisplayTemperature())
			snprintf(tempBuffer, tempBufferSize, "%s %02dC", track, temperature);
		else
			snprintf(tempBuffer, tempBufferSize, "%s", track);

		// Pad to the full row. These strings are different lengths - "NO INSTALL"
		// is shorter than "SCANNING 97%" - and PrintText only paints the
		// characters it is given, so without this the tail of the longer one
		// survives underneath the shorter.
		u32 cols = screenLCD->Width() / screenLCD->GetFontWidth();
		if (cols > tempBufferSize - 1)
			cols = tempBufferSize - 1;
		for (u32 pad = strlen(tempBuffer); pad < cols; ++pad)
			tempBuffer[pad] = ' ';
		tempBuffer[cols] = 0;

		screenLCD->PrintText(false, 0, lcdTrackRow * screenLCD->GetFontHeight(), tempBuffer, 0, RGBA(0xff, 0xff, 0xff, 0xff));
		screenLCD->RefreshRows(lcdTrackRow, 1);

		IEC_Bus::WaitMicroSeconds(100);
#if not defined(EXPERIMENTALZERO)
		core0RefreshingScreen.Release();
#endif
	}
}

// Show the CMD HD's front panel indicator lamps on the LCD.
//
// The panel is monochrome, so a lit lamp cannot be a different colour - it is
// drawn as an inverse video block instead, which reads clearly at a glance.
// (ScreenLCD::PrintText treats any non zero background colour as inverse.)
// Only redrawn when a lamp actually changes, to keep I2C traffic off core 0.
static void UpdateLCDLamps(void)
{
#if not defined(EXPERIMENTALZERO)
	static u8 oldLamps = 0xff;
	static bool oldPower = false;

	if (!screenLCD || !options.GetCMDHDLcdLamps())
		return;

	if (emulating != EMULATING_CMDHD)
	{
		oldPower = false;
		oldLamps = 0xff;		// force a redraw when emulation next starts
		return;
	}

	u8 lamps = piCMDHD.LEDs;
	if (lamps == oldLamps && oldPower)
		return;
	oldLamps = lamps;
	oldPower = true;

	u32 fontHeight = screenLCD->GetFontHeight();
	u32 rows = screenLCD->Height() / fontHeight;
	if (rows == 0)
		return;

	bool on[6];
	const char* wide[6];
	const char* narrow[6];

	on[0] = true;								// POWER - the drive is running
	on[1] = piCMDHD.IsActivityLEDOn();
	on[2] = piCMDHD.IsErrorLEDOn();
	on[3] = piCMDHD.IsWriteProtectLEDOn();
	on[4] = piCMDHD.IsSwap8LEDOn();
	on[5] = piCMDHD.IsSwap9LEDOn();

	wide[0] = "POWER";   narrow[0] = "PWR";
	wide[1] = "ACTIVE";  narrow[1] = "ACT";
	wide[2] = "ERROR";   narrow[2] = "ERR";
	wide[3] = "WR PROT"; narrow[3] = "WP";
	wide[4] = "DRIVE 8"; narrow[4] = "D8";
	wide[5] = "DRIVE 9"; narrow[5] = "D9";

	core0RefreshingScreen.Acquire();
	IEC_Bus::WaitMicroSeconds(100);

	u32 lampRows;
	if (rows >= 4)
	{
		// 16 characters across, so two lamps per row in eight column fields
		lampRows = 3;
		for (int i = 0; i < 6; ++i)
		{
			snprintf(tempBuffer, tempBufferSize, "%-8s", wide[i]);
			screenLCD->PrintText(false, (i & 1) ? 8 * 8 : 0, (i >> 1) * fontHeight,
				tempBuffer, 0, on[i] ? RGBA(0xff, 0xff, 0xff, 0xff) : 0);
		}
	}
	else
	{
		// Only room for a couple of rows: four short tags each
		lampRows = 2;
		for (int i = 0; i < 6; ++i)
		{
			snprintf(tempBuffer, tempBufferSize, "%-4s", narrow[i]);
			screenLCD->PrintText(false, (i % 4) * 4 * 8, (i / 4) * fontHeight,
				tempBuffer, 0, on[i] ? RGBA(0xff, 0xff, 0xff, 0xff) : 0);
		}
	}
	screenLCD->RefreshRows(0, lampRows);

	// Keep the track/temperature line below the lamps if there is room for it.
	lcdTrackRow = (rows > lampRows) ? lampRows : 0;

	IEC_Bus::WaitMicroSeconds(100);
	core0RefreshingScreen.Release();
#endif
}

// This runs on core0 and frees up core1 to just run the emulator.
// Care must be taken not to crowd out the shared cache with core1 as this could slow down core1 so that it no longer can perform its duties in the 1us timings it requires.
void UpdateScreen()
{
#if not defined(EXPERIMENTALZERO)
	bool oldLED = false;
	bool oldMotor = false;
	bool oldATN = false;
	bool oldDATA = false;
	bool oldCLOCK = false;
	bool oldSRQ = false;
	bool refreshLCDStatusDisplay;

	u32 oldTrack = 0;
	u8 oldLamps = 0xff;
	u32 textColour = COLOUR_BLACK;
	u32 bgColour = COLOUR_WHITE;
	u32 oldTemperature = 0;

	RGBA atnColour = COLOUR_YELLOW;
	RGBA dataColour = COLOUR_GREEN;
	RGBA clockColour = COLOUR_CYAN;
	RGBA SRQColour = COLOUR_MAGENTA;
	RGBA BkColour = FileBrowser::Colour(VIC2_COLOUR_INDEX_BLUE);

	int height = screen.ScaleY(60);
	int screenHeight = screen.Height();
	int screenWidthM1 = screen.Width() - 1;
	int top, top2, top3;
	int bottom;
	int graphX = 0;
  //bool refreshUartStatusDisplay;
	unsigned temperature = 0;

	const long int tempBufferTrackSize = 16;
	char tempBufferTrack[tempBufferTrackSize];

	top = screenHeight - height / 2;
	bottom = screenHeight - 1;

	top2 = top - (bottom - top);
	top3 = top2 - (bottom - top);

	while (1)
	{
		bool value;
		u32 y = screen.ScaleY(STATUS_BAR_POSITION_Y);

		//RPI_UpdateTouch();
		//refreshUartStatusDisplay = false;

		bool led = false;
		bool motor = false;

		refreshLCDStatusDisplay = false;

		if (emulating == EMULATING_CMDHD)
		{
			led = piCMDHD.IsActivityLEDOn();
			motor = piCMDHD.IsErrorLEDOn();	// The CMD HD has no motor; show the error LED here instead.
		}

		value = led;
		if (value != oldLED)
		{
//			SetACTLed(value);
			oldLED = value;
			snprintf(tempBuffer, tempBufferSize, "%d", value);
			screen.PrintText(false, 4 * 8, y, tempBuffer, value ? COLOUR_RED : textColour, bgColour);
			//refreshUartStatusDisplay = true;
		}

		value = motor;
		if (value != oldMotor)
		{
			oldMotor = value;
			snprintf(tempBuffer, tempBufferSize, "%d", value);
			screen.PrintText(false, 12 * 8, y, tempBuffer, textColour, bgColour);
			//refreshUartStatusDisplay = true;
		}

		// The CMD HD's front panel indicator lamps. The board only has one LED
		// but the drive signals a lot through these six (configuration mode,
		// FPPS partition digits, write protect state, errors), so show them.
		if (emulating == EMULATING_CMDHD)
		{
			u8 lamps = piCMDHD.LEDs;
			if (lamps != oldLamps)
			{
				oldLamps = lamps;
				snprintf(tempBuffer, tempBufferSize, "%s %s %s %s %s %s",
					piCMDHD.IsActivityLEDOn() ? "ACT" : "   ",
					piCMDHD.IsErrorLEDOn() ? "ERR" : "   ",
					piCMDHD.IsSwap8LEDOn() ? "SW8" : "   ",
					piCMDHD.IsSwap9LEDOn() ? "SW9" : "   ",
					piCMDHD.IsWriteProtectLEDOn() ? "WP" : "  ",
					piCMDHD.IsGeosLEDOn() ? "GEOS" : "    ");
				screen.PrintText(false, 0, y - screen.GetFontHeight(), tempBuffer, textColour, bgColour);
			}
		}

		if (options.GraphIEC())
			screen.DrawLineV(graphX, top3, bottom, BkColour);

		value = IEC_Bus::GetPI_Atn();
		if (options.GraphIEC())
		{
			bottom = top2 - 2;
			if (value ^ oldATN)
			{
				screen.DrawLineV(graphX, top3, bottom, atnColour);
			}
			else
			{
				if (value) screen.PlotPixel(graphX, top3, atnColour);
				else screen.PlotPixel(graphX, bottom, atnColour);
			}
		}
		if (value != oldATN)
		{
			oldATN = value;
			snprintf(tempBuffer, tempBufferSize, "%d", value);
			screen.PrintText(false, 29 * 8, y, tempBuffer, textColour, bgColour);
			//refreshUartStatusDisplay = true;
		}

		value = IEC_Bus::GetPI_Data();
		if (options.GraphIEC())
		{
			bottom = top - 2;
			if (value ^ oldDATA)
			{
				screen.DrawLineV(graphX, top2, bottom, dataColour);
			}
			else
			{
				if (value) screen.PlotPixel(graphX, top2, dataColour);
				else screen.PlotPixel(graphX, bottom, dataColour);
			}
		}
		if (value != oldDATA)
		{
			oldDATA = value;
			snprintf(tempBuffer, tempBufferSize, "%d", value);
			screen.PrintText(false, 35 * 8, y, tempBuffer, textColour, bgColour);
			//refreshUartStatusDisplay = true;
		}

		value = IEC_Bus::GetPI_Clock();
		if (options.GraphIEC())
		{
			bottom = screenHeight - 1;
			if (value ^ oldCLOCK)
			{
				screen.DrawLineV(graphX, top, bottom, clockColour);
			}
			else
			{
				if (value) screen.PlotPixel(graphX, top, clockColour);
				else screen.PlotPixel(graphX, bottom, clockColour);
			}
		}
		if (value != oldCLOCK)
		{
			oldCLOCK = value;
			snprintf(tempBuffer, tempBufferSize, "%d", value);
			screen.PrintText(false, 41 * 8, y, tempBuffer, textColour, bgColour);
			//refreshUartStatusDisplay = true;
		}

		//value = IEC_Bus::GetPI_SRQ();
		//if (options.GraphIEC())
		//{
		//	if (value ^ oldSRQ)
		//	{
		//		screen.DrawLineV(graphX, 0, 100, SRQColour);
		//	}
		//	else
		//	{
		//		if (value) screen.PlotPixel(graphX, 0, SRQColour);
		//		else screen.PlotPixel(graphX, 100, SRQColour);
		//	}
		//}
		//if (value != oldSRQ)
		//{
		//	oldSRQ = value;
		////	snprintf(tempBuffer, tempBufferSize, "%d", value);
		////	screen.PrintText(false, 41 * 8, y, tempBuffer, textColour, bgColour);
		////	//refreshUartStatusDisplay = true;
		//}

		if (graphX++ > screenWidthM1) graphX = 0;
// black vertical line ahead of graph
		if (options.GraphIEC())
			screen.DrawLineV(graphX, top3, bottom, COLOUR_BLACK);

		u32 track;
		if (emulating == EMULATING_CMDHD)
		{
			// Show the rough head position (0-199) like VICE's track indicator.
			track = piCMDHD.GetHeadPosition();
			if (track != oldTrack)
			{
				oldTrack = track;
				snprintf(tempBufferTrack, tempBufferTrackSize, "%03d ", oldTrack);
				screen.PrintText(false, 20 * 8, y, tempBufferTrack, textColour, bgColour);
				//refreshUartStatusDisplay = true;
				refreshLCDStatusDisplay = true;
			}
		}
		if (emulating != IEC_COMMANDS)
		{
			if (options.DisplayTemperature())
			{
				if (GetTemperature(temperature))
				{
					temperature /= 1000;
					if (temperature != oldTemperature)
					{
						oldTemperature = temperature;
						//DEBUG_LOG("%0x %d %d\r\n", temp, temp, temp / 1000);
						snprintf(tempBuffer, tempBufferSize, "%02d", temperature);
						screen.PrintText(false, 43 * 8, y, tempBuffer, textColour, bgColour);
						refreshLCDStatusDisplay = true;
					}
				}
			}

			// Nothing else changes while a whole-disk scan runs - that is the
			// point of showing it - so ask for the refresh ourselves.
			static u8 oldDeviceNumber = 0xff;
			u8 deviceNumber = piCMDHD.GetDeviceNumber();
			if (deviceNumber != oldDeviceNumber)
			{
				oldDeviceNumber = deviceNumber;
				refreshLCDStatusDisplay = true;
				if (deviceNumber == 30)
				{
					const char* what;
					switch (piCMDHD.GetFrontPanelMode())
					{
						case PiCMDHD::PANEL_INSTALL: what = "INSTALL MODE - ready for HD-TOOLS   "; break;
						case PiCMDHD::PANEL_CONFIG:  what = "CONFIG MODE                         "; break;
						default:                     what = "NO INSTALL - no HDOS on this image  "; break;
					}
					snprintf(tempBuffer, tempBufferSize, "device 30: %s", what);
				}
				else if (deviceNumber)
					snprintf(tempBuffer, tempBufferSize, "device %-2d                      ", deviceNumber);
				else
					snprintf(tempBuffer, tempBufferSize, "                               ");
				screen.PrintText(false, 0, y - 32, tempBuffer, textColour, bgColour);
			}

			// Longest the emulated CPU has been stuck inside one SD access.
			// The drive cannot answer ATN while that happens, so if this creeps
			// into the milliseconds the computer will start seeing the drive
			// disappear mid-transfer.
			static u32 oldWorstStall = 0xffffffff;
			u32 worstStall = ScsiImage::WorstStallMicros();
			if (worstStall != oldWorstStall)
			{
				oldWorstStall = worstStall;
				snprintf(tempBuffer, tempBufferSize, "worst SD stall %u.%03u ms   ",
					worstStall / 1000, worstStall % 1000);
				screen.PrintText(false, 0, y - 48, tempBuffer, textColour, bgColour);
			}

			static u32 oldScanPercent = 0xffffffff;
			if (piCMDHD.IsScanning())
			{
				u32 pc = piCMDHD.ScanPercent();
				if (pc != oldScanPercent)
				{
					oldScanPercent = pc;
					refreshLCDStatusDisplay = true;
					snprintf(tempBuffer, tempBufferSize, "scanning disk %3d%% ", pc);
					screen.PrintText(false, 0, y - 16, tempBuffer, textColour, bgColour);
				}
			}
			else if (oldScanPercent != 0xffffffff)
			{
				oldScanPercent = 0xffffffff;
				refreshLCDStatusDisplay = true;
				snprintf(tempBuffer, tempBufferSize, "                    ");
				screen.PrintText(false, 0, y - 16, tempBuffer, textColour, bgColour);
			}

			// The lamps go first: they own the rows above the track line
			// and set which row that line uses.
			UpdateLCDLamps();

			if (refreshLCDStatusDisplay)
			{
				UpdateLCD(tempBufferTrack, temperature);
			}
		}


		//if (options.GetSupportUARTInput())
		//	UpdateUartControls(refreshUartStatusDisplay, oldLED, oldMotor, oldATN, oldDATA, oldCLOCK, oldTrack, romIndex);

		// Go back to sleep. The USB irq will wake us up again.
		__asm ("WFE");
	}
#endif
}

//--------------------------------------------------------------------------------------
// This is an implementation of FNV-1a
// (http://www.isthe.com/chongo/tech/comp/fnv/)
//--------------------------------------------------------------------------------------
u32 HashBuffer(const void* pBuffer, u32 length)
{
	u8*	pu8Buffer = (u8*)pBuffer;
	u32	hash = 0x811c9dc5U;

	while (length)
	{
		hash ^= *pu8Buffer++;
		hash *= 16777619U;
		--length;
	}
	return hash;
}

EmulatingMode BeginEmulating(FileBrowser* fileBrowser, const char* filenameForIcon)
{
	const char* imagePath = fileBrowser->SelectedDHDPath();

	if (imagePath && imagePath[0])
	{
		bool readOnly = fileBrowser->SelectedDHDReadOnly();
		if (piCMDHD.Insert(imagePath, readOnly))
		{
			fileBrowser->DisplayDHDInfo(imagePath, piCMDHD.imagesize, filenameForIcon);
			fileBrowser->ShowRomName();
			return EMULATING_CMDHD;
		}
		DEBUG_LOG("Failed to attach %s\r\n", imagePath);
	}
	fileBrowser->ClearSelections();
	inputMappings->WaitForClearButtons();
	return IEC_COMMANDS;
}

// The SD driver calls this while it spins waiting for the card. An access can
// take tens of milliseconds, and for all of it the emulated CPU is stopped
// inside a single instruction - so nothing refreshes the serial bus outputs
// and the drive appears to have been unplugged.
//
// On a real CMD HD that cannot happen: the 6522 acknowledges ATN in hardware,
// through the ATNA gate, whatever the CPU is doing. Servicing that gate here
// is what makes the emulation keep the same promise.
//
// It matters more than it sounds. A stall during an ordinary load is
// invisible, because the computer is waiting for the drive to talk and that
// wait is untimed. The same stall while the computer is asserting ATN is
// fatal: no DATA in response and the KERNAL reports ?DEVICE NOT PRESENT.
static void ServiceIECWhileCardBusy(void)
{
	if (emulating != EMULATING_CMDHD)
		return;
	// ReadEmulationModeCMDHD dereferences both without checking.
	if (!IEC_Bus::port || !IEC_Bus::VIA)
		return;

	IEC_Bus::ReadEmulationModeCMDHD();
	IEC_Bus::RefreshOutsCMDHD();
}

void CheckAutoMountImage(EXIT_TYPE reset_reason , FileBrowser* fileBrowser)
{
	const char* autoMountImageName = options.GetAutoMountImageName();
	if (autoMountImageName[0] != 0)
	{
		switch (reset_reason)
		{
			case EXIT_UNKNOWN:
			case EXIT_AUTOLOAD:
			case EXIT_RESET:
				fileBrowser->SelectAutoMountImage(autoMountImageName);
			break;
			case EXIT_CD:
			case EXIT_KEYBOARD:
			break;
			default:
			break;
		}
	}
}

// The CMD HD's four front panel buttons are mapped onto the board's buttons
// while emulating. The defaults are (all remappable in options.txt);-
//   button 1 = SWAP 8
//   button 2 = SWAP 9
//   button 3 = WRITE PROTECT
//   button 4 = RESET
//   button 5 = exit emulation (back to the file browser; not a CMD HD button)
//
// SWAP 8, SWAP 9 and WRITE PROTECT are momentary, exactly like the real front
// panel, so HDOS sees the press and decides what it means. RESET takes effect
// when the button is released, which is what lets the hardware's configuration
// combinations work: hold WRITE PROTECT (or a SWAP button) down, tap RESET, and
// the drive samples the held buttons as it starts up.
EXIT_TYPE EmulateCMDHD(FileBrowser* fileBrowser)
{
	EXIT_TYPE exitReason = EXIT_UNKNOWN;
	bool oldLED = false;
	unsigned ctBefore = 0;
	unsigned ctAfter = 0;
	int resetCount = 0;

	unsigned buttonSwap8 = options.GetCMDHDButtonSwap8();
	unsigned buttonSwap9 = options.GetCMDHDButtonSwap9();
	unsigned buttonWP = options.GetCMDHDButtonWP();
	unsigned buttonReset = options.GetCMDHDButtonReset();
	unsigned buttonExit = options.GetCMDHDButtonExit();

	bool resetButtonPrev = false;
	bool exitButtonPrev = false;

	inputMappings->directDiskSwapRequest = 0;
	// Force an update on all the buttons now before we start emulation mode.
	IEC_Bus::ReadBrowseMode();

	piCMDHD.m65c02.SetBusFunctions(read65C02CMDHD, write65C02CMDHD);

	IEC_Bus::VIA = &piCMDHD.via10;
	IEC_Bus::port = piCMDHD.via10.GetPortB();
	piCMDHD.Reset();	// will call IEC_Bus::Reset();

	IEC_Bus::LetSRQBePulledHigh();


#if defined(RPI2)
	asm volatile ("mrc p15,0,%0,c9,c13,0" : "=r" (ctBefore));
#else
	ctBefore = read32(ARM_SYSTIMER_CLO);
#endif

	while (exitReason == EXIT_UNKNOWN)
	{
		IEC_Bus::ReadEmulationModeCMDHD();

		// The CMD HD's 65C02 runs at 2MHz; two CPU cycles per 1MHz loop.
		for (int cycle2MHz = 0; cycle2MHz < 2; ++cycle2MHz)
		{
			piCMDHD.m65c02.Step();
			piCMDHD.Update();
		}

		IEC_Bus::RefreshOutsCMDHD();	// Now output all outputs.

		IEC_Bus::OutputLED = piCMDHD.IsActivityLEDOn();
#if defined(RPI3)
		if (IEC_Bus::OutputLED ^ oldLED)
		{
			SetACTLed(IEC_Bus::OutputLED);
			oldLED = IEC_Bus::OutputLED;
		}
#endif

		// Push cached writes to the card only once nothing has touched the disk
		// and ATN has been released for a while. Any SD access freezes the
		// emulated CPU - 35ms has been measured - and while it is frozen the
		// drive cannot acknowledge ATN, so the computer decides it has gone
		// away. Doing this on a timer instead landed it between SCSI commands,
		// which is the worst possible moment.
		{
			static u32 lastAccessCount = 0;
			static u32 quietLoops = 0;
			u32 accessCount = ScsiImage::AccessCounter();
			if (accessCount != lastAccessCount || IEC_Bus::IsAtnAsserted())
			{
				lastAccessCount = accessCount;
				quietLoops = 0;
			}
			else if (++quietLoops >= 500000)		// this loop runs at 1MHz
			{
				quietLoops = 0;
				ScsiImage::FlushIdle();
			}
		}

		IEC_Bus::ReadGPIOUserInput();

		// SWAP 8, SWAP 9 and WRITE PROTECT are momentary, just like the real
		// front panel; HDOS samples them and decides what they mean.
		if (buttonSwap8 < 5) piCMDHD.SetSwap8Button(IEC_Bus::GetInputButton(buttonSwap8));
		if (buttonSwap9 < 5) piCMDHD.SetSwap9Button(IEC_Bus::GetInputButton(buttonSwap9));
		if (buttonWP < 5) piCMDHD.SetWPButton(IEC_Bus::GetInputButton(buttonWP));

		// RESET fires when the button is released, so that holding one of the
		// other buttons down while tapping RESET reproduces the real drive's
		// start up configuration combinations.
		if (buttonReset < 5)
		{
			bool resetButtonNow = IEC_Bus::GetInputButton(buttonReset);
			if (resetButtonPrev && !resetButtonNow)
			{
				piCMDHD.Reset();
				resetCount = 0;
#if defined(RPI2)
				asm volatile ("mrc p15,0,%0,c9,c13,0" : "=r" (ctBefore));
#else
				ctBefore = read32(ARM_SYSTIMER_CLO);
#endif
				resetButtonPrev = false;
				continue;
			}
			resetButtonPrev = resetButtonNow;
		}

		// Other core will check the uart (as it is slow) (could enable uart irqs - will they execute on this core?)
#if not defined(EXPERIMENTALZERO)
		inputMappings->CheckKeyboardEmulationMode(0, 0);
#endif

		// Note: we deliberately do not use CheckButtonsEmulationMode()/Exit()
		// here as those map the browser's buttons onto disk swapping and would
		// steal the CMD HD's front panel buttons.
		bool exitEmulation = false;
		if (buttonExit < 5)
		{
			bool exitButtonNow = IEC_Bus::GetInputButton(buttonExit);
			if (exitButtonPrev && !exitButtonNow)
				exitEmulation = true;
			exitButtonPrev = exitButtonNow;
		}
#if not defined(EXPERIMENTALZERO)
		exitEmulation |= inputMappings->KeyboardEscape();
#endif
		bool exitDoAutoLoad = inputMappings->AutoLoad();

		if (exitEmulation || exitDoAutoLoad)
		{
			if (exitEmulation)
				exitReason = EXIT_KEYBOARD;
			if (exitDoAutoLoad)
				exitReason = EXIT_AUTOLOAD;
		}

		// A reset on the IEC bus resets the drive but, unlike a floppy drive
		// in a caddy workflow, a hard drive stays attached; keep emulating.
		bool reset = IEC_Bus::IsReset();
		if (reset)
			resetCount++;
		else
			resetCount = 0;

		if (resetCount > 10)
		{
			IEC_Bus::WaitUntilReset();
			piCMDHD.Reset();
			resetCount = 0;
#if defined(RPI2)
			asm volatile ("mrc p15,0,%0,c9,c13,0" : "=r" (ctBefore));
#else
			ctBefore = read32(ARM_SYSTIMER_CLO);
#endif
			continue;
		}

#if defined(RPI2)
		do  // Sync to the 1MHz clock
		{
			asm volatile ("mrc p15,0,%0,c9,c13,0" : "=r" (ctAfter));
		} while ((ctAfter - ctBefore) < clockCycles1MHz);
#else
		do	// Sync to the 1MHz clock
		{
			ctAfter = read32(ARM_SYSTIMER_CLO);
			unsigned ct = ctAfter - ctBefore;
			if (ct > 1)
			{
				// If this ever occurs then we have taken too long (ie >1us) and lost a cycle.
				// Cycle accuracy is now in jeopardy. If this occurs during critical communication loops then emulation can fail!
				//DEBUG_LOG("!");
			}
		} while (ctAfter == ctBefore);
#endif
		ctBefore = ctAfter;
	}

	// Make sure everything is flushed to the SD card before leaving.
	piCMDHD.Eject();

	return exitReason;
}

void emulator()
{
#if not defined(EXPERIMENTALZERO)
	Keyboard* keyboard = Keyboard::Instance();
#endif
	FileBrowser* fileBrowser;
	EXIT_TYPE exitReason = EXIT_UNKNOWN;

	fileBrowser = new FileBrowser(inputMappings, cmdhdRomName, options.DisplayPNGIcons(), &screen, screenLCD, options.ScrollHighlightRate());


	emulating = IEC_COMMANDS;
	while (1)
	{
		if (emulating == IEC_COMMANDS)
		{
			IEC_Bus::VIA = 0;
			IEC_Bus::port = 0;

			IEC_Bus::Reset();

			IEC_Bus::LetSRQBePulledHigh();
#if not defined(EXPERIMENTALZERO)
			core0RefreshingScreen.Acquire();
#endif
			IEC_Bus::WaitMicroSeconds(100);

			fileBrowser->ClearScreen();

			fileBrowserSelectedName = 0;
			fileBrowser->ClearSelections();

			fileBrowser->RefeshDisplay(); // Just redisplay the current folder.
#if not defined(EXPERIMENTALZERO)
			core0RefreshingScreen.Release();
#endif

			inputMappings->Reset();
#if not defined(EXPERIMENTALZERO)
			inputMappings->SetKeyboardBrowseLCDScreen(screenLCD && options.KeyboardBrowseLCDScreen());
#endif
			fileBrowser->ShowRomName();

			CheckAutoMountImage(exitReason, fileBrowser);

			while (emulating == IEC_COMMANDS)
			{
				fileBrowser->Update();
				if (fileBrowser->SelectionsMade())
					emulating = BeginEmulating(fileBrowser, fileBrowser->LastSelectionName());
				usDelay(1);
			}
		}
		else
		{
			exitReason = EmulateCMDHD(fileBrowser);

			DEBUG_LOG("Exited emulation\r\n");

			IEC_Bus::WaitUntilReset();
			emulating = IEC_COMMANDS;
	
			if ((exitReason == EXIT_RESET) && options.GetOnResetChangeToStartingFolder())
				fileBrowser->DisplayRoot(); // TO CHECK

			inputMappings->WaitForClearButtons();

#if not defined(EXPERIMENTALZERO)
			core0RefreshingScreen.Release();
#endif
		}
	}
	delete fileBrowser;
}

//static void MouseHandler(unsigned nButtons,
//	int nDisplacementX,		// -127..127
//	int nDisplacementY)		// -127..127
//{
//	DEBUG_LOG("Mouse: %x %d %d\r\n", nButtons, nDisplacementX, nDisplacementY);
//}

#ifdef HAS_MULTICORE
extern "C" 
{
	void run_core() 
	{
		enable_MMU_and_IDCaches();
		_enable_unaligned_access();

		DEBUG_LOG("emulator running on core %d\r\n", _get_core());
		emulator();
	}
}
static void start_core(int core, func_ptr func)
{
	write32(0x4000008C + 0x10 * core, (unsigned int)func);
	__asm ("SEV");	// and wake it up.
}
#endif

// Load the CMD HD boot ROM. Accepts 16K images and 32K dumps (which contain
// the 16K image twice).
static bool AttemptToLoadROM(const char* ROMName)
{
	FIL fp;
	static unsigned char bootROM[0x8000];

	char ROMName2[256] = "/roms/";

	if (ROMName[0] != '/')	// not a full path, prepend /roms/
		strncat (ROMName2, ROMName, 240);
	else
		ROMName2[0] = 0;

	if ( (FR_OK == f_open(&fp, ROMName, FA_READ))
		|| (FR_OK == f_open(&fp, ROMName2, FA_READ)) )
	{
		u32 bytesRead;
		SetACTLed(true);
		f_read(&fp, bootROM, sizeof(bootROM), &bytesRead);
		SetACTLed(false);
		f_close(&fp);

		if (piCMDHD.SetROM(bootROM, bytesRead))
		{
			strncpy(cmdhdRomName, ROMName, sizeof(cmdhdRomName) - 1);
			DEBUG_LOG("Opened CMD HD boot ROM %s (%d bytes)\r\n", ROMName, bytesRead);
			return true;
		}
		DEBUG_LOG("CMD HD boot ROM %s has the wrong size (%d bytes; expected 16384 or 32768)\r\n", ROMName, bytesRead);
		return false;
	}
	else
	{
		DEBUG_LOG("COULD NOT OPEN ROM FILE;- %s!\r\n", ROMName);
		return false;
	}
}

static void DisplayLogo()
{
#if not defined(EXPERIMENTALZERO)
	int w;
	int h;
	int channels_in_file;
	stbi_uc* image = stbi_load_from_memory((stbi_uc const*)I__logo_png, I__logo_png_size, &w, &h, &channels_in_file, 0);

	screen.PlotImage((u32*)image, 0, 0, w, h);

	snprintf(tempBuffer, tempBufferSize, "V%d.%02d", versionMajor, versionMinor);
	screen.PrintText(false, 20, 180, tempBuffer, FileBrowser::Colour(VIC2_COLOUR_INDEX_BLUE));
#endif
}

static void LoadOptions()
{
	FIL fp;
	FRESULT res;

	res = f_open(&fp, "options.txt", FA_READ);
	if (res == FR_OK)
	{
		u32 bytesRead;
		SetACTLed(true);
		f_read(&fp, s_u8Memory, sizeof(s_u8Memory), &bytesRead);
		SetACTLed(false);
		f_close(&fp);

		options.Process((char*)s_u8Memory);

		screenWidth = options.ScreenWidth();
		screenHeight = options.ScreenHeight();
	}
}

void DisplayOptions(int y_pos)
{
#if not defined(EXPERIMENTALZERO)
	// print confirmation of parsed options
	snprintf(tempBuffer, tempBufferSize, "ignoreReset = %d\r\n", options.IgnoreReset());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
	snprintf(tempBuffer, tempBufferSize, "splitIECLines = %d\r\n", options.SplitIECLines());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
	snprintf(tempBuffer, tempBufferSize, "invertIECInputs = %d\r\n", options.InvertIECInputs());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
	snprintf(tempBuffer, tempBufferSize, "invertIECOutputs = %d\r\n", options.InvertIECOutputs());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
	snprintf(tempBuffer, tempBufferSize, "i2cLcdAddress = %d\r\n", options.I2CLcdAddress());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
	snprintf(tempBuffer, tempBufferSize, "i2cLcdFlip = %d\r\n", options.I2CLcdFlip());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
	snprintf(tempBuffer, tempBufferSize, "LCDName = %s\r\n", options.GetLCDName());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
	snprintf(tempBuffer, tempBufferSize, "LcdLogoName = %s\r\n", options.GetLcdLogoName());
	screen.PrintText(false, 0, y_pos += 16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
#endif
}

void DisplayI2CScan(int y_pos)
{
#if not defined(EXPERIMENTALZERO)
	int BSCMaster = options.I2CBusMaster();

	snprintf(tempBuffer, tempBufferSize, "Scanning i2c bus %d ...\r\n", BSCMaster);
	screen.PrintText(false, 0, y_pos , tempBuffer, COLOUR_WHITE, COLOUR_BLACK);

	RPI_I2CInit(BSCMaster, 1);

	int count=0;
	int ptr = 0;
	ptr = snprintf (tempBuffer+ptr, tempBufferSize-ptr, "Found ");
	for (int address = 0; address<128; address++)
	{
		if (RPI_I2CScan(BSCMaster, address))
		{
			ptr += snprintf (tempBuffer+ptr, tempBufferSize-ptr, "%3d ", address);
			count++;
		}
	}
	if (count == 0)
		ptr += snprintf (tempBuffer+ptr, tempBufferSize-ptr, "Nothing");

	screen.PrintText(false, 0, y_pos+16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
#endif
}

static void CheckOptions()
{
	FIL fp;
	FRESULT res;

	u32 widthText, heightText;
	u32 widthScreen = screen.Width();
	u32 heightScreen = screen.Height();
	u32 xpos, ypos;

#if not defined(EXPERIMENTALZERO)
	const char* FontROMName = options.GetRomFontName();
	if (FontROMName)
	{
		char FontROMName2[256] = "/roms/";

		if (FontROMName[0] != '/')	// not a full path, prepend /roms/
			strncat (FontROMName2, FontROMName, 240);
		else
			FontROMName2[0] = 0;

		//DEBUG_LOG("%d Rom Name = %s\r\n", ROMIndex, ROMName);
		if ( (FR_OK == f_open(&fp, FontROMName, FA_READ))
			|| (FR_OK == f_open(&fp, FontROMName2, FA_READ)) )
		{
			u32 bytesRead;

			screen.Clear(COLOUR_BLACK);
			snprintf(tempBuffer, tempBufferSize, "Loading Font ROM %s\r\n", FontROMName);
			screen.MeasureText(false, tempBuffer, &widthText, &heightText);
			xpos = (widthScreen - widthText) >> 1;
			ypos = (heightScreen - heightText) >> 1;
			screen.PrintText(false, xpos, ypos, tempBuffer, COLOUR_WHITE, COLOUR_RED);

			SetACTLed(true);
			res = f_read(&fp, CBMFontData, CBMFont_size, &bytesRead);
			SetACTLed(false);
			f_close(&fp);
			if (res == FR_OK && bytesRead == CBMFont_size)
			{
				CBMFont = CBMFontData;
			}
			//DEBUG_LOG("Read ROM %s from options\r\n", ROMName);
		}
	}
#endif
	// Load the CMD HD boot ROM. Try the name from options.txt first, then
	// some common names.
	const char* ROMNameCMDHD = options.GetRomNameCMDHD();

	if (!(ROMNameCMDHD && ROMNameCMDHD[0] && AttemptToLoadROM(ROMNameCMDHD))
		&& !AttemptToLoadROM("cmdhd-bootrom.bin")
		&& !AttemptToLoadROM("bootromCMDHD-v2-80.bin")
		&& !AttemptToLoadROM("cmd_hd_bootrom_v2.80.bin")
		&& !AttemptToLoadROM("cmdhd.rom"))
	{
		snprintf(tempBuffer, tempBufferSize, "No CMD HD boot ROM found!\r\nPlease copy a CMD HD boot ROM (16K or 32K, eg v2.80) into the root folder\r\nof the SD card and name it 'cmdhd-bootrom.bin'\r\n(or set CMDHDRomName in options.txt).");
		screen.MeasureText(false, tempBuffer, &widthText, &heightText);
		xpos = (widthScreen - widthText) >> 1;
		ypos = (heightScreen - heightText) >> 1;
		do
		{
			screen.Clear(COLOUR_RED);
			IEC_Bus::WaitMicroSeconds(20000);
			screen.PrintText(false, xpos, ypos, tempBuffer, COLOUR_WHITE, COLOUR_RED);
			IEC_Bus::WaitMicroSeconds(100000);
		}
		while (1);
	}

	// Options for the CMD HD emulation itself.
	piCMDHD.SetForcedDeviceID((u8)options.GetCMDHDDeviceID());
	ScsiImage::InitCache(options.GetCMDHDCacheMB() * 1024 * 1024);
	EMMCBlockingWaitHook = ServiceIECWhileCardBusy;

	inputMappings->INPUT_BUTTON_ENTER = options.GetButtonEnter();
	inputMappings->INPUT_BUTTON_UP = options.GetButtonUp();
	inputMappings->INPUT_BUTTON_DOWN = options.GetButtonDown();
	inputMappings->INPUT_BUTTON_BACK = options.GetButtonBack();
	inputMappings->INPUT_BUTTON_INSERT = options.GetButtonInsert();
}

void Reboot_Pi()
{
	if (screenLCD)
		screenLCD->ClearInit(0);
	reboot_now();
}

bool SwitchDrive(const char* drive)
{
	FRESULT res;

	res = f_chdrive(drive);
	DEBUG_LOG("chdrive %s res %d\r\n", drive, res);
	return res == FR_OK;
}

void UpdateFirmwareToSD()
{
#if not defined(EXPERIMENTALZERO)
	const char* firmwareName = "kernel.img";
	DIR dir;
	FILINFO filInfo;
	FRESULT res;
	u32 widthText, heightText;
	u32 widthScreen = screen.Width();
	u32 heightScreen = screen.Height();
	u32 xpos, ypos;

	if (SwitchDrive("USB01:"))
	{
		char cwd[1024];
		if (f_getcwd(cwd, 1024) == FR_OK)
		{
			f_chdir("\\");

			bool found = f_findfirst(&dir, &filInfo, ".", firmwareName) == FR_OK;

			if (found)
			{
				char* mem = (char*)malloc((u32)filInfo.fsize);
				if (mem)
				{
					FIL fp;
					u32 bytes;
					res = f_open(&fp, firmwareName, FA_READ);
					if (res == FR_OK)
					{
						screen.Clear(COLOUR_BLACK);
						snprintf(tempBuffer, tempBufferSize, "Checking firmware on USB.\r\n");
						screen.MeasureText(false, tempBuffer, &widthText, &heightText);
						xpos = (widthScreen - widthText) >> 1;
						ypos = (heightScreen - heightText) >> 1;
						screen.PrintText(false, xpos, ypos, tempBuffer, COLOUR_WHITE, COLOUR_RED);

						res = f_read(&fp, mem, (u32)filInfo.fsize, &bytes);
						f_close(&fp);
						if ((res == FR_OK) && ((u32)filInfo.fsize == bytes))
						{
							if (SwitchDrive("SD:"))
							{
								if (f_chdir("\\") == FR_OK)
								{
									bool same = true;
									if (FR_OK == f_open(&fp, firmwareName, FA_READ))
									{
										char* ptr = mem;
										char buffer[256];
										unsigned bufferIndex = 0;
										unsigned bytesRead;
										do
										{
											f_read(&fp, buffer, 256, &bytesRead);

											for (unsigned index = 0; index < bytesRead; ++index)
											{
												if (buffer[index] != mem[bufferIndex + index])
												{
													same = false;
													break;
												}
											}
											bufferIndex += bytesRead;
										} while (same && (bufferIndex < (u32)filInfo.fsize));
										f_close(&fp);
									}

									screen.Clear(COLOUR_BLACK);
									if (!same && (FR_OK == f_open(&fp, firmwareName, FA_CREATE_ALWAYS | FA_WRITE)))
									{
										snprintf(tempBuffer, tempBufferSize, "Updating firmware.\r\n");
										screen.MeasureText(false, tempBuffer, &widthText, &heightText);
										xpos = (widthScreen - widthText) >> 1;
										ypos = (heightScreen - heightText) >> 1;
										screen.PrintText(false, xpos, ypos, tempBuffer, COLOUR_WHITE, COLOUR_RED);

										res = f_write(&fp, mem, (u32)filInfo.fsize, &bytes);
										f_close(&fp);
									}
								}
							}
						}
					}
					else
					{
						DEBUG_LOG("failed to open file %s %d\r\n", firmwareName, (int)res);
					}
					free(mem);
				}
			}

			SwitchDrive("USB01:");
			f_chdir(cwd);
		}
	}
#endif
}

void DisplayMessage(int x, int y, bool LCD, const char* message, u32 textColour, u32 backgroundColour)
{
#if not defined(EXPERIMENTALZERO)
	char buffer[256] = { 0 };

	if (!LCD)
	{
		x = screen.ScaleX(x);
		y = screen.ScaleY(y);

		screen.PrintText(false, x, y, (char*)message, textColour, backgroundColour);
	}
	else if (screenLCD)
	{
		RGBA BkColour = RGBA(0, 0, 0, 0xFF);

		core0RefreshingScreen.Acquire();

		screenLCD->Clear(BkColour);
		screenLCD->PrintText(false, x, y, (char*)message, textColour, backgroundColour);
		screenLCD->SwapBuffers();

		core0RefreshingScreen.Release();
	}
#else
	RGBA BkColour = RGBA(0, 0, 0, 0xFF);

	screenLCD->Clear(BkColour);
	screenLCD->PrintText(false, x, y, (char*)message, textColour, backgroundColour);
	screenLCD->SwapBuffers();

#endif
}

extern "C"
{
	void kernel_main(unsigned int r0, unsigned int r1, unsigned int atags)
	{
		FRESULT res;
		FATFS fileSystemSD;
		FATFS fileSystemUSB[16];

		m_EMMC.Initialize();

#if not defined(EXPERIMENTALZERO)
		RPI_AuxMiniUartInit(115200, 8);
#endif

		disk_setEMM(&m_EMMC);
		f_mount(&fileSystemSD, "SD:", 1);

		LoadOptions();

		InitialiseHardware();
		enable_MMU_and_IDCaches();
		_enable_unaligned_access();

		write32(ARM_GPIO_GPCLR0, 0xFFFFFFFF);

		DisplayLogo();

		InitialiseLCD();
#if not defined(EXPERIMENTALZERO)
		int y_pos = 184;
		snprintf(tempBuffer, tempBufferSize, "Copyright(C) 2018 Stephen White");
		screen.PrintText(false, 0, y_pos+=16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
		snprintf(tempBuffer, tempBufferSize, "This program comes with ABSOLUTELY NO WARRANTY.");
		screen.PrintText(false, 0, y_pos+=16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);
		snprintf(tempBuffer, tempBufferSize, "This is free software, and you are welcome to redistribute it.");
		screen.PrintText(false, 0, y_pos+=16, tempBuffer, COLOUR_WHITE, COLOUR_BLACK);

		if (options.I2CScan())
			DisplayI2CScan(y_pos+=32);

		if (options.ShowOptions())
			DisplayOptions(y_pos+=32);

#endif
		//if (!options.QuickBoot())
			//IEC_Bus::WaitMicroSeconds(3 * 1000000);

		InterruptSystemInitialize();
#if not defined(EXPERIMENTALZERO)
		TimerSystemInitialize();

		USPiInitialize();

		DEBUG_LOG("\r\n");

		numberOfUSBMassStorageDevices = USPiMassStorageDeviceAvailable();
		DEBUG_LOG("%d USB Mass Storage Devices found\r\n", numberOfUSBMassStorageDevices);

		USBKeyboardDetected = USPiKeyboardAvailable();
		if (!USBKeyboardDetected)
			DEBUG_LOG("Keyboard not found\r\n");
		else
			DEBUG_LOG("Keyboard found\r\n");

		//if (!USPiMouseAvailable())
		//	DEBUG_LOG("Mouse not found\r\n");
		//else
		//	DEBUG_LOG("Mouse found\r\n");

		keyboard = new Keyboard();
#endif
		inputMappings = new InputMappings();
		//USPiMouseRegisterStatusHandler(MouseHandler);


		CheckOptions();

		IEC_Bus::SetSplitIECLines(options.SplitIECLines());
		IEC_Bus::SetInvertIECInputs(options.InvertIECInputs());
		IEC_Bus::SetInvertIECOutputs(options.InvertIECOutputs());
		IEC_Bus::SetIgnoreReset(options.IgnoreReset());
		IEC_Bus::SetAtnOutGPIO(options.GetCMDHDAtnOutGPIO());
#if not defined(EXPERIMENTALZERO)
		for (int USBDriveIndex = 0; USBDriveIndex < numberOfUSBMassStorageDevices; ++USBDriveIndex)
		{
			char USBDriveId[16];
			disk_setUSB(USBDriveIndex);
			sprintf(USBDriveId, "USB%02d:", USBDriveIndex + 1);
			res = f_mount(&fileSystemUSB[USBDriveIndex], USBDriveId, 1);
		}
		if (numberOfUSBMassStorageDevices > 0)
		{
			if (SwitchDrive("USB01:"))
				UpdateFirmwareToSD();
		}
#endif
		ChangeToImageFolder();



		IEC_Bus::Initialise();
		if (screenLCD)
			screenLCD->ClearInit(0);

#ifdef HAS_MULTICORE
		start_core(3, _spin_core);
		start_core(2, _spin_core);
#ifdef USE_MULTICORE
		start_core(1, _init_core);
		UpdateScreen();		// core0 now loops here where it will handle interrupts and passively update the screen.
		while (1);
#else
		start_core(1, _spin_core);
#endif
#endif
#ifndef USE_MULTICORE
		emulator();	// If only one core the emulator runs on it now.
#endif
	}
}

