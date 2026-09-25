#include "fakegpio.h"
#include "rpihardware.h"
namespace FakeGPIO {
unsigned levels = ~0u;
std::vector<Access> writes;
unsigned Read(unsigned address) { return address == ARM_GPIO_GPLEV0 ? levels : 0; }
void Write(unsigned address, unsigned value) { writes.push_back({address, value}); }
}
