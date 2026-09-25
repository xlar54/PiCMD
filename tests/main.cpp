#include "harness.h"

void RunScsiCacheTests();
void RunScsiPhysicalReadTests();

void RunViaTests();

int main()
{
	RunViaTests();
	RunScsiCacheTests();
	RunScsiPhysicalReadTests();
	return Harness::Summary();
}
