#include "harness.h"
#include "ui/startupimage.h"
#include <vector>

void RunStartupImageTests()
{
	struct Entry { struct { const char* fname; unsigned fattrib; } filImage; };
	const unsigned directory = 0x10;
	std::vector<Entry> entries = {
		{{"..", directory}}, {{"folder.img", directory}},
		{{"a.bin", 0}}, {{"alpha.DHD", 0}}, {{"hd0.img", 0}}, {{"notes.txt", 0}}
	};
	TEST_CASE("startup picks an image and honors the configured disk");
	CHECK_EQ(FindStartupImage(entries, "", directory), 3);
	CHECK_EQ(FindStartupImage(entries, "HD0.IMG", directory), 4);
	CHECK_EQ(FindStartupImage(entries, "missing.img", directory), -1);
	CHECK_EQ(FindStartupImage(entries, "folder.img", directory), -1);
	CHECK_EQ(FindStartupImage(entries, "a.bin", directory), -1);
	entries.resize(3);
	CHECK_EQ(FindStartupImage(entries, "", directory), -1);
	entries.clear();
	CHECK_EQ(FindStartupImage(entries, "", directory), -1);
	TEST_CASE("image extensions must match completely");
	CHECK_EQ(IsCMDHDImageName("hd.img.old"), false);
	CHECK_EQ(IsCMDHDImageName("cmdhd-bootrom.bin"), false);
	CHECK_EQ(IsCMDHDImageName("."), false);
	CHECK_EQ(IsCMDHDImageName("hd.d"), false);
	CHECK_EQ(IsCMDHDImageName("hd.dhd"), true);
}
