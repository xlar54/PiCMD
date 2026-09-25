#include "harness.h"
#include "m6522.h"

void RunViaTests()
{
	static m6522 via;
	Interrupt irq;
	via.ConnectIRQ(&irq);
	via.Reset();
	TEST_CASE("idle VIA wakes for timers and clears pending IRQ on reset");
	for (int i = 0; i < 100; ++i) via.Execute();
	CHECK_EQ(via.Read(13) & 0x7f, 0);
	via.Write(14, 0xc0); // enable timer 1 IRQ
	via.Write(4, 3); via.Write(5, 0);
	for (int i = 0; i < 8; ++i) via.Execute();
	CHECK(via.Read(13) & 0x40);
	CHECK(irq.IsAsserted());
	via.Reset();
	for (int i = 0; i < 100; ++i) via.Execute();
	CHECK_EQ(via.Read(13) & 0x7f, 0);
	CHECK(!irq.IsAsserted());
	via.Write(8, 3); via.Write(9, 0);
	for (int i = 0; i < 8; ++i) via.Execute();
	CHECK(via.Read(13) & 0x20);

	TEST_CASE("external shift input works after idle with guarded pin updates");
	via.Reset();
	for (int i = 0; i < 10; ++i)
	{
		via.InputCB1Level((i & 1) != 0);
		via.Execute();
	}
	via.Write(11, 0x0c); // shift in under external clock
	via.Write(10, 0);
	unsigned char byte = 0xa5;
	for (int bit = 7; bit >= 0; --bit)
	{
		via.InputCB1Level(true); via.Execute();
		via.InputCB2Level((byte & (1 << bit)) != 0);
		via.InputCB2Level((byte & (1 << bit)) != 0); // same level is harmless
		via.InputCB1Level(false); via.Execute();
	}
	CHECK(via.Read(13) & 4);
	CHECK_EQ(via.Read(10), byte);
}
