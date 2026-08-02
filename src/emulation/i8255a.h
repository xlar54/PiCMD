// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// Intel 8255A PPI emulation.
// Ported from VICE's i8255a.c/h written by Roberto Muscedere.
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

#ifndef I8255A_H
#define I8255A_H

#include "types.h"

/* control register bit masks */
#define I8255A_G2_PC 0x01
#define I8255A_G2_PB 0x02
#define I8255A_G2_MS 0x04
#define I8255A_G1_PC 0x08
#define I8255A_G1_PA 0x10
#define I8255A_G1_MS 0x60
#define I8255A_MODE  0x80

typedef struct _i8255a_state
{
	u8 ctrl;
	u8 data[3];

	/* hooks that set the i/o lines */
	void (*set_pa)(struct _i8255a_state*, u8, s8);
	void (*set_pb)(struct _i8255a_state*, u8, s8);
	void (*set_pc)(struct _i8255a_state*, u8, s8);

	/* hooks that read the status of i/o lines */
	u8 (*get_pa)(struct _i8255a_state*, s8);
	u8 (*get_pb)(struct _i8255a_state*, s8);
	u8 (*get_pc)(struct _i8255a_state*, s8);

	/* parent context that may be used by the hooks */
	void* p;
} i8255a_state;

void i8255a_reset(i8255a_state* ctx);
u8 i8255a_read(i8255a_state* ctx, s8 reg);
u8 i8255a_peek(i8255a_state* ctx, s8 reg);
void i8255a_store(i8255a_state* ctx, s8 reg, u8 data);

#endif
