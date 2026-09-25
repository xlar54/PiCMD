#include "harness.h"

void RunScsiCacheTests();
void RunScsiPhysicalReadTests();

void RunViaTests();
void RunIecTests();
void RunOptionsTests();

int main()
{
	RunViaTests();
	RunIecTests();
	RunOptionsTests();
	RunScsiCacheTests();
	RunScsiPhysicalReadTests();
	return Harness::Summary();
}
