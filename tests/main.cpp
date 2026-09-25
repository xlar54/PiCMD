#include "harness.h"

void RunScsiCacheTests();
void RunScsiPhysicalReadTests();

void RunViaTests();
void RunOptionsTests();

int main()
{
	RunViaTests();
	RunOptionsTests();
	RunScsiCacheTests();
	RunScsiPhysicalReadTests();
	return Harness::Summary();
}
