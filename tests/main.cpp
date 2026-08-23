#include "harness.h"

void RunScsiCacheTests();
void RunScsiPhysicalReadTests();

int main()
{
	RunScsiCacheTests();
	RunScsiPhysicalReadTests();
	return Harness::Summary();
}
