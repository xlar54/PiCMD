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

#include "options.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define INVALID_VALUE	((unsigned) -1)

// strncpy leaves the destination unterminated when the source fills it. The
// string options all copy into 256 byte members, and only survived that by
// accident: Options is a global, so .bss had already zeroed the last byte.
static void CopyStringOption(char* dest, size_t size, const char* value)
{
	strncpy(dest, value, size - 1);
	dest[size - 1] = 0;
}

char* TextParser::GetToken(bool includeSpace)
{
	bool isSpace;

	do
	{
	} while (ParseComment());

	while (*data != '\0')
	{
		isSpace = isspace(*data);

		if (!isSpace || (includeSpace && isSpace))
			break;

		data++;
	}

	if (*data == '\0')
		return 0;

	char* pToken = data;

	while (*data != '\0')
	{
		isSpace = isspace(*data);

		if ((!includeSpace && isSpace) || (*data == '\n') || (*data == '\r'))
		{
			*data++ = '\0';
			break;
		}
		data++;
	}

	return pToken;
}

void TextParser::SkipWhiteSpace()
{
	while (*data != '\0')
	{
		if (!isspace(*data))
			break;

		data++;
	}
}

bool TextParser::ParseComment()
{
	SkipWhiteSpace();

	if (*data == '\0')
		return 0;

	if (data[0] != '/')
		return false;

	if (data[1] == '/')
	{
		// One line comment
		data += 2;

		while (*data)
		{
			if (*data == '\n')
				break;

			data++;
		}
		SkipWhiteSpace();
		return true;
	}
	else if (data[1] == '*')
	{
		// Multiline comment
		data += 2;

		while (*data)
		{
			if (*data++ == '*' && *data && *data == '/')
			{
				data++;
				break;
			}
		}
		SkipWhiteSpace();
		return true;
	}
	return false;
}


Options::Options(void)
	: TextParser()
	, CMDHDDeviceID(0)
	, CMDHDCacheMB(24)
	, CMDHDPreloadMB(18)
	, CMDHDAtnOutGPIO(0)
	, CMDHDLcdLamps(1)
	, onResetChangeToStartingFolder(0)
	, supportUARTInput(0)
	, graphIEC(0)
	, quickBoot(0)
	, showOptions(0)
	, displayPNGIcons(0)
	, invertIECInputs(0)
	, invertIECOutputs(1)
	, splitIECLines(0)
	, ignoreReset(0)
	, displayTemperature(0)
	, screenWidth(1024)
	, screenHeight(768)
	, i2cBusMaster(1)
	, i2cLcdAddress(0x3C)
	, i2cScan(0)
	, i2cLcdFlip(0)
	, i2cLcdOnContrast(127)
	, i2cLcdUseCBMChar(0)
	, i2cLcdModel(LCD_UNKNOWN)
	, scrollHighlightRate(0.125f)
	, keyboardBrowseLCDScreen(0)
        , buttonEnter(1)
        , buttonUp(2)
        , buttonDown(3)
        , buttonBack(4)
        , buttonInsert(5)
	, CMDHDButtonSwap8(1)
	, CMDHDButtonSwap9(2)
	, CMDHDButtonWP(3)
	, CMDHDButtonReset(4)
	, CMDHDButtonExit(5)
{
	autoMountImageName[0] = 0;
	strcpy(ROMFontName, "chargen");
	strcpy(LcdLogoName, "1541ii");
	ROMNameCMDHD[0] = 0;
}

#define ELSE_CHECK_DECIMAL_OPTION(Name) \
	else if (strcasecmp(pOption, #Name) == 0) \
	{ \
		unsigned nValue = 0; \
		if ((nValue = GetDecimal(pValue)) != INVALID_VALUE) \
			Name = nValue; \
	}
#define ELSE_CHECK_FLOAT_OPTION(Name) \
	else if (strcasecmp(pOption, #Name) == 0) \
	{ \
		float value = 0; \
		if ((value = GetFloat(pValue)) != INVALID_VALUE) \
			Name = value; \
	}

void Options::Process(char* buffer)
{
	SetData(buffer);

	char* pOption;
	while ((pOption = GetToken()) != 0)
	{
		/*char* equals = */GetToken();
		char* pValue = GetToken();

		// A name with no value at the end of the file. Every branch below reads
		// it, and strcasecmp/strncpy on a null pointer would take the Pi down
		// before it ever showed the boot screen.
		if (pValue == 0)
			break;

		if ((strcasecmp(pOption, "Font") == 0) || (strcasecmp(pOption, "ChargenFont") == 0))
		{
			CopyStringOption(ROMFontName, sizeof(ROMFontName), pValue);
		}
		else if ((strcasecmp(pOption, "AutoMountImage") == 0))
		{
			CopyStringOption(autoMountImageName, sizeof(autoMountImageName), pValue);
		}
		ELSE_CHECK_DECIMAL_OPTION(onResetChangeToStartingFolder)
		ELSE_CHECK_DECIMAL_OPTION(supportUARTInput)
		ELSE_CHECK_DECIMAL_OPTION(graphIEC)
		ELSE_CHECK_DECIMAL_OPTION(quickBoot)
		ELSE_CHECK_DECIMAL_OPTION(showOptions)
		ELSE_CHECK_DECIMAL_OPTION(displayPNGIcons)
		ELSE_CHECK_DECIMAL_OPTION(invertIECInputs)
		ELSE_CHECK_DECIMAL_OPTION(invertIECOutputs)
		ELSE_CHECK_DECIMAL_OPTION(splitIECLines)
		ELSE_CHECK_DECIMAL_OPTION(ignoreReset)
		ELSE_CHECK_DECIMAL_OPTION(displayTemperature)
		ELSE_CHECK_DECIMAL_OPTION(screenWidth)
		ELSE_CHECK_DECIMAL_OPTION(screenHeight)
		ELSE_CHECK_DECIMAL_OPTION(i2cBusMaster)
		ELSE_CHECK_DECIMAL_OPTION(i2cLcdAddress)
		ELSE_CHECK_DECIMAL_OPTION(i2cScan)
		ELSE_CHECK_DECIMAL_OPTION(i2cLcdFlip)
		ELSE_CHECK_DECIMAL_OPTION(i2cLcdOnContrast)
		ELSE_CHECK_DECIMAL_OPTION(i2cLcdDimContrast)
		ELSE_CHECK_DECIMAL_OPTION(i2cLcdDimTime)
		ELSE_CHECK_DECIMAL_OPTION(i2cLcdUseCBMChar)
		ELSE_CHECK_FLOAT_OPTION(scrollHighlightRate)
		ELSE_CHECK_DECIMAL_OPTION(keyboardBrowseLCDScreen)
		ELSE_CHECK_DECIMAL_OPTION(buttonEnter)
		ELSE_CHECK_DECIMAL_OPTION(buttonUp)
		ELSE_CHECK_DECIMAL_OPTION(buttonDown)
		ELSE_CHECK_DECIMAL_OPTION(buttonBack)
		ELSE_CHECK_DECIMAL_OPTION(buttonInsert)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDButtonSwap8)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDButtonSwap9)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDButtonWP)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDButtonReset)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDButtonExit)
		else if ((strcasecmp(pOption, "LCDLogoName") == 0))
		{
			CopyStringOption(LcdLogoName, sizeof(LcdLogoName), pValue);
		}
		else if ((strcasecmp(pOption, "LCDName") == 0))
		{
			CopyStringOption(LCDName, sizeof(LCDName), pValue);
			if (strcasecmp(pValue, "ssd1306_128x64") == 0)
				i2cLcdModel = LCD_1306_128x64;
			else if (strcasecmp(pValue, "ssd1306_128x32") == 0)
				i2cLcdModel = LCD_1306_128x32;
			else if (strcasecmp(pValue, "sh1106_128x64") == 0)
				i2cLcdModel = LCD_1106_128x64;
		}
		else if ((strcasecmp(pOption, "CMDHDRomName") == 0) || (strcasecmp(pOption, "ROMCMDHD") == 0))
		{
			CopyStringOption(ROMNameCMDHD, sizeof(ROMNameCMDHD), pValue);
		}
		ELSE_CHECK_DECIMAL_OPTION(CMDHDDeviceID)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDCacheMB)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDPreloadMB)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDAtnOutGPIO)
		ELSE_CHECK_DECIMAL_OPTION(CMDHDLcdLamps)
	}

	if (!SplitIECLines())
	{
		invertIECInputs = false;
		// If using non split lines then only the 1st bus master can be used (as ATN is using the 2nd)
		i2cBusMaster = 0;
	}
}

unsigned Options::GetDecimal(char* pString)
{
	if (pString == 0 || *pString == '\0')
		return INVALID_VALUE;

	// Report anything that is not a whole non-negative number as invalid so the
	// option keeps its default. This used to hand back strtol's zero for input
	// it could not parse at all, which turned a typo into a silent setting of
	// zero - and made the INVALID_VALUE check at every call site dead code.
	char* end = 0;
	long value = strtol(pString, &end, 0);

	if (end == pString || value < 0)
		return INVALID_VALUE;

	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		end++;
	if (*end != '\0')
		return INVALID_VALUE;

	return (unsigned)value;
}

float Options::GetFloat(char* pString)
{
	// Same contract as GetDecimal: unparseable input has to be distinguishable
	// from a real zero, or a typo silently becomes a setting of zero.
	if (pString == 0 || *pString == '\0')
		return (float)INVALID_VALUE;

	char* end = 0;
	float value = strtof(pString, &end);

	if (end == pString)
		return (float)INVALID_VALUE;

	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		end++;
	if (*end != '\0')
		return (float)INVALID_VALUE;

	return value;
}

