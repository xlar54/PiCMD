#include "harness.h"
#include "options.h"

void RunOptionsTests()
{
	TEST_CASE("eject logging requires an explicit option");
	Options options;
	CHECK_EQ(options.GetCMDHDEjectLog(), 0);
	char enable[] = "CMDHDEjectLog = 1\n";
	options.Process(enable);
	CHECK_EQ(options.GetCMDHDEjectLog(), 1);
	char disable[] = "CMDHDEjectLog = 0\n";
	options.Process(disable);
	CHECK_EQ(options.GetCMDHDEjectLog(), 0);
	char invalid[] = "CMDHDEjectLog = typo\n";
	options.Process(invalid);
	CHECK_EQ(options.GetCMDHDEjectLog(), 0);
}
