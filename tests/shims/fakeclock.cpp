#include "fakeclock.h"

namespace FakeClock
{
	unsigned int micros = 0;
	unsigned int microsPerAccess = 0;

	void Reset()
	{
		micros = 0;
		microsPerAccess = 0;
	}

	void Advance(unsigned int us)
	{
		micros += us;
	}
}
