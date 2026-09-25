#include "harness.h"
#include "iec_bus.h"
#include "shims/fakegpio.h"

void RunIecTests()
{
	static m6522 via;
	via.Reset();
	IEC_Bus::SetSplitIECLines(true);
	IEC_Bus::SetInvertIECInputs(false);
	IEC_Bus::SetIgnoreReset(false);
	IEC_Bus::SetAtnOutGPIO(0);
	IEC_Bus::VIA = &via;
	IEC_Bus::port = via.GetPortB();
	via.GetPortB()->SetPortOut(0, IEC_Bus::PortB_OnPortOut);
	via.GetPortB()->SetDirection(0x1a);
	via.GetPortB()->SetOutput(0);
	FakeGPIO::levels = ~0u;
	IEC_Bus::ReadEmulationModeCMDHD();

	TEST_CASE("trimmed sample updates port inputs without resampling fast-serial DATA/SRQ");
	FakeGPIO::levels &= ~((1u << PIGPIO_IN_DATA) | (1u << PIGPIO_IN_CLOCK) | (1u << PIGPIO_IN_SRQ));
	IEC_Bus::ReadBusInputsCMDHD();
	CHECK_EQ(via.GetPortB()->GetInput() & 5, 5);
	CHECK(!IEC_Bus::GetPI_Data());
	CHECK(!IEC_Bus::GetPI_SRQ());

	TEST_CASE("released DATA remains guarded until the next full sample");
	via.GetPortB()->SetOutput(2);
	IEC_Bus::ReadEmulationModeCMDHD();
	via.GetPortB()->SetOutput(0);
	FakeGPIO::levels = ~0u;
	IEC_Bus::ReadBusInputsCMDHD();
	CHECK(via.GetPortB()->GetInput() & 1);
	IEC_Bus::ReadEmulationModeCMDHD();
	CHECK_EQ(via.GetPortB()->GetInput() & 1, 0);

	TEST_CASE("reset sampling stays live without an attached VIA");
	IEC_Bus::VIA = 0; IEC_Bus::port = 0;
	FakeGPIO::levels &= ~(1u << PIGPIO_IN_RESET);
	IEC_Bus::ReadEmulationModeCMDHD(); CHECK(IEC_Bus::IsReset());
	FakeGPIO::levels = ~0u;
	IEC_Bus::ReadEmulationModeCMDHD(); CHECK(!IEC_Bus::IsReset());
	IEC_Bus::VIA = &via; IEC_Bus::port = via.GetPortB();

	TEST_CASE("fast output experiment orders DATA before SRQ for both polarities");
	for (int invert = 0; invert < 2; ++invert)
	{
		IEC_Bus::SetInvertIECOutputs(invert != 0);
		IEC_Bus::SetFastSerialOutputs(false, false);
		FakeGPIO::writes.clear();
		IEC_Bus::SetFastSerialOutputs(true, true);
#if PICMD_FAST_SERIAL_PUSH
		CHECK_EQ(FakeGPIO::writes.size(), 3);
		unsigned dataReg = invert ? ARM_GPIO_GPSET0 : ARM_GPIO_GPCLR0;
		CHECK_EQ(FakeGPIO::writes[0].address, dataReg);
		CHECK(FakeGPIO::writes[0].value & (1u << PIGPIO_OUT_DATA));
		CHECK_EQ(FakeGPIO::writes.back().address, dataReg);
		CHECK_EQ(FakeGPIO::writes.back().value, 1u << PIGPIO_OUT_SRQ);
		IEC_Bus::SetFastSerialOutputs(false, false); // next 2 MHz cycle
		CHECK_EQ(FakeGPIO::writes.size(), 6);
		CHECK_EQ(FakeGPIO::writes.back().address, invert ? ARM_GPIO_GPCLR0 : ARM_GPIO_GPSET0);
		CHECK_EQ(FakeGPIO::writes.back().value, 1u << PIGPIO_OUT_SRQ);
		// A slow-port write changed the shared DATA flag during Step(). The
		// subsequent fast output must restore the pin even if SRQ is unchanged.
		via.GetPortB()->SetOutput(2);
		FakeGPIO::writes.clear();
		IEC_Bus::SetFastSerialOutputs(false, false);
		CHECK(!FakeGPIO::writes.empty());
#else
		CHECK(FakeGPIO::writes.empty());
#endif
	}
	IEC_Bus::VIA = 0; IEC_Bus::port = 0;
}
