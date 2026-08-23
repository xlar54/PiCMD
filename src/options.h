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

#ifndef OPTIONS_H
#define OPTIONS_H

#include "types.h"

class TextParser
{
public:
	TextParser(void)
		: data(0)
	{
	}

	void SetData(char* buffer) { data = buffer; }

	char* GetToken(bool includeSpace = false);

protected:
	char* data;
	bool ParseComment();
	void SkipWhiteSpace();
};

class Options : public TextParser
{
public:
	Options(void);

	void Process(char* buffer);

	inline unsigned int GetOnResetChangeToStartingFolder() const { return onResetChangeToStartingFolder; }
	inline const char* GetAutoMountImageName() const { return autoMountImageName; }
	inline const char* GetRomFontName() const { return ROMFontName; }
	inline const char* GetRomNameCMDHD() const { return ROMNameCMDHD; }
	inline unsigned int GetCMDHDDeviceID() const { return CMDHDDeviceID; }
	inline unsigned int GetCMDHDCacheMB() const { return CMDHDCacheMB; }
	// GPIO used to pull the IEC ATN line low (0 = the drive cannot drive ATN).
	// 24 on a Pi1541io: its ATN level shifter is bidirectional, so the pin that
	// reads ATN can drive it too.
	inline unsigned int GetCMDHDAtnOutGPIO() const { return CMDHDAtnOutGPIO; }
	inline unsigned int GetCMDHDLcdLamps() const { return CMDHDLcdLamps; }
	inline unsigned int GetSupportUARTInput() const { return supportUARTInput; }

	inline unsigned int GraphIEC() const { return graphIEC; }
	inline unsigned int QuickBoot() const { return quickBoot; }
	inline unsigned int ShowOptions() const { return showOptions; }
	inline unsigned int DisplayPNGIcons() const { return displayPNGIcons; }
	inline unsigned int SplitIECLines() const { return splitIECLines; }
	inline unsigned int InvertIECInputs() const { return invertIECInputs; }
	inline unsigned int InvertIECOutputs() const { return invertIECOutputs; }
	inline unsigned int IgnoreReset() const { return ignoreReset; }


	inline unsigned int DisplayTemperature() const { return displayTemperature; }


	inline unsigned int ScreenWidth() const { return screenWidth; }
	inline unsigned int ScreenHeight() const { return screenHeight; }

	inline unsigned int I2CBusMaster() const { return i2cBusMaster; }
	inline unsigned int I2CLcdAddress() const { return i2cLcdAddress; }
	inline unsigned int I2CScan() const { return i2cScan; }
	inline unsigned int I2CLcdFlip() const { return i2cLcdFlip; }
	inline unsigned int I2CLcdOnContrast() const { return i2cLcdOnContrast; }
	inline unsigned int I2CLcdDimContrast() const { return i2cLcdDimContrast; }
	inline unsigned int I2CLcdDimTime() const { return i2cLcdDimTime; }
	inline unsigned int I2cLcdUseCBMChar() const { return i2cLcdUseCBMChar; }
	inline LCD_MODEL I2CLcdModel() const { return i2cLcdModel; }

	inline const char* GetLcdLogoName() const { return LcdLogoName; }

	inline float ScrollHighlightRate() const { return scrollHighlightRate; }

	// options.txt numbers the buttons 1-5; the arrays behind them are indexed
	// from 0, hence the -1. An out of range value used to sail straight
	// through: "buttonEnter = 0" gave 0u - 1 = 0xFFFFFFFF, truncated to 255
	// when stored in a u8, and inputmappings then read 250 elements past the
	// end of IEC_Bus's five element arrays.
	//
	// The two families need different handling. The CMD HD buttons are always
	// tested with "< 5" before use, so out of range can map to a sentinel that
	// disables the function - which is what options.txt already documents 0 to
	// mean. The browser buttons are stored as u8 and indexed unguarded, so
	// there is no disabled state available: they fall back to their default
	// instead, which at least leaves the browser usable.
	static const unsigned int BUTTON_DISABLED = 0xFFFFFFFF;

	static inline unsigned int ButtonIndex(unsigned int n)
	{
		return (n >= 1 && n <= 5) ? n - 1 : BUTTON_DISABLED;
	}

	static inline unsigned int BrowserButtonIndex(unsigned int n, unsigned int fallback)
	{
		return (n >= 1 && n <= 5) ? n - 1 : fallback - 1;
	}

	inline unsigned int GetButtonEnter() const { return BrowserButtonIndex(buttonEnter, 1); }
	inline unsigned int GetButtonUp() const { return BrowserButtonIndex(buttonUp, 2); }
	inline unsigned int GetButtonDown() const { return BrowserButtonIndex(buttonDown, 3); }
	inline unsigned int GetButtonBack() const { return BrowserButtonIndex(buttonBack, 4); }
	inline unsigned int GetButtonInsert() const { return BrowserButtonIndex(buttonInsert, 5); }

	// CMD HD front panel buttons (1-5 in options.txt, 0 = function disabled)
	inline unsigned int GetCMDHDButtonSwap8() const { return ButtonIndex(CMDHDButtonSwap8); }
	inline unsigned int GetCMDHDButtonSwap9() const { return ButtonIndex(CMDHDButtonSwap9); }
	inline unsigned int GetCMDHDButtonWP() const { return ButtonIndex(CMDHDButtonWP); }
	inline unsigned int GetCMDHDButtonReset() const { return ButtonIndex(CMDHDButtonReset); }
	inline unsigned int GetCMDHDButtonExit() const { return ButtonIndex(CMDHDButtonExit); }


	// Page up and down will jump a different amount based on the maximum number rows displayed.
	// Perhaps we should use some keyboard modifier to the the other screen?
	inline unsigned int KeyboardBrowseLCDScreen() const { return keyboardBrowseLCDScreen; }

	const char* GetLCDName() const { return LCDName; }


	static unsigned GetDecimal(char* pString);
	static float GetFloat(char* pString);

private:
	unsigned int CMDHDDeviceID;
	unsigned int CMDHDCacheMB;
	unsigned int CMDHDAtnOutGPIO;
	unsigned int CMDHDLcdLamps;
	unsigned int onResetChangeToStartingFolder;
	unsigned int supportUARTInput;
	unsigned int graphIEC;
	unsigned int quickBoot;
	unsigned int showOptions;
	unsigned int displayPNGIcons;
	unsigned int invertIECInputs;
	unsigned int invertIECOutputs;
	unsigned int splitIECLines;
	unsigned int ignoreReset;

	unsigned int displayTemperature;


	unsigned int screenWidth;
	unsigned int screenHeight;

	unsigned int i2cBusMaster;
	unsigned int i2cLcdAddress;
	unsigned int i2cScan;
	unsigned int i2cLcdFlip;
	unsigned int i2cLcdOnContrast;
	unsigned int i2cLcdDimContrast;
	unsigned int i2cLcdDimTime;
	unsigned int i2cLcdUseCBMChar;
	LCD_MODEL i2cLcdModel = LCD_UNKNOWN;

	float scrollHighlightRate;

	unsigned int keyboardBrowseLCDScreen;

        u8 buttonEnter;
        u8 buttonUp;
        u8 buttonDown;
        u8 buttonBack;
        u8 buttonInsert;
	u8 CMDHDButtonSwap8;
	u8 CMDHDButtonSwap9;
	u8 CMDHDButtonWP;
	u8 CMDHDButtonReset;
	u8 CMDHDButtonExit;

	char LCDName[256];
	char LcdLogoName[256];

	char autoMountImageName[256];
	char ROMFontName[256];
	char ROMNameCMDHD[256];


};
#endif
