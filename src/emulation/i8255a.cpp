// Pi-CMD - A Commodore CMD-HD hard drive emulator
//
// Intel 8255A PPI emulation.
// Ported from VICE's i8255a.c written by Roberto Muscedere.
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

#include "i8255a.h"
#include "debug.h"

void i8255a_reset(i8255a_state* ctx)
{
	ctx->ctrl = 0x1b;
	ctx->data[0] = 0;
	ctx->data[1] = 0;
	ctx->data[2] = 0;

	/* on reset relay inputs to outputs */
	if (ctx->get_pa && ctx->set_pa)
	{
		ctx->set_pa(ctx, ctx->get_pa(ctx, 0), 0);
	}
	if (ctx->get_pb && ctx->set_pb)
	{
		ctx->set_pb(ctx, ctx->get_pb(ctx, 1), 1);
	}
	if (ctx->get_pc && ctx->set_pc)
	{
		ctx->set_pc(ctx, ctx->get_pc(ctx, 2), 2);
	}
}

u8 i8255a_read(i8255a_state* ctx, s8 reg)
{
	u8 data = 0xff;

	switch (reg & 3)
	{
	case 0:
		if (ctx->ctrl & I8255A_G1_PA)
		{
			if (ctx->get_pa)
			{
				data = ctx->get_pa(ctx, reg);
			}
		}
		else
		{
			data = ctx->data[0];
		}
		break;
	case 1:
		if (ctx->ctrl & I8255A_G2_PB)
		{
			if (ctx->get_pb)
			{
				data = ctx->get_pb(ctx, reg);
			}
		}
		else
		{
			data = ctx->data[1];
		}
		break;
	case 2:
		if (ctx->ctrl & (I8255A_G2_PC | I8255A_G1_PC))
		{
			if (ctx->get_pc)
			{
				data = ctx->get_pc(ctx, reg);
			}
		}
		if (!(ctx->ctrl & I8255A_G2_PC))
		{
			data = (data & 0xf0) | (ctx->data[2] & 0x0f);
		}
		if (!(ctx->ctrl & I8255A_G1_PC))
		{
			data = (data & 0x0f) | (ctx->data[2] & 0xf0);
		}
		break;
	}
	return data;
}

u8 i8255a_peek(i8255a_state* ctx, s8 reg)
{
	if ((reg & 3) == 3)
	{
		return ctx->ctrl;
	}
	else
	{
		return i8255a_read(ctx, reg | 4);
	}
}

void i8255a_store(i8255a_state* ctx, s8 reg, u8 data)
{
	reg = reg & 3;
	switch (reg)
	{
	case 0:
		ctx->data[0] = data;
		if (!(ctx->ctrl & I8255A_G1_PA) && ctx->set_pa)
		{
			ctx->set_pa(ctx, ctx->data[0], 0);
		}
		break;
	case 1:
		ctx->data[1] = data;
		if (!(ctx->ctrl & I8255A_G2_PB) && ctx->set_pb)
		{
			ctx->set_pb(ctx, ctx->data[1], 1);
		}
		break;
	case 3:
		if (!(data & I8255A_MODE))
		{
			break;
		}
		if ((data & I8255A_G2_MS) || (data & I8255A_G1_MS))
		{
			DEBUG_LOG("I8255A: Unsupported mode set.\r\n");
		}
		ctx->ctrl = data;
		if (!(ctx->ctrl & I8255A_G1_PA) && ctx->set_pa)
		{
			ctx->set_pa(ctx, ctx->data[0], 3);
		}
		else if ((ctx->ctrl & I8255A_G1_PA) && ctx->set_pa)
		{
			ctx->set_pa(ctx, ctx->get_pa(ctx, 3), 3);
		}
		if (!(ctx->ctrl & I8255A_G2_PB) && ctx->set_pb)
		{
			ctx->set_pb(ctx, ctx->data[1], 3);
		}
		else if ((ctx->ctrl & I8255A_G2_PB) && ctx->set_pb)
		{
			ctx->set_pb(ctx, ctx->get_pb(ctx, 3), 3);
		}
		/* fall through */
	case 2:
		if (reg == 2)
		{
			ctx->data[2] = data;
		}
		else
		{
			data = ctx->data[2];
		}
		/* if both upper and lower port c are inputs, we are done */
		if ((ctx->ctrl & I8255A_G2_PC) && (ctx->ctrl & I8255A_G1_PC))
		{
			if (reg == 3)
			{
				ctx->set_pc(ctx, ctx->get_pc(ctx, 3), 3);
			}
			break;
		}
		/* one or more is an output; grab from port c if one is an input */
		if (ctx->ctrl & (I8255A_G2_PC | I8255A_G1_PC))
		{
			if (ctx->get_pc)
			{
				data = ctx->get_pc(ctx, reg);
			}
		}
		if (!(ctx->ctrl & I8255A_G2_PC))
		{
			data = (data & 0xf0) | (ctx->data[2] & 0x0f);
		}
		if (!(ctx->ctrl & I8255A_G1_PC))
		{
			data = (data & 0x0f) | (ctx->data[2] & 0xf0);
		}
		if (ctx->set_pc)
		{
			ctx->set_pc(ctx, data, reg);
		}
		break;
	}
}
