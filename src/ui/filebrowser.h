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

#ifndef FileBrowser_H
#define FileBrowser_H
#include <assert.h>
#include <string.h>
#include "ff.h"
#include <vector>
#include "types.h"
#include "screenbase.h"
#include "inputmappings.h"

// The folder images live in. Pi1541 called this "/1541"; Pi-CMD mounts CMD HD
// images, so it is named for those instead.
#define IMAGE_FOLDER "/cmd-images"

static inline FRESULT ChangeToImageFolder(void)
{
	return f_chdir(IMAGE_FOLDER);
}

// Is this a CMD HD image, as produced by hdimage.py from the cmd-utils repo?
// Both extensions name the same thing: .dhd is what CMD's own tools and VICE
// use, .img is what people tend to end up with after writing one out.
// Defined in FileBrowser.cpp - it has ten call sites and is not worth inlining
// into each of them.
bool IsDiskImageExtention(const char* name);

#define VIC2_COLOUR_INDEX_BLACK		0
#define VIC2_COLOUR_INDEX_WHITE		1
#define VIC2_COLOUR_INDEX_RED		2
#define VIC2_COLOUR_INDEX_CYAN		3
#define VIC2_COLOUR_INDEX_MAGENTA	4
#define VIC2_COLOUR_INDEX_GREEN		5
#define VIC2_COLOUR_INDEX_BLUE		6
#define VIC2_COLOUR_INDEX_YELLOW	7
#define VIC2_COLOUR_INDEX_ORANGE	8
#define VIC2_COLOUR_INDEX_BROWN		9
#define VIC2_COLOUR_INDEX_PINK		10
#define VIC2_COLOUR_INDEX_DGREY		11
#define VIC2_COLOUR_INDEX_GREY		12
#define VIC2_COLOUR_INDEX_LGREEN	13
#define VIC2_COLOUR_INDEX_LBLUE		14
#define VIC2_COLOUR_INDEX_LGREY		15

#define STATUS_BAR_POSITION_Y (40 * 16 + 10)

#define KEYBOARD_SEARCH_BUFFER_SIZE 512

class FileBrowser
{
public:

	class BrowsableList;

	class BrowsableListView
	{
	public:
		BrowsableListView(BrowsableList* list, InputMappings* inputMappings, ScreenBase* screen, u32 columns, u32 rows, u32 positionX, u32 positionY, bool lcdPgUpDown)
			: list(list)
			, inputMappings(inputMappings)
			, screen(screen)
			, columns(columns)
			, rows(rows)
			, positionX(positionX)
			, positionY(positionY)
			, lcdPgUpDown(lcdPgUpDown)
			, highlightScrollOffset(0)
			, highlightScrollStartCount(0)
			, highlightScrollEndCount(0)
			, scrollHighlightRate()
		{
		}

		void Refresh();
		void RefreshLine(u32 entryIndex, u32 x, u32 y, bool selected);
		void RefreshHighlightScroll();
		bool CheckBrowseNavigation(bool pageOnly);

		BrowsableList* list;
		u32 offset;
		InputMappings* inputMappings;
		ScreenBase* screen;
		u32 columns;
		u32 rows;
		u32 positionX;
		u32 positionY;
		bool lcdPgUpDown;
		u32 highlightScrollOffset;
		u32 highlightScrollStartCount;
		u32 highlightScrollEndCount;
		float scrollHighlightRate;
	};

	class BrowsableList
	{
	public:
		BrowsableList();

		void Clear()
		{
			u32 index;
			entries.clear();
			current = 0;
			currentIndex = 0;
			for (index = 0; index < views.size(); ++index)
			{
				views[index].offset = 0;
			}
		}

		void AddView(ScreenBase* screen, InputMappings* inputMappings, u32 columns, u32 rows, u32 positionX, u32 positionY, bool lcdPgUpDown)
		{
			this->inputMappings = inputMappings;
			BrowsableListView view(this, inputMappings, screen, columns, rows, positionX, positionY, lcdPgUpDown);
			views.push_back(view);
		}

		void ClearSelections();

		void SetCurrent()
		{
			if (entries.size() > 0)
			{
				Entry* currentEntry = &entries[currentIndex];
				if (currentEntry != current)
				{
					current = currentEntry;
					currentHighlightTime = scrollHighlightRate;
				}
			}
			else
			{
				current = 0;
			}
		}

		struct Entry
		{
			// Both FILINFOs get cleared, not just caddyIndex. f_readdir fills
			// them in completely, but the synthetic entries - "..", the device
			// list - are built by hand and only ever OR AM_DIR into fattrib,
			// so anything else left on the stack stayed set. A stray AM_RDO
			// makes an entry render as read only and be treated that way.
			Entry() : caddyIndex(-1)
			{
				memset(&filImage, 0, sizeof(filImage));
				memset(&filIcon, 0, sizeof(filIcon));
			}
			FILINFO filImage;
			FILINFO filIcon;
			int caddyIndex;
		};

		Entry* FindEntry(const char* name);

		void RefreshViews();
		void RefreshViewsHighlightScroll();
		bool CheckBrowseNavigation();

		InputMappings* inputMappings;
		std::vector<Entry> entries;
		Entry* current;
		u32 currentIndex;
		float currentHighlightTime;
		float scrollHighlightRate;

		u32 lastUpdateTime;
		char searchPrefix[KEYBOARD_SEARCH_BUFFER_SIZE];
		u32 searchPrefixIndex;
		u32 searchLastKeystrokeTime;
		std::vector<BrowsableListView> views;
	};

	FileBrowser(InputMappings* inputMappings, const char* romName, bool displayPNGIcons, ScreenBase* screenMain, ScreenBase* screenLCD, float scrollHighlightRate);

	void SelectAutoMountImage(const char* image);
	void DisplayRoot();
	void Update();

	void RefeshDisplay();

	void DisplayStatusBar();

	void FolderChanged();
	void PopFolder();

	bool SelectionsMade() { return selectionsMade; }
	const char* LastSelectionName() { return lastSelectionName; }
	void SetLastSelectionName(const char* name)
	{
		if (!name)
		{
			lastSelectionName[0] = 0;
			return;
		}
		strncpy(lastSelectionName, name, sizeof(lastSelectionName) - 1);
		lastSelectionName[sizeof(lastSelectionName) - 1] = 0;
	}
	void ClearSelections();

	// DHD images are streamed from the SD card rather than loaded into RAM,
	// so a selection just records the full path of the chosen image.
	const char* SelectedDHDPath() { return selectedDHDPath; }
	bool SelectedDHDReadOnly() { return selectedDHDReadOnly; }
	bool SetSelectedDHD(const char* filename, bool readOnly);
	void DisplayDHDInfo(const char* imagePath, u32 sizeInSectors, const char* filenameForIcon);

	void ShowRomName();
	

	void ClearScreen();

	static u32 Colour(int index);

	static void RefreshDevicesEntries(std::vector<FileBrowser::BrowsableList::Entry>& entries, bool toLower);

	void SetScrollHighlightRate(float value) { scrollHighlightRate = value; }


private:
	void DisplayPNG(FILINFO& filIcon, int x, int y);
	void RefreshFolderEntries();

	void UpdateInputFolders();

	void UpdateCurrentHighlight();
	//void RefeshDisplayForBrowsableList(FileBrowser::BrowsableList* browsableList, int xOffset, bool showSelected = true);
	bool FillCaddyWithSelections();

	bool AddToCaddy(FileBrowser::BrowsableList::Entry* current);
	bool AddImageToCaddy(FileBrowser::BrowsableList::Entry* current);

	bool CheckForPNG(const char* filename, FILINFO& filIcon);
	void DisplayPNG();


	// returns the volume index if at the root of a volume else -1
	int IsAtRootOfDevice();

	InputMappings* inputMappings;

	BrowsableList folder;
	bool selectionsMade;
	char selectedDHDPath[512];
	bool selectedDHDReadOnly;
	// Owned, not borrowed. This used to point straight at a FILINFO inside
	// folder.entries, which is only safe while nothing refreshes or reallocates
	// that vector between the selection and the read of it. Nothing does today,
	// but it is a dangling pointer waiting for someone to add a refresh, and a
	// copy costs one buffer.
	char lastSelectionName[_MAX_LFN + 1];
	const char* romName;
	bool displayPNGIcons;
	bool buttonChangedROMDevice;

	BrowsableList caddySelections;
#if not defined(EXPERIMENTALZERO)
	ScreenBase* screenMain;
#endif
	ScreenBase* screenLCD;
	float scrollHighlightRate;

	bool displayingDevices;
};
#endif
