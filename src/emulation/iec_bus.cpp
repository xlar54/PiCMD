// Pi1541 - A Commodore 1541 disk drive emulator
// Copyright(C) 2018 Stephen White
//
// This file is part of Pi1541.
// 
// Pi1541 is free software : you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// 
// Pi1541 is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with Pi1541. If not, see <http://www.gnu.org/licenses/>.

#include "iec_bus.h"
#include "inputmappings.h"

//#define REAL_XOR 1

int IEC_Bus::buttonCount = sizeof(ButtonPinFlags) / sizeof(unsigned);

u32 IEC_Bus::oldClears = 0;
u32 IEC_Bus::oldSets = 0;
u32 IEC_Bus::PIGPIO_MASK_IN_ATN = 1 << PIGPIO_ATN;
u32 IEC_Bus::PIGPIO_MASK_IN_DATA = 1 << PIGPIO_DATA;
u32 IEC_Bus::PIGPIO_MASK_IN_CLOCK = 1 << PIGPIO_CLOCK;
u32 IEC_Bus::PIGPIO_MASK_IN_SRQ = 1 << PIGPIO_SRQ;
u32 IEC_Bus::PIGPIO_MASK_IN_RESET = 1 << PIGPIO_RESET;

bool IEC_Bus::PI_Atn = false;
bool IEC_Bus::PI_Data = false;
bool IEC_Bus::PI_Clock = false;
bool IEC_Bus::PI_SRQ = false;
bool IEC_Bus::PI_Reset = false;

bool IEC_Bus::VIA_Atna = false;
bool IEC_Bus::VIA_Data = false;
bool IEC_Bus::VIA_Clock = false;

bool IEC_Bus::DataSetToOut = false;
bool IEC_Bus::AtnaDataSetToOut = false;
bool IEC_Bus::sampledDataSetToOut = false;
bool IEC_Bus::sampledAtnaDataSetToOut = false;
bool IEC_Bus::sampledClockSetToOut = false;
bool IEC_Bus::ClockSetToOut = false;
bool IEC_Bus::SRQSetToOut = false;
bool IEC_Bus::AtnSetToOut = false;
u32 IEC_Bus::atnOutGPIO = 0;

m6522* IEC_Bus::VIA = 0;
IOPort* IEC_Bus::port = 0;

bool IEC_Bus::OutputLED = false;
bool IEC_Bus::OutputSound = false;

bool IEC_Bus::Resetting = false;

bool IEC_Bus::splitIECLines = false;
bool IEC_Bus::invertIECInputs = false;
bool IEC_Bus::invertIECOutputs = true;
bool IEC_Bus::ignoreReset = false;

u32 IEC_Bus::myOutsGPFSEL1 = 0;
u32 IEC_Bus::myOutsGPFSEL0 = 0;
bool IEC_Bus::InputButton[5] = { 0 };
bool IEC_Bus::InputButtonPrev[5] = { 0 };
u32 IEC_Bus::validInputCount[5] = { 0 };
u32 IEC_Bus::inputRepeatThreshold[5];
u32 IEC_Bus::inputRepeat[5] = { 0 };
u32 IEC_Bus::inputRepeatPrev[5] = { 0 };


u32 IEC_Bus::emulationModeCheckButtonIndex = 0;

unsigned IEC_Bus::gplev0;

void IEC_Bus::ReadGPIOUserInput(unsigned step)
{
	for (int index = 0; index < buttonCount; ++index)
	{
		UpdateButton(index, gplev0, step);
	}
}


void IEC_Bus::ReadBrowseMode(void)
{
	gplev0 = read32(ARM_GPIO_GPLEV0);
	ReadGPIOUserInput();

	bool ATNIn = (gplev0 & PIGPIO_MASK_IN_ATN) == (invertIECInputs ? PIGPIO_MASK_IN_ATN : 0);
	if (PI_Atn != ATNIn)
	{
		PI_Atn = ATNIn;
	}

	if (!AtnaDataSetToOut && !DataSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool DATAIn = (gplev0 & PIGPIO_MASK_IN_DATA) == (invertIECInputs ? PIGPIO_MASK_IN_DATA : 0);
		if (PI_Data != DATAIn)
		{
			PI_Data = DATAIn;
		}
	}
	else
	{
		PI_Data = true;
	}

	if (!ClockSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool CLOCKIn = (gplev0 & PIGPIO_MASK_IN_CLOCK) == (invertIECInputs ? PIGPIO_MASK_IN_CLOCK  : 0);
		if (PI_Clock != CLOCKIn)
		{
			PI_Clock = CLOCKIn;
		}
	}
	else
	{
		PI_Clock = true;
	}

	Resetting = !ignoreReset && ((gplev0 & PIGPIO_MASK_IN_RESET) == (invertIECInputs ? PIGPIO_MASK_IN_RESET : 0));
}

// The CMD HD's U10 VIA is wired to the IEC bus like a 1541's $1800 VIA, with
// the addition of SRQ for fast serial. The one difference that matters is the
// ATN acknowledge gate: the CMD HD ANDs ATNA with ATN (like the 1581) rather
// than XORing them (like the 1541).
// A second, cheap look at the bus, taken between the two emulated CPU cycles.
//
// Why a trimmed one rather than another ReadEmulationModeCMDHD: that costs 156
// cycles and does not fit - it was tried in fe3cdc2 and took the loop to 69
// overruns per thousand. Of those 156, the blocking read of GPLEV0 is 69,
// measured (PICMD-LST27.LOG, "read cost: gplev 69"), so most of the rest is
// decoding that the second look does not need.
//
// What it deliberately does NOT do:
//   - it does not touch PI_Data or PI_Clock. PI_Data feeds via10's CB2 in
//     PiCMDHD::Update, and cb2 is the data bit of the fast serial shift
//     register; moving it half a microsecond relative to the CB1 shift clock
//     could change which side of an SRQ edge a bit lands on. That would
//     manufacture corruption in the exact mechanism under investigation.
//     The routines that need serving read ORB, which is portB's input latch.
//   - it does not touch ATN, which is edge triggered into CA1 and whose
//     acknowledge already runs on the full sample.
//   - it does not write IEC_Bus::gplev0, which the front panel sampler reads
//     once every 256 loops and which must keep meaning "the level at the top
//     of the loop".
//
// The guards match the full sampler exactly: a line the drive is pulling low
// itself cannot be sensed, because the pin is an output.
void IEC_Bus::ReadBusInputsCMDHD(void)
{
	unsigned lev = read32(ARM_GPIO_GPLEV0);

	IOPort* portB = port;
	if (!portB)
		return;

	// The guards are a SNAPSHOT taken at the top of the loop, not the live
	// flags, and that is the whole point.
	//
	// If the emulated CPU releases DATA or CLOCK during the first emulated
	// cycle, PortB_OnPortOut puts that on the wire immediately. A live guard
	// would be open by the time this runs a few hundred nanoseconds later, so
	// we would sample a line that has not finished rising - open collector,
	// pull-up, cable capacitance - and hand the drive back its own pull as if
	// the computer were still asserting it. On the hardware that looks exactly
	// like the fault this change exists to fix: a first symbol late or with a
	// bit stuck low, and WORSE with the fix than without it.
	//
	// With the snapshot, a line released mid-loop is not sensed until the next
	// top-of-loop sample, which is the timing the baseline has always had.
#ifndef REAL_XOR
	if (!sampledAtnaDataSetToOut && !sampledDataSetToOut)
#else
	if (!sampledDataSetToOut)
#endif
		portB->SetInput(VIAPORTPINS_DATAIN,
			(lev & PIGPIO_MASK_IN_DATA) == (invertIECInputs ? PIGPIO_MASK_IN_DATA : 0));

	if (!sampledClockSetToOut)
		portB->SetInput(VIAPORTPINS_CLOCKIN,
			(lev & PIGPIO_MASK_IN_CLOCK) == (invertIECInputs ? PIGPIO_MASK_IN_CLOCK : 0));
}

void IEC_Bus::ReadEmulationModeCMDHD(void)
{
	gplev0 = read32(ARM_GPIO_GPLEV0);

	IOPort* portB = port;

	// Nearly every line below dereferences these, and the one null check that
	// was here covered a single case out of five. Both are set together before
	// emulation starts, so this only matters to a future caller that gets the
	// order wrong - but then it matters as a hang, not a wrong bus level.
	if (!portB || !VIA)
		return;

#ifndef REAL_XOR
	// ATN is an input unless the drive itself is driving it (pb6), in which
	// case the line is low and must be read back as asserted.
	bool ATNIn = AtnSetToOut ||
		((gplev0 & PIGPIO_MASK_IN_ATN) == (invertIECInputs ? PIGPIO_MASK_IN_ATN : 0));
	if (PI_Atn != ATNIn)
	{
		PI_Atn = ATNIn;

		//DEBUG_LOG("A%d\r\n", PI_Atn);
		//if (port)
		{
			if ((portB->GetDirection() & 0x10) != 0)
			{
				// Emulate the CMD HD's ATN acknowledge gate.
				// Unlike the 1541 (which XORs ATNA with ATN), the CMD HD - like
				// the 1581 - ANDs them: HDOS idles with ATNA asserted so that an
				// incoming ATN automatically pulls DATA low, then clears ATNA to
				// release it. Using the 1541's XOR here holds DATA low forever.
				AtnaDataSetToOut = (VIA_Atna & PI_Atn);
			}

			portB->SetInput(VIAPORTPINS_ATNIN, ATNIn);	//is inverted and then connected to pb7
			// CA1 has the opposite polarity to the 1541: it FALLS when ATN
			// asserts (VICE: VIA_SIG_FALL on assert, VIA_SIG_RISE on release).
			// The boot ROM and HDOS program PCR=0 (negative edge) and rely on
			// this for their ATN service interrupt.
			VIA->InputCA1(!ATNIn);
		}
	}

	if ((portB->GetDirection() & 0x10) == 0)
		AtnaDataSetToOut = false; // If the ATNA PB4 gets set to an input then we can't be pulling data low. (Maniac Mansion does this)

	// moved from PortB_OnPortOut
	if (AtnaDataSetToOut)
		portB->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software

	if (!AtnaDataSetToOut && !DataSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool DATAIn = (gplev0 & PIGPIO_MASK_IN_DATA) == (invertIECInputs ? PIGPIO_MASK_IN_DATA : 0);
		//if (PI_Data != DATAIn)
		{
			PI_Data = DATAIn;
			portB->SetInput(VIAPORTPINS_DATAIN, DATAIn);	// VIA DATAin pb0 output from inverted DIN 5 DATA
		}
	}
	else
	{
		PI_Data = true;
		portB->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software
	}
#else
	bool ATNIn = (gplev0 & PIGPIO_MASK_IN_ATN) == (invertIECInputs ? PIGPIO_MASK_IN_ATN : 0);
	if (PI_Atn != ATNIn)
	{
		PI_Atn = ATNIn;

		{
			portB->SetInput(VIAPORTPINS_ATNIN, ATNIn);	//is inverted and then connected to pb7 and ca1
			VIA->InputCA1(ATNIn);
		}
	}

	if (!DataSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool DATAIn = (gplev0 & PIGPIO_MASK_IN_DATA) == (invertIECInputs ? PIGPIO_MASK_IN_DATA : 0);
		//if (PI_Data != DATAIn)
		{
			PI_Data = DATAIn;
			portB->SetInput(VIAPORTPINS_DATAIN, DATAIn);	// VIA DATAin pb0 output from inverted DIN 5 DATA
		}
	}
	else
	{
		PI_Data = true;
		portB->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software
	}

#endif
	if (!ClockSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool CLOCKIn = (gplev0 & PIGPIO_MASK_IN_CLOCK) == (invertIECInputs ? PIGPIO_MASK_IN_CLOCK : 0);
		//if (PI_Clock != CLOCKIn)
		{
			PI_Clock = CLOCKIn;
			portB->SetInput(VIAPORTPINS_CLOCKIN, CLOCKIn); // VIA CLKin pb2 output from inverted DIN 4 CLK
		}
	}
	else
	{
		PI_Clock = true;
		portB->SetInput(VIAPORTPINS_CLOCKIN, true); // simulate the read in software
	}

	if (!SRQSetToOut)	// only sense if we have not brought the line low (because we can't as we have the pin set to output but we can simulate in software)
	{
		bool SRQIn = (gplev0 & PIGPIO_MASK_IN_SRQ) == (invertIECInputs ? PIGPIO_MASK_IN_SRQ : 0);
		if (PI_SRQ != SRQIn)
		{
			PI_SRQ = SRQIn;
		}
	}
	else
	{
		PI_SRQ = true;
	}

	Resetting = !ignoreReset && ((gplev0 & PIGPIO_MASK_IN_RESET) == (invertIECInputs ? PIGPIO_MASK_IN_RESET : 0));

	// What ReadBusInputsCMDHD will use half a microsecond from now. See the
	// note there for why it must not read the live flags.
	sampledDataSetToOut = DataSetToOut;
	sampledAtnaDataSetToOut = AtnaDataSetToOut;
	sampledClockSetToOut = ClockSetToOut;
}

void IEC_Bus::PortB_OnPortOut(void* pUserData, unsigned char status)
{
	bool oldDataSetToOut = DataSetToOut;
	bool oldClockSetToOut = ClockSetToOut;
	bool AtnaDataSetToOutOld = AtnaDataSetToOut;

	// These are the values the VIA is trying to set the outputs to
	VIA_Atna = (status & (unsigned char)VIAPORTPINS_ATNAOUT) != 0;
	// pb6 is the CMD HD's ATN output and is ACTIVE LOW, unlike the pb1/pb3
	// data and clock outputs. It only counts while pb6 is configured as an
	// output; HDOS makes it one (and clears ATNA so it does not acknowledge
	// its own ATN) for about a millisecond while performing a SWAP.
	AtnSetToOut = port && (port->GetDirection() & (unsigned char)VIAPORTPINS_ATNOUT) &&
		((status & (unsigned char)VIAPORTPINS_ATNOUT) == 0);
	VIA_Data = (status & (unsigned char)VIAPORTPINS_DATAOUT) != 0;		// VIA DATAout PB1 inverted and then connected to DIN DATA
	VIA_Clock = (status & (unsigned char)VIAPORTPINS_CLOCKOUT) != 0;	// VIA CLKout PB3 inverted and then connected to DIN CLK

#ifndef REAL_XOR
	// The CMD HD ANDs ATNA with ATN (see ReadEmulationModeCMDHD). This is the
	// same for browse mode, so there is no longer a separate case here.
	AtnaDataSetToOut = (VIA_Atna & PI_Atn);
#else
	AtnaDataSetToOut = VIA_Atna;
#endif

	//if (AtnaDataSetToOut)
	//{
	//	// if the output of the XOR gate is high (ie VIA_Atna != PI_Atn) then this is inverted and pulls DATA low (activating it)
	//	//PI_Data = true;
	//	if (port) port->SetInput(VIAPORTPINS_DATAIN, true);	// simulate the read in software
	//}

	if (VIA && port)
	{
		// If the VIA's data and clock outputs ever get set to inputs the real hardware reads these lines as asserted.
		bool PB1SetToInput = (port->GetDirection() & 2) == 0;
		bool PB3SetToInput = (port->GetDirection() & 8) == 0;
		if (PB1SetToInput) VIA_Data = true;
		if (PB3SetToInput) VIA_Clock = true;
	}

	ClockSetToOut = VIA_Clock;
	DataSetToOut = VIA_Data;

	// Put the change on the wire now rather than waiting for the main loop's
	// once-per-microsecond refresh.
	//
	// The drive answers a strobe by writing this port and then counting NOPs:
	// its first transmitted symbol is due about 14 cycles - 7us - after the
	// poll that spotted the strobe, and the following three have 7.5 to 10us
	// of slack. Deferring the write to the end of the microsecond spends up to
	// 1us of that first budget for nothing, and the first symbol is where 404
	// of 422 observed bit flips land.
	//
	// It is close to free: these flags only move when the CPU writes the port,
	// which is four times per transmitted byte against roughly 35us of
	// transfer, so the common case does not reach here at all. Writes to
	// device memory are posted, so the store itself does not stall either.
	if (DataSetToOut != oldDataSetToOut ||
		ClockSetToOut != oldClockSetToOut ||
		AtnaDataSetToOut != AtnaDataSetToOutOld)
	{
		RefreshOutsCMDHD();
	}

	//if (!oldDataSetToOut && DataSetToOut)
	//{
	//	//PI_Data = true;
	//	if (port) port->SetInput(VIAPORTPINS_DATAOUT, true); // simulate the read in software
	//}

	//if (!oldClockSetToOut && ClockSetToOut)
	//{
	//	//PI_Clock = true;
	//	if (port) port->SetInput(VIAPORTPINS_CLOCKIN, true); // simulate the read in software
	//}

	//if (AtnaDataSetToOutOld ^ AtnaDataSetToOut)
}

void IEC_Bus::Reset(void)
{
	WaitUntilReset();

	// VIA $1800
	//	CA2, CB1 and CB2 are not connected (reads as high)
	// VIA $1C00
	//	CB1 not connected (reads as high)

	VIA_Atna = false;
	VIA_Data = false;
	VIA_Clock = false;

	DataSetToOut = false;
	ClockSetToOut = false;
	SRQSetToOut = false;
	AtnSetToOut = false;

	PI_Atn = false;
	PI_Data = false;
	PI_Clock = false;
	PI_SRQ = false;

#ifdef REAL_XOR
	AtnaDataSetToOut = VIA_Atna;
#else
	AtnaDataSetToOut = (VIA_Atna & PI_Atn);

	if (AtnaDataSetToOut) PI_Data = true;
#endif

	RefreshOutsCMDHD();
}

