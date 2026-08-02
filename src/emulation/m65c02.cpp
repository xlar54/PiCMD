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

#include "m65c02.h"

// Opcode function table for the R65C02.
M65C02::OpcodeCycleFunction M65C02::opcodeFunctions[256] =
{
//        0            1            2            3            4            5            6            7            8            9            A            B            C            D            E            F
&M65C02::BRK,&M65C02::ORA,&M65C02::NOP,&M65C02::NOP,&M65C02::TSB,&M65C02::ORA,&M65C02::ASL,&M65C02::RMB,&M65C02::PHP,&M65C02::ORA,&M65C02::ASL,&M65C02::NOP,&M65C02::TSB,&M65C02::ORA,&M65C02::ASL,&M65C02::BBR,// 0
&M65C02::BPL,&M65C02::ORA,&M65C02::ORA,&M65C02::NOP,&M65C02::TRB,&M65C02::ORA,&M65C02::ASL,&M65C02::RMB,&M65C02::CLC,&M65C02::ORA,&M65C02::INC,&M65C02::NOP,&M65C02::TRB,&M65C02::ORA,&M65C02::ASL,&M65C02::BBR,// 1
&M65C02::JSR,&M65C02::AND,&M65C02::NOP,&M65C02::NOP,&M65C02::BIT,&M65C02::AND,&M65C02::ROL,&M65C02::RMB,&M65C02::PLP,&M65C02::AND,&M65C02::ROL,&M65C02::NOP,&M65C02::BIT,&M65C02::AND,&M65C02::ROL,&M65C02::BBR,// 2
&M65C02::BMI,&M65C02::AND,&M65C02::AND,&M65C02::NOP,&M65C02::BIT,&M65C02::AND,&M65C02::ROL,&M65C02::RMB,&M65C02::SEC,&M65C02::AND,&M65C02::DEC,&M65C02::NOP,&M65C02::BIT,&M65C02::AND,&M65C02::ROL,&M65C02::BBR,// 3
&M65C02::RTI,&M65C02::EOR,&M65C02::NOP,&M65C02::NOP,&M65C02::NOP,&M65C02::EOR,&M65C02::LSR,&M65C02::RMB,&M65C02::PHA,&M65C02::EOR,&M65C02::LSR,&M65C02::NOP,&M65C02::JMP,&M65C02::EOR,&M65C02::LSR,&M65C02::BBR,// 4
&M65C02::BVC,&M65C02::EOR,&M65C02::EOR,&M65C02::NOP,&M65C02::NOP,&M65C02::EOR,&M65C02::LSR,&M65C02::RMB,&M65C02::CLI,&M65C02::EOR,&M65C02::PHY,&M65C02::NOP,&M65C02::NOP,&M65C02::EOR,&M65C02::LSR,&M65C02::BBR,// 5
&M65C02::RTS,&M65C02::ADC,&M65C02::NOP,&M65C02::NOP,&M65C02::STZ,&M65C02::ADC,&M65C02::ROR,&M65C02::RMB,&M65C02::PLA,&M65C02::ADC,&M65C02::ROR,&M65C02::NOP,&M65C02::JMP,&M65C02::ADC,&M65C02::ROR,&M65C02::BBR,// 6
&M65C02::BVS,&M65C02::ADC,&M65C02::ADC,&M65C02::NOP,&M65C02::STZ,&M65C02::ADC,&M65C02::ROR,&M65C02::RMB,&M65C02::SEI,&M65C02::ADC,&M65C02::PLY,&M65C02::NOP,&M65C02::JMP,&M65C02::ADC,&M65C02::ROR,&M65C02::BBR,// 7
&M65C02::BRA,&M65C02::STA,&M65C02::NOP,&M65C02::NOP,&M65C02::STY,&M65C02::STA,&M65C02::STX,&M65C02::SMB,&M65C02::DEY,&M65C02::BITimm,&M65C02::TXA,&M65C02::NOP,&M65C02::STY,&M65C02::STA,&M65C02::STX,&M65C02::BBS,// 8
&M65C02::BCC,&M65C02::STA,&M65C02::STA,&M65C02::NOP,&M65C02::STY,&M65C02::STA,&M65C02::STX,&M65C02::SMB,&M65C02::TYA,&M65C02::STA,&M65C02::TXS,&M65C02::NOP,&M65C02::STZ,&M65C02::STA,&M65C02::STZ,&M65C02::BBS,// 9
&M65C02::LDY,&M65C02::LDA,&M65C02::LDX,&M65C02::NOP,&M65C02::LDY,&M65C02::LDA,&M65C02::LDX,&M65C02::SMB,&M65C02::TAY,&M65C02::LDA,&M65C02::TAX,&M65C02::NOP,&M65C02::LDY,&M65C02::LDA,&M65C02::LDX,&M65C02::BBS,// A
&M65C02::BCS,&M65C02::LDA,&M65C02::LDA,&M65C02::NOP,&M65C02::LDY,&M65C02::LDA,&M65C02::LDX,&M65C02::SMB,&M65C02::CLV,&M65C02::LDA,&M65C02::TSX,&M65C02::NOP,&M65C02::LDY,&M65C02::LDA,&M65C02::LDX,&M65C02::BBS,// B
&M65C02::CPY,&M65C02::CMP,&M65C02::NOP,&M65C02::NOP,&M65C02::CPY,&M65C02::CMP,&M65C02::DEC,&M65C02::SMB,&M65C02::INY,&M65C02::CMP,&M65C02::DEX,&M65C02::NOP,&M65C02::CPY,&M65C02::CMP,&M65C02::DEC,&M65C02::BBS,// C
&M65C02::BNE,&M65C02::CMP,&M65C02::CMP,&M65C02::NOP,&M65C02::NOP,&M65C02::CMP,&M65C02::DEC,&M65C02::SMB,&M65C02::CLD,&M65C02::CMP,&M65C02::PHX,&M65C02::NOP,&M65C02::NOP,&M65C02::CMP,&M65C02::DEC,&M65C02::BBS,// D
&M65C02::CPX,&M65C02::SBC,&M65C02::NOP,&M65C02::NOP,&M65C02::CPX,&M65C02::SBC,&M65C02::INC,&M65C02::SMB,&M65C02::INX,&M65C02::SBC,&M65C02::NOP,&M65C02::NOP,&M65C02::CPX,&M65C02::SBC,&M65C02::INC,&M65C02::BBS,// E
&M65C02::BEQ,&M65C02::SBC,&M65C02::SBC,&M65C02::NOP,&M65C02::NOP,&M65C02::SBC,&M65C02::INC,&M65C02::SMB,&M65C02::SED,&M65C02::SBC,&M65C02::PLX,&M65C02::NOP,&M65C02::NOP,&M65C02::SBC,&M65C02::INC,&M65C02::BBS // F
};

// T1 address mode function table for the R65C02.
// Undefined opcodes in columns 3 and B are single cycle NOPs; the T0 fetch is
// the entire instruction so T1 is simply the next instruction fetch.
M65C02::AddressModeCycleFunction M65C02::T1AddressModeFunctions[256] =
{
//        0                   1                   2                   3                          4                   5                   6                    7                   8               9                   A               B                          C                    D                   E                     F
&M65C02::brk_T1,    &M65C02::idx_r_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_rmw_T1,&M65C02::zp_r_T1,  &M65C02::zp_rmw_T1,  &M65C02::zp_rmw_T1,&M65C02::ph_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_rmw_T1, &M65C02::abs_r_T1,  &M65C02::abs_rmw_T1,  &M65C02::zpbit_T1,//0
&M65C02::rel_T1,    &M65C02::idy_r_T1, &M65C02::izp_r_T1, &M65C02::InstructionFetch,&M65C02::zp_rmw_T1,&M65C02::zpx_r_T1, &M65C02::zpx_rmw_T1, &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_r_T1,&M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_rmw_T1, &M65C02::absx_r_T1, &M65C02::absx_rmwc_T1,&M65C02::zpbit_T1,//1
&M65C02::jsr_T1,    &M65C02::idx_r_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_r_T1,  &M65C02::zp_r_T1,  &M65C02::zp_rmw_T1,  &M65C02::zp_rmw_T1,&M65C02::pl_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_r_T1,   &M65C02::abs_r_T1,  &M65C02::abs_rmw_T1,  &M65C02::zpbit_T1,//2
&M65C02::rel_T1,    &M65C02::idy_r_T1, &M65C02::izp_r_T1, &M65C02::InstructionFetch,&M65C02::zpx_r_T1, &M65C02::zpx_r_T1, &M65C02::zpx_rmw_T1, &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_r_T1,&M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::absx_r_T1,  &M65C02::absx_r_T1, &M65C02::absx_rmwc_T1,&M65C02::zpbit_T1,//3
&M65C02::rti_T1,    &M65C02::idx_r_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_r_T1,  &M65C02::zp_r_T1,  &M65C02::zp_rmw_T1,  &M65C02::zp_rmw_T1,&M65C02::ph_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::jmpabs_T1,  &M65C02::abs_r_T1,  &M65C02::abs_rmw_T1,  &M65C02::zpbit_T1,//4
&M65C02::rel_T1,    &M65C02::idy_r_T1, &M65C02::izp_r_T1, &M65C02::InstructionFetch,&M65C02::zpx_r_T1, &M65C02::zpx_r_T1, &M65C02::zpx_rmw_T1, &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_r_T1,&M65C02::ph_T1,&M65C02::InstructionFetch,&M65C02::nop5c_T1,   &M65C02::absx_r_T1, &M65C02::absx_rmwc_T1,&M65C02::zpbit_T1,//5
&M65C02::rts_T1,    &M65C02::idx_r_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_w_T1,  &M65C02::zp_r_T1,  &M65C02::zp_rmw_T1,  &M65C02::zp_rmw_T1,&M65C02::pl_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::jmpind_T1,  &M65C02::abs_r_T1,  &M65C02::abs_rmw_T1,  &M65C02::zpbit_T1,//6
&M65C02::rel_T1,    &M65C02::idy_r_T1, &M65C02::izp_r_T1, &M65C02::InstructionFetch,&M65C02::zpx_w_T1, &M65C02::zpx_r_T1, &M65C02::zpx_rmw_T1, &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_r_T1,&M65C02::pl_T1,&M65C02::InstructionFetch,&M65C02::jmpiax_T1,  &M65C02::absx_r_T1, &M65C02::absx_rmwc_T1,&M65C02::zpbit_T1,//7
&M65C02::rel_T1,    &M65C02::idx_w_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_w_T1,  &M65C02::zp_w_T1,  &M65C02::zp_w_T1,    &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_w_T1,   &M65C02::abs_w_T1,  &M65C02::abs_w_T1,    &M65C02::zpbit_T1,//8
&M65C02::rel_T1,    &M65C02::idy_w_T1, &M65C02::izp_w_T1, &M65C02::InstructionFetch,&M65C02::zpx_w_T1, &M65C02::zpx_w_T1, &M65C02::zpy_w_T1,   &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_w_T1,&M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_w_T1,   &M65C02::absx_w_T1, &M65C02::absx_w_T1,   &M65C02::zpbit_T1,//9
&M65C02::imm_T1,    &M65C02::idx_r_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_r_T1,  &M65C02::zp_r_T1,  &M65C02::zp_r_T1,    &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_r_T1,   &M65C02::abs_r_T1,  &M65C02::abs_r_T1,    &M65C02::zpbit_T1,//A
&M65C02::rel_T1,    &M65C02::idy_r_T1, &M65C02::izp_r_T1, &M65C02::InstructionFetch,&M65C02::zpx_r_T1, &M65C02::zpx_r_T1, &M65C02::zpy_r_T1,   &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_r_T1,&M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::absx_r_T1,  &M65C02::absx_r_T1, &M65C02::absy_r_T1,   &M65C02::zpbit_T1,//B
&M65C02::imm_T1,    &M65C02::idx_r_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_r_T1,  &M65C02::zp_r_T1,  &M65C02::zp_rmw_T1,  &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_r_T1,   &M65C02::abs_r_T1,  &M65C02::abs_rmw_T1,  &M65C02::zpbit_T1,//C
&M65C02::rel_T1,    &M65C02::idy_r_T1, &M65C02::izp_r_T1, &M65C02::InstructionFetch,&M65C02::zpx_r_T1, &M65C02::zpx_r_T1, &M65C02::zpx_rmw_T1, &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_r_T1,&M65C02::ph_T1,&M65C02::InstructionFetch,&M65C02::abs_r_T1,   &M65C02::absx_r_T1, &M65C02::absx_rmw7_T1,&M65C02::zpbit_T1,//D
&M65C02::imm_T1,    &M65C02::idx_r_T1, &M65C02::imm_T1,   &M65C02::InstructionFetch,&M65C02::zp_r_T1,  &M65C02::zp_r_T1,  &M65C02::zp_rmw_T1,  &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::imm_T1,   &M65C02::sb_T1,&M65C02::InstructionFetch,&M65C02::abs_r_T1,   &M65C02::abs_r_T1,  &M65C02::abs_rmw_T1,  &M65C02::zpbit_T1,//E
&M65C02::rel_T1,    &M65C02::idy_r_T1, &M65C02::izp_r_T1, &M65C02::InstructionFetch,&M65C02::zpx_r_T1, &M65C02::zpx_r_T1, &M65C02::zpx_rmw_T1, &M65C02::zp_rmw_T1,&M65C02::sb_T1,&M65C02::absy_r_T1,&M65C02::pl_T1,&M65C02::InstructionFetch,&M65C02::abs_r_T1,   &M65C02::absx_r_T1, &M65C02::absx_rmw7_T1,&M65C02::zpbit_T1 //F
};

// CMOS ADC. In decimal mode the flags are valid and an extra cycle is consumed.
void M65C02::ADC(void)
{
	if (status & FLAG_DECIMAL)
	{
		decimalExtraCycle = 1;
		u16 result = (a & 0xf) + (value & 0xf) + (status & FLAG_CARRY);
		if (result > 0x9) result += 0x6;
		if (result <= 0x0f) result = (result & 0xf) + (a & 0xf0) + (value & 0xf0);
		else result = (result & 0xf) + (a & 0xf0) + (value & 0xf0) + 0x10;
		EstablishV(result, (u8)value);
		if ((result & 0x1f0) > 0x90) result += 0x60;
		EstablishC(result);
		EstablishNZ(result);	// CMOS: N and Z are valid for the adjusted result.
		a = (u8)result;
	}
	else
	{
		u16 result = a + value + (status & FLAG_CARRY);
		EstablishZ(result);
		EstablishC(result);
		EstablishV(result, (u8)value);
		EstablishN(result);
		a = (u8)result;
	}
}

// CMOS SBC. In decimal mode the flags are valid and an extra cycle is consumed.
void M65C02::SBC(void)
{
	u16 result = a - value - ((status & FLAG_CARRY) ? 0 : 1);
	if (status & FLAG_DECIMAL)
	{
		decimalExtraCycle = 1;
		u16 tmp_a = (a & 0xf) - (value & 0xf) - ((status & FLAG_CARRY) ? 0 : 1);
		if (tmp_a & 0x10) tmp_a = ((tmp_a - 6) & 0xf) | ((a & 0xf0) - (value & 0xf0) - 0x10);
		else tmp_a = (tmp_a & 0xf) | ((a & 0xf0) - (value & 0xf0));
		if (tmp_a & 0x100) tmp_a -= 0x60;
		SetC(result < 0x100);
		EstablishV(result, (u8)(value ^ 0xff));
		EstablishNZ(tmp_a);	// CMOS: N and Z are valid for the adjusted result.
		a = (u8)tmp_a;
	}
	else
	{
		EstablishNZ(result);
		SetC(result < 0x100);
		EstablishV(result, (u8)(value ^ 0xff));
		a = (u8)result;
	}
}

// On the CMOS part the page crossing penalty cycle re-reads the last byte of
// the instruction rather than performing a read from a partially formed address.
void M65C02::absx_r_T3(void)
{
	u16 startpage = ea & 0xFF00;
	ea += x;
	if (startpage != (ea & 0xFF00))
	{
		dataBusReadFn((u16)(pc - 1));
		addressModeCycleFn = &M65C02::absx_r_T4;
	}
	else
	{
		value = dataBusReadFn(ea);
		ExecuteOpcode();
	}
}

void M65C02::absy_r_T3(void)
{
	u16 startpage = ea & 0xFF00;
	ea += y;
	if (startpage != (ea & 0xFF00))
	{
		dataBusReadFn((u16)(pc - 1));
		addressModeCycleFn = &M65C02::absy_r_T4;
	}
	else
	{
		value = dataBusReadFn(ea);
		ExecuteOpcode();
	}
}

void M65C02::idy_r_T4(void)
{
	u16 startpage = ea & 0xFF00;
	ea += y;
	if (startpage != (ea & 0xFF00))
	{
		dataBusReadFn((u16)(pc - 1));
		addressModeCycleFn = &M65C02::idy_r_T5;
	}
	else
	{
		value = dataBusReadFn(ea);
		ExecuteOpcode();
	}
}

// ASL/LSR/ROL/ROR abs,x take 6 cycles on the CMOS part unless a page is crossed.
void M65C02::absx_rmwc_T3(void)
{
	u16 startpage = ea & 0xFF00;
	ea += x;
	if (startpage != (ea & 0xFF00))
	{
		dataBusReadFn((u16)(pc - 1));
		addressModeCycleFn = &M65C02::absx_rmwc_T4;
	}
	else
	{
		value = dataBusReadFn(ea);
		addressModeCycleFn = &M65C02::absx_rmwc_T5;
	}
}

void M65C02::rel_T2(void)
{
	dataBusReadFn(oldpc);
	pc = oldpc + ra;
	if ((oldpc & 0xFF00) == (pc & 0xFF00))
	{
		BranchTakenMaskingInterrupt = true;
		addressModeCycleFn = &M65C02::InstructionFetch;	// Opcode has already been executed in T1 so just move on to the next instruction.
	}
	else
	{
		addressModeCycleFn = &M65C02::rel_T3;
	}
}

// Interrupts are polled before starting a new instruction.
// T0 of every address mode (except reset).
void M65C02::InstructionFetch()
{
	opcode = dataBusReadFn(pc);

	if (IRQPending && !IRQDisabled())
	{
		IRQPending = 0;
		addressModeCycleFn = &M65C02::IRQ_T1;
	}
	else
	{
		pc++;
		addressModeCycleFn = T1AddressModeFunctions[opcode];
		opcodeCycleFn = opcodeFunctions[opcode];
	}
}

void M65C02::Step(void)
{
	bool irq;

	irq = IRQ.IsAsserted();
	if (irq && ((status & FLAG_INTERRUPT) == 0) && !CLIMaskingInterrupt && !BranchTakenMaskingInterrupt)
		IRQPending = 1;
	if (!irq)
		IRQPending = 0;

	if (CLIMaskingInterrupt)
		CLIMaskingInterrupt = false;
	if (BranchTakenMaskingInterrupt)
		BranchTakenMaskingInterrupt = false;

	(this->*M65C02::addressModeCycleFn)();
}

void M65C02::Reset(void)
{
	CLIMaskingInterrupt = false;
	BranchTakenMaskingInterrupt = false;
	decimalExtraCycle = 0;
	IRQ.Reset();
	IRQPending = 0;
	Reset_T0();
}
