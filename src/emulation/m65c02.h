// Pi-CMD - A Commodore CMD-HD hard drive emulator
// Based on Pi1541 Copyright(C) 2018 Stephen White
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

/////////////////////////////////////////////////////////////////////////////////
// Emulates a Rockwell R65C02 (as used in the CMD HD) to a cycle accurate level.
//
// This core follows the same cycle-stepped design as Pi1541's NMOS M6502 core
// (see m6502.h). Each instruction is broken up into its correct sequence of
// bus cycles, called one after another via member function pointers.
//
// CMOS differences implemented here;-
//  - All undefined opcodes execute as NOPs with well defined byte/cycle counts.
//  - New instructions: BRA, PHX/PHY/PLX/PLY, STZ, TRB/TSB, INC A/DEC A,
//    BIT #imm/zp,x/abs,x, JMP (abs,x) and the (zp) addressing mode.
//  - Rockwell bit instructions: RMB0-7, SMB0-7, BBR0-7, BBS0-7.
//  - JMP (abs) takes 6 cycles and the NMOS page wrap bug is fixed.
//  - Read/Modify/Write instructions perform a read-read-write sequence
//    (NMOS performs read-write-write).
//  - ASL/LSR/ROL/ROR abs,x take 6 cycles when no page boundary is crossed.
//  - Decimal mode ADC/SBC take one extra cycle and set N,Z,C,V correctly.
//  - The D flag is cleared by RESET, IRQ and BRK.
//
// To use, supply bus read and write functions just like the M6502 core.

#ifndef M65C02_H
#define M65C02_H
#include "types.h"

typedef u8(*C02DataBusReadFn)(u16 address);
typedef void(*C02DataBusWriteFn)(u16 address, const u8 value);

class Interrupt
{
public:
	Interrupt() : asserted(false) { }
	inline bool IsAsserted() { return asserted; }
	inline void Assert() { asserted = true; }
	inline void Release() { asserted = false; }
	inline void Reset() { Release(); }
private:
	bool asserted;
};

//2, 3 or 4 cycles
#define BRANCH_CONDITION_C02(flag, condition)		\
	ra = dataBusReadFn(pc++);						\
	if (ra & 0x80) ra |= 0xFF00;					\
	if ((status & flag) == condition)				\
	{												\
		oldpc = pc;									\
		pc = (pc & 0xff00) | ((pc + ra) & 0xff);	\
		addressModeCycleFn = &M65C02::rel_T2;		\
	}												\
	else addressModeCycleFn = &M65C02::InstructionFetch;

class M65C02
{
private:
	enum
	{
		FLAG_CARRY = 0x01,
		FLAG_ZERO = 0x02,
		FLAG_INTERRUPT = 0x04,
		FLAG_DECIMAL = 0x08,
		FLAG_BREAK = 0x10,
		FLAG_CONSTANT = 0x20,
		FLAG_OVERFLOW = 0x40,
		FLAG_SIGN = 0x80
	};

	typedef void (M65C02::*AddressModeCycleFunction)(void);	// Member function pointers for the starting cycle of the address mode functions.
	static AddressModeCycleFunction T1AddressModeFunctions[256];
	typedef void (M65C02::*OpcodeCycleFunction)(void);		// Member function pointers for the opcodes.
	static OpcodeCycleFunction opcodeFunctions[256];

	union
	{
		u16 ea;		// Effective address
		u16 ra;		// Relative address
	};
	union
	{
		u16 ia;		// Intermediate address
		u16 oldpc;	// A branch's old PC
	};

	u16 value;		// Intermediate data value
	u16 pc;			// Program Counter
	u8 opcode;		// The current Opcode
	u8 a, x, y, status, sp; // Registers

	u8 CLIMaskingInterrupt : 1;
	u8 BranchTakenMaskingInterrupt : 1;
	u8 IRQPending : 1;
	u8 decimalExtraCycle : 1;	// CMOS decimal mode ADC/SBC take an extra cycle.

	C02DataBusReadFn dataBusReadFn;		// A pointer to the externally supplied Data Bus read function.
	C02DataBusWriteFn dataBusWriteFn;	// A pointer to the externally supplied Data Bus write function.

	AddressModeCycleFunction addressModeCycleFn;	// The function that will process the current address mode functionality for the current cycle.
	OpcodeCycleFunction opcodeCycleFn;				// The function that will be called after (or during) the address mode cycle(s) that execute the actual opcode.

	// Helper function to call opcodeCycleFn and set up for the next instruction fetch.
	// CMOS decimal mode ADC/SBC insert one extra internal cycle after the opcode executes.
	inline void ExecuteOpcode(void)
	{
		(this->*M65C02::opcodeCycleFn)();
		if (decimalExtraCycle)
		{
			decimalExtraCycle = 0;
			addressModeCycleFn = &M65C02::decimal_T;
		}
		else
		{
			addressModeCycleFn = &M65C02::InstructionFetch;
		}
	}

	// Stack manipulation helpers.
	inline void Push(u8 val) { dataBusWriteFn(0x100 + sp--, val); }
	inline u8 Pull(void) { return (dataBusReadFn(0x100 + ++sp)); }

	// Helper function to write back the results of an instruction (to memory or the A register).
	inline void WriteValue(u8 byte)
	{
		if (addressModeCycleFn == &M65C02::sb_T1) a = byte;
		else dataBusWriteFn(ea, byte);
	}

	void InstructionFetch();	// T0 of every address mode (except reset).

	// Opcode functions.
	void ADC(void);
	void AND(void) { u16 result = a & value; EstablishNZ(result); a = (u8)result; }
	void ASL(void) { u16 result = value << 1; EstablishC(result); EstablishNZ(result); WriteValue((u8)result); }
	void BCC(void) { BRANCH_CONDITION_C02(FLAG_CARRY, 0); }
	void BCS(void) { BRANCH_CONDITION_C02(FLAG_CARRY, FLAG_CARRY); }
	void BEQ(void) { BRANCH_CONDITION_C02(FLAG_ZERO, FLAG_ZERO); }
	void BIT(void) { u16 result = a & value; EstablishZ(result); SetV(value & 0x40); EstablishN(value); }
	void BITimm(void) { u16 result = a & value; EstablishZ(result); }	// BIT #imm only affects Z
	void BMI(void) { BRANCH_CONDITION_C02(FLAG_SIGN, FLAG_SIGN); }
	void BNE(void) { BRANCH_CONDITION_C02(FLAG_ZERO, 0); }
	void BPL(void) { BRANCH_CONDITION_C02(FLAG_SIGN, 0); }
	void BRA(void) { BRANCH_CONDITION_C02(0, 0); }	// Always taken
	void BVC(void) { BRANCH_CONDITION_C02(FLAG_OVERFLOW, 0); }
	void BVS(void) { BRANCH_CONDITION_C02(FLAG_OVERFLOW, FLAG_OVERFLOW); }
	void BRK(void) {}
	void CLC(void) { ClearC(); }
	void CLD(void) { ClearD(); }
	void CLI(void) { ClearI(); CLIMaskingInterrupt = true; }
	void CLV(void) { ClearV(); }
	void CMP(void) { u16 result = a - value; SetC(a >= (u8)value); SetZ(a == (u8)value); EstablishN(result); }
	void CPX(void) { u16 result = x - value; SetC(x >= (u8)value); SetZ(x == (u8)value); EstablishN(result); }
	void CPY(void) { u16 result = y - value; SetC(y >= (u8)value); SetZ(y == (u8)value); EstablishN(result); }
	void DEC(void) { u16 result = value - 1; EstablishNZ(result); WriteValue((u8)result); }
	void DEX(void) { x--; EstablishNZ(x); }
	void DEY(void) { y--; EstablishNZ(y); }
	void EOR(void) { u16 result = a ^ value; EstablishNZ(result); a = (u8)result; }
	void INC(void) { u16 result = value + 1; EstablishNZ(result); WriteValue((u8)result); }
	void INX(void) { x++; EstablishNZ(x); }
	void INY(void) { y++; EstablishNZ(y); }
	void JMP(void) { pc = ea; }
	void JSR(void) {}
	void LDA(void) { a = (u8)value; EstablishNZ(a); }
	void LDX(void) { x = (u8)value; EstablishNZ(x); }
	void LDY(void) { y = (u8)value; EstablishNZ(y); }
	void LSR(void) { u16 result = value >> 1; SetC(value & 1); EstablishNZ(result); WriteValue((u8)result); }
	void NOP(void) {}
	void ORA(void) { u16 result = a | value; EstablishNZ(result); a = (u8)result; }
	void PHA(void) { Push(a); }
	void PHP(void) { Push(status | FLAG_CONSTANT | FLAG_BREAK); }
	void PHX(void) { Push(x); }
	void PHY(void) { Push(y); }
	void PLA(void) { a = Pull(); EstablishNZ(a); }
	void PLP(void) { status = Pull() | FLAG_CONSTANT; }
	void PLX(void) { x = Pull(); EstablishNZ(x); }
	void PLY(void) { y = Pull(); EstablishNZ(y); }
	void ROL(void) { u16 result = (value << 1) | (status & FLAG_CARRY); EstablishC(result); EstablishNZ(result); WriteValue((u8)result); }
	void ROR(void) { u16 result = (value >> 1) | ((status & FLAG_CARRY) << 7); SetC(value & 1); EstablishNZ(result); WriteValue((u8)result); }
	void RTI(void) {}
	void RTS(void) {}
	void SBC(void);
	void SEC(void) { SetC(); }
	void SED(void) { SetD(); }
	void SEI(void) { SetI(); }
	void STA(void) { WriteValue(a); }
	void STX(void) { WriteValue(x); }
	void STY(void) { WriteValue(y); }
	void STZ(void) { WriteValue(0); }
	void TAX(void) { x = a; EstablishNZ(a); }
	void TAY(void) { y = a; EstablishNZ(y); }
	void TRB(void) { EstablishZ(a & value); WriteValue((u8)(value & ~a)); }
	void TSB(void) { EstablishZ(a & value); WriteValue((u8)(value | a)); }
	void TSX(void) { x = sp; EstablishNZ(x); }
	void TXA(void) { a = x; EstablishNZ(a); }
	void TXS(void) { sp = x; }
	void TYA(void) { a = y; EstablishNZ(a); }

	// Rockwell bit instructions. The bit index is encoded in the opcode's high nibble.
	void RMB(void) { WriteValue((u8)(value & ~(1 << ((opcode >> 4) & 7)))); }
	void SMB(void) { WriteValue((u8)(value | (1 << ((opcode >> 4) & 7)))); }
	void BBR(void)	// value holds the zero page byte, branch if bit clear
	{
		u16 mask = 1 << ((opcode >> 4) & 7);
		ra = dataBusReadFn(pc++);
		if (ra & 0x80) ra |= 0xFF00;
		if ((value & mask) == 0)
		{
			oldpc = pc;
			pc = (pc & 0xff00) | ((pc + ra) & 0xff);
			addressModeCycleFn = &M65C02::rel_T2;
		}
		else addressModeCycleFn = &M65C02::InstructionFetch;
	}
	void BBS(void)	// branch if bit set
	{
		u16 mask = 1 << ((opcode >> 4) & 7);
		ra = dataBusReadFn(pc++);
		if (ra & 0x80) ra |= 0xFF00;
		if ((value & mask) != 0)
		{
			oldpc = pc;
			pc = (pc & 0xff00) | ((pc + ra) & 0xff);
			addressModeCycleFn = &M65C02::rel_T2;
		}
		else addressModeCycleFn = &M65C02::InstructionFetch;
	}

	// Address modes.

	// Extra internal cycle for CMOS decimal mode ADC/SBC.
	void decimal_T(void) { dataBusReadFn(pc); addressModeCycleFn = &M65C02::InstructionFetch; }

	// Single byte instructions. 2 cycles.
	void sb_T1(void) { dataBusReadFn(pc); value = a; ExecuteOpcode(); }

	// Single cycle NOPs (all CMOS undefined opcodes in columns 3, 7, B and F execute in 1 cycle).
	// The T0 fetch is the entire instruction so T1 is simply the next instruction's fetch.
	// (Columns 7 and F are the Rockwell bit instructions on a R65C02 so only 3 and B here.)

	void imm_T1(void) { value = dataBusReadFn(pc++); ExecuteOpcode(); } //2 cycles

	// Branches; the opcode executes in T1 just like the NMOS core.
	void rel_T1(void) { (this->*M65C02::opcodeCycleFn)(); }
	void rel_T2(void);
	void rel_T3(void) { dataBusReadFn(pc); addressModeCycleFn = &M65C02::InstructionFetch; }

	// Zero page read. 3 cycles.
	void zp_r_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zp_r_T2; }
	void zp_r_T2(void) { value = dataBusReadFn(ea); ExecuteOpcode(); }

	// Zero page write. 3 cycles.
	void zp_w_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zp_w_T2; }
	void zp_w_T2(void) { ExecuteOpcode(); }

	// Absolute read. 4 cycles.
	void abs_r_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::abs_r_T2; }
	void abs_r_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::abs_r_T3; }
	void abs_r_T3(void) { value = dataBusReadFn(ea); ExecuteOpcode(); }

	// Absolute write. 4 cycles.
	void abs_w_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::abs_w_T2; }
	void abs_w_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::abs_w_T3; }
	void abs_w_T3(void) { ExecuteOpcode(); }

	// (zp,x) read. 6 cycles.
	void idx_r_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::idx_r_T2; }
	void idx_r_T2(void) { dataBusReadFn(ia); addressModeCycleFn = &M65C02::idx_r_T3; }
	void idx_r_T3(void) { ia = (ia + x) & 0xff; ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::idx_r_T4; }
	void idx_r_T4(void) { ea |= (dataBusReadFn(ia & 0xff) << 8); addressModeCycleFn = &M65C02::idx_r_T5; }
	void idx_r_T5(void) { value = dataBusReadFn(ea); ExecuteOpcode(); }

	// (zp,x) write. 6 cycles.
	void idx_w_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::idx_w_T2; }
	void idx_w_T2(void) { dataBusReadFn(ia); addressModeCycleFn = &M65C02::idx_w_T3; }
	void idx_w_T3(void) { ia = (ia + x) & 0xff; ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::idx_w_T4; }
	void idx_w_T4(void) { ea |= (dataBusReadFn(ia & 0xff) << 8); addressModeCycleFn = &M65C02::idx_w_T5; }
	void idx_w_T5(void) { ExecuteOpcode(); }

	// (zp) read. 5 cycles.
	void izp_r_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::izp_r_T2; }
	void izp_r_T2(void) { ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::izp_r_T3; }
	void izp_r_T3(void) { ea |= (dataBusReadFn(ia & 0xff) << 8); addressModeCycleFn = &M65C02::izp_r_T4; }
	void izp_r_T4(void) { value = dataBusReadFn(ea); ExecuteOpcode(); }

	// (zp) write. 5 cycles.
	void izp_w_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::izp_w_T2; }
	void izp_w_T2(void) { ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::izp_w_T3; }
	void izp_w_T3(void) { ea |= (dataBusReadFn(ia & 0xff) << 8); addressModeCycleFn = &M65C02::izp_w_T4; }
	void izp_w_T4(void) { ExecuteOpcode(); }

	// abs,x read. 4 cycles (+1 if page crossed).
	// On the CMOS part the extra cycle re-reads the last byte of the instruction.
	void absx_r_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::absx_r_T2; }
	void absx_r_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::absx_r_T3; }
	void absx_r_T3(void);
	void absx_r_T4(void) { value = dataBusReadFn(ea); ExecuteOpcode(); }

	// abs,x write. 5 cycles.
	void absx_w_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::absx_w_T2; }
	void absx_w_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::absx_w_T3; }
	void absx_w_T3(void) { dataBusReadFn((u16)(pc - 1)); ea += x; addressModeCycleFn = &M65C02::absx_w_T4; }
	void absx_w_T4(void) { ExecuteOpcode(); }

	// abs,y read. 4 cycles (+1 if page crossed).
	void absy_r_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::absy_r_T2; }
	void absy_r_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::absy_r_T3; }
	void absy_r_T3(void);
	void absy_r_T4(void) { value = dataBusReadFn(ea); ExecuteOpcode(); }

	// abs,y write. 5 cycles.
	void absy_w_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::absy_w_T2; }
	void absy_w_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::absy_w_T3; }
	void absy_w_T3(void) { dataBusReadFn((u16)(pc - 1)); ea += y; addressModeCycleFn = &M65C02::absy_w_T4; }
	void absy_w_T4(void) { ExecuteOpcode(); }

	// zp,x read. 4 cycles.
	void zpx_r_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zpx_r_T2; }
	void zpx_r_T2(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpx_r_T3; }
	void zpx_r_T3(void) { ea = (ea + x) & 0xFF; value = dataBusReadFn(ea); ExecuteOpcode(); }

	// zp,x write. 4 cycles.
	void zpx_w_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zpx_w_T2; }
	void zpx_w_T2(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpx_w_T3; }
	void zpx_w_T3(void) { ea = (ea + x) & 0xFF; ExecuteOpcode(); }

	// zp,y read. 4 cycles.
	void zpy_r_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zpy_r_T2; }
	void zpy_r_T2(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpy_r_T3; }
	void zpy_r_T3(void) { ea = (ea + y) & 0xFF; value = dataBusReadFn(ea); ExecuteOpcode(); }

	// zp,y write. 4 cycles.
	void zpy_w_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zpy_w_T2; }
	void zpy_w_T2(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpy_w_T3; }
	void zpy_w_T3(void) { ea = (ea + y) & 0xFF; ExecuteOpcode(); }

	// (zp),y read. 5 cycles (+1 if page crossed).
	void idy_r_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::idy_r_T2; }
	void idy_r_T2(void) { ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::idy_r_T3; }
	void idy_r_T3(void) { ea |= (dataBusReadFn(ia & 0xff) << 8); addressModeCycleFn = &M65C02::idy_r_T4; }
	void idy_r_T4(void);
	void idy_r_T5(void) { value = dataBusReadFn(ea); ExecuteOpcode(); }

	// (zp),y write. 6 cycles.
	void idy_w_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::idy_w_T2; }
	void idy_w_T2(void) { ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::idy_w_T3; }
	void idy_w_T3(void) { ea |= (dataBusReadFn(ia & 0xff) << 8); addressModeCycleFn = &M65C02::idy_w_T4; }
	void idy_w_T4(void) { dataBusReadFn((u16)(pc - 1)); ea += y; addressModeCycleFn = &M65C02::idy_w_T5; }
	void idy_w_T5(void) { ExecuteOpcode(); }

	// Zero page RMW. 5 cycles. CMOS does read-read-write.
	void zp_rmw_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zp_rmw_T2; }
	void zp_rmw_T2(void) { value = dataBusReadFn(ea); addressModeCycleFn = &M65C02::zp_rmw_T3; }
	void zp_rmw_T3(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zp_rmw_T4; }
	void zp_rmw_T4(void) { ExecuteOpcode(); }

	// Absolute RMW. 6 cycles.
	void abs_rmw_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::abs_rmw_T2; }
	void abs_rmw_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::abs_rmw_T3; }
	void abs_rmw_T3(void) { value = dataBusReadFn(ea); addressModeCycleFn = &M65C02::abs_rmw_T4; }
	void abs_rmw_T4(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::abs_rmw_T5; }
	void abs_rmw_T5(void) { ExecuteOpcode(); }

	// Zero page,x RMW. 6 cycles.
	void zpx_rmw_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zpx_rmw_T2; }
	void zpx_rmw_T2(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpx_rmw_T3; }
	void zpx_rmw_T3(void) { ea = (ea + x) & 0xFF; value = dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpx_rmw_T4; }
	void zpx_rmw_T4(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpx_rmw_T5; }
	void zpx_rmw_T5(void) { ExecuteOpcode(); }

	// abs,x RMW for ASL/LSR/ROL/ROR. 6 cycles (+1 if page crossed).
	void absx_rmwc_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::absx_rmwc_T2; }
	void absx_rmwc_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::absx_rmwc_T3; }
	void absx_rmwc_T3(void);
	void absx_rmwc_T4(void) { value = dataBusReadFn(ea); addressModeCycleFn = &M65C02::absx_rmwc_T5; }
	void absx_rmwc_T5(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::absx_rmwc_T6; }
	void absx_rmwc_T6(void) { ExecuteOpcode(); }

	// abs,x RMW for INC/DEC. Always 7 cycles.
	void absx_rmw7_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::absx_rmw7_T2; }
	void absx_rmw7_T2(void) { ea |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::absx_rmw7_T3; }
	void absx_rmw7_T3(void) { dataBusReadFn((u16)(pc - 1)); ea += x; addressModeCycleFn = &M65C02::absx_rmw7_T4; }
	void absx_rmw7_T4(void) { value = dataBusReadFn(ea); addressModeCycleFn = &M65C02::absx_rmw7_T5; }
	void absx_rmw7_T5(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::absx_rmw7_T6; }
	void absx_rmw7_T6(void) { ExecuteOpcode(); }

	// Push instructions. 3 cycles.
	void ph_T1(void) { dataBusReadFn(pc); addressModeCycleFn = &M65C02::ph_T2; }
	void ph_T2(void) { ExecuteOpcode(); }

	// Pull instructions. 4 cycles.
	void pl_T1(void) { dataBusReadFn(pc); addressModeCycleFn = &M65C02::pl_T2; }
	void pl_T2(void) { dataBusReadFn(0x100 + sp); addressModeCycleFn = &M65C02::pl_T3; }
	void pl_T3(void) { ExecuteOpcode(); }

	// JSR. 6 cycles.
	void jsr_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::jsr_T2; }
	void jsr_T2(void) { dataBusReadFn(0x100 + sp); addressModeCycleFn = &M65C02::jsr_T3; }
	void jsr_T3(void) { Push((u8)((pc) >> 8)); addressModeCycleFn = &M65C02::jsr_T4; }
	void jsr_T4(void) { Push(pc & 0xff); addressModeCycleFn = &M65C02::jsr_T5; }
	void jsr_T5(void) { ea |= (dataBusReadFn(pc++) << 8); pc = ea; ExecuteOpcode(); }

	// RTI. 6 cycles.
	void rti_T1(void) { dataBusReadFn(pc++); addressModeCycleFn = &M65C02::rti_T2; }
	void rti_T2(void) { dataBusReadFn(0x100 + sp); addressModeCycleFn = &M65C02::rti_T3; }
	void rti_T3(void) { status = Pull() | FLAG_CONSTANT; addressModeCycleFn = &M65C02::rti_T4; }
	void rti_T4(void) { pc = Pull(); addressModeCycleFn = &M65C02::rti_T5; }
	void rti_T5(void) { pc |= (Pull() << 8); ExecuteOpcode(); }

	// RTS. 6 cycles.
	void rts_T1(void) { dataBusReadFn(pc++); addressModeCycleFn = &M65C02::rts_T2; }
	void rts_T2(void) { dataBusReadFn(0x100 + sp); addressModeCycleFn = &M65C02::rts_T3; }
	void rts_T3(void) { pc = Pull(); addressModeCycleFn = &M65C02::rts_T4; }
	void rts_T4(void) { pc |= (Pull() << 8); addressModeCycleFn = &M65C02::rts_T5; }
	void rts_T5(void) { dataBusReadFn(pc); pc++; ExecuteOpcode(); }

	// JMP abs. 3 cycles.
	void jmpabs_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::jmpabs_T2; }
	void jmpabs_T2(void) { ea |= (dataBusReadFn(pc++) << 8); ExecuteOpcode(); }

	// JMP (abs). 6 cycles on CMOS, page wrap bug fixed.
	void jmpind_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::jmpind_T2; }
	void jmpind_T2(void) { ia |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::jmpind_T3; }
	void jmpind_T3(void) { dataBusReadFn((u16)(pc - 1)); addressModeCycleFn = &M65C02::jmpind_T4; }
	void jmpind_T4(void) { ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::jmpind_T5; }
	void jmpind_T5(void) { ea |= (dataBusReadFn(ia) << 8); ExecuteOpcode(); }

	// JMP (abs,x). 6 cycles.
	void jmpiax_T1(void) { ia = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::jmpiax_T2; }
	void jmpiax_T2(void) { ia |= (dataBusReadFn(pc++) << 8); addressModeCycleFn = &M65C02::jmpiax_T3; }
	void jmpiax_T3(void) { dataBusReadFn((u16)(pc - 1)); ia += x; addressModeCycleFn = &M65C02::jmpiax_T4; }
	void jmpiax_T4(void) { ea = dataBusReadFn(ia++); addressModeCycleFn = &M65C02::jmpiax_T5; }
	void jmpiax_T5(void) { ea |= (dataBusReadFn(ia) << 8); ExecuteOpcode(); }

	// The odd $5C NOP. 3 bytes, 8 cycles. Reads $FF00|operand then $FFFF four times.
	void nop5c_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::nop5c_T2; }
	void nop5c_T2(void) { dataBusReadFn(pc++); addressModeCycleFn = &M65C02::nop5c_T3; }
	void nop5c_T3(void) { dataBusReadFn(0xFF00 | ea); addressModeCycleFn = &M65C02::nop5c_T4; }
	void nop5c_T4(void) { dataBusReadFn(0xFFFF); addressModeCycleFn = &M65C02::nop5c_T5; }
	void nop5c_T5(void) { dataBusReadFn(0xFFFF); addressModeCycleFn = &M65C02::nop5c_T6; }
	void nop5c_T6(void) { dataBusReadFn(0xFFFF); addressModeCycleFn = &M65C02::nop5c_T7; }
	void nop5c_T7(void) { dataBusReadFn(0xFFFF); ExecuteOpcode(); }

	// Rockwell BBR/BBS. 5 cycles (+1 taken, +1 page cross).
	void zpbit_T1(void) { ea = dataBusReadFn(pc++); addressModeCycleFn = &M65C02::zpbit_T2; }
	void zpbit_T2(void) { value = dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpbit_T3; }
	void zpbit_T3(void) { dataBusReadFn(ea); addressModeCycleFn = &M65C02::zpbit_T4; }
	void zpbit_T4(void) { (this->*M65C02::opcodeCycleFn)(); }	// BBR/BBS read the relative operand and behave like a branch.

	// BRK. 7 cycles. On the CMOS part BRK does not morph into an IRQ; it completes,
	// and the D flag is cleared by the sequence.
	void brk_T1(void) { dataBusReadFn(pc); pc++; addressModeCycleFn = &M65C02::brk_T2; }
	void brk_T2(void) { Push((u8)(pc >> 8)); addressModeCycleFn = &M65C02::brk_T3; }
	void brk_T3(void) { Push(pc & 0xff); addressModeCycleFn = &M65C02::brk_T4; }
	void brk_T4(void) { Push(status | FLAG_CONSTANT | FLAG_BREAK); addressModeCycleFn = &M65C02::brk_T5; }
	void brk_T5(void) { ea = dataBusReadFn(0xFFFE); addressModeCycleFn = &M65C02::brk_T6; }
	void brk_T6(void) { SetI(); ClearD(); pc = ea | (dataBusReadFn(0xFFFF) << 8); addressModeCycleFn = &M65C02::InstructionFetch; }

	// RESET. 7 cycles. Clears D.
	void Reset_T0(void) { sp = 0; dataBusReadFn(pc); addressModeCycleFn = &M65C02::Reset_T1; }
	void Reset_T1(void) { dataBusReadFn(pc); addressModeCycleFn = &M65C02::Reset_T2; }
	void Reset_T2(void) { dataBusReadFn(0x100 + sp--); addressModeCycleFn = &M65C02::Reset_T3; }
	void Reset_T3(void) { dataBusReadFn(0x100 + sp--); addressModeCycleFn = &M65C02::Reset_T4; }
	void Reset_T4(void) { ClearB(); ClearD(); dataBusReadFn(0x100 + sp--); addressModeCycleFn = &M65C02::Reset_T5; }
	void Reset_T5(void) { ea = dataBusReadFn(0xFFFC); addressModeCycleFn = &M65C02::Reset_T6; }
	void Reset_T6(void) { SetI(); pc = ea | (dataBusReadFn(0xFFFD) << 8); addressModeCycleFn = &M65C02::InstructionFetch; }

	// IRQ. 7 cycles. Clears D.
	void IRQ_T1(void) { dataBusReadFn(pc); addressModeCycleFn = &M65C02::IRQ_T2; }
	void IRQ_T2(void) { Push((u8)(pc >> 8)); addressModeCycleFn = &M65C02::IRQ_T3; }
	void IRQ_T3(void) { Push(pc & 0xff); addressModeCycleFn = &M65C02::IRQ_T4; }
	void IRQ_T4(void) { Push((status | FLAG_CONSTANT) & ~FLAG_BREAK); addressModeCycleFn = &M65C02::IRQ_T5; }
	void IRQ_T5(void) { ea = dataBusReadFn(0xFFFE); addressModeCycleFn = &M65C02::IRQ_T6; }
	void IRQ_T6(void) { SetI(); ClearD(); pc = ea | (dataBusReadFn(0xFFFF) << 8); addressModeCycleFn = &M65C02::InstructionFetch; }

	inline void ClearB() { status &= (~FLAG_BREAK); }
	inline void SetB() { status |= FLAG_BREAK; }
	inline void ClearC() { status &= (~FLAG_CARRY); }
	inline void SetC() { status |= FLAG_CARRY; }
	inline void SetC(u16 test) { test != 0 ? SetC() : ClearC(); }
	inline void ClearZ() { status &= (~FLAG_ZERO); }
	inline void SetZ() { status |= FLAG_ZERO; }
	inline void SetZ(u16 test) { test != 0 ? SetZ() : ClearZ(); }
	inline void ClearI() { status &= (~FLAG_INTERRUPT); }
	inline void SetI() { status |= FLAG_INTERRUPT; }
	inline void ClearD() { status &= (~FLAG_DECIMAL); }
	inline void SetD() { status |= FLAG_DECIMAL; }
	inline void ClearV() { status &= (~FLAG_OVERFLOW); }
	inline void SetV() { status |= FLAG_OVERFLOW; }
	inline void SetV(u16 test) { test != 0 ? SetV() : ClearV(); }
	inline void ClearN() { status &= (~FLAG_SIGN); }
	inline void SetN() { status |= FLAG_SIGN; }
	inline void SetN(u16 test) { test != 0 ? SetN() : ClearN(); }

	inline void EstablishZ(u16 val) { SetZ((val & 0x00FF) == 0); }
	inline void EstablishN(u16 val) { SetN(val & 0x0080); }
	inline void EstablishC(u16 val) { SetC(val & 0xFF00); }
	inline void EstablishV(u16 result, u8 val) { SetV((result ^ a) & (result ^ val) & 0x0080); }
	inline void EstablishNZ(u16 val) { EstablishZ(val); EstablishN(val); }

public:
	M65C02() : status(FLAG_CONSTANT), dataBusReadFn(0), dataBusWriteFn(0) {}
	M65C02(C02DataBusReadFn dataBusReadFn, C02DataBusWriteFn dataBusWriteFn) { SetBusFunctions(dataBusReadFn, dataBusWriteFn); }
	void SetBusFunctions(C02DataBusReadFn dataBusReadFn, C02DataBusWriteFn dataBusWriteFn) { this->dataBusReadFn = dataBusReadFn; this->dataBusWriteFn = dataBusWriteFn; status = FLAG_CONSTANT; Reset(); }
	void Reset(void);
	void Step(void);

	inline bool IRQDisabled(void) const { return (status & FLAG_INTERRUPT) != 0; }

	void GetRegs(u16& PC, u8& SP, u8& A, u8& X, u8& Y, u8& Status) { PC = pc; SP = sp; A = a; X = x; Y = y; Status = status; }
	u16 GetPC() const { return pc; }
	u8 GetA() const { return a; }
	u8 GetX() const { return x; }
	u8 GetY() const { return y; }
	u8 GetStatus() const { return status; }
	// Emulate the SYNC signal.
	bool SYNC(void) const { return addressModeCycleFn == &M65C02::InstructionFetch; }

	Interrupt IRQ;
};
#endif
