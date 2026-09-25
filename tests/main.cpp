#include "harness.h"

void RunScsiCacheTests();
void RunScsiPhysicalReadTests();

void RunViaTests();
void RunIecTests();
void RunOptionsTests();
void RunStartupImageTests();

int main()
{
	RunViaTests();
	RunIecTests();
	RunOptionsTests();
	RunStartupImageTests();
	RunScsiCacheTests();
	RunScsiPhysicalReadTests();
	return Harness::Summary();
}
