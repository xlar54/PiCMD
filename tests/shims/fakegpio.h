#ifndef TESTS_FAKEGPIO_H
#define TESTS_FAKEGPIO_H
#include <vector>
namespace FakeGPIO {
struct Access { unsigned address, value; };
extern unsigned levels;
extern std::vector<Access> writes;
unsigned Read(unsigned address);
void Write(unsigned address, unsigned value);
}
#endif
