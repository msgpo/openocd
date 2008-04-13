/***************************************************************************
 *   Copyright (C) 2008 by �yvind Harboe                                   *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "replacements.h"

#include "jtag.h"
#include "bitbang.h"


int dummy_speed(int speed);
int dummy_register_commands(struct command_context_s *cmd_ctx);
int dummy_init(void);
int dummy_quit(void);

/* The dummy driver is used to easily check the code path 
 * where the target is unresponsive.
 */
jtag_interface_t dummy_interface = 
{
	.name = "dummy",
	
	.execute_queue = bitbang_execute_queue,

	.speed = dummy_speed,	
	.register_commands = dummy_register_commands,
	.init = dummy_init,
	.quit = dummy_quit,
};

int dummy_read(void);
void dummy_write(int tck, int tms, int tdi);
void dummy_reset(int trst, int srst);
void dummy_led(int on);

bitbang_interface_t dummy_bitbang =
{
	.read = dummy_read,
	.write = dummy_write,
	.reset = dummy_reset,
	.blink = dummy_led
};

int dummy_read(void)
{
	return 1;
}


void dummy_write(int tck, int tms, int tdi)
{
}

void dummy_reset(int trst, int srst)
{
}
	

int dummy_speed(int speed)
{
	return ERROR_OK;
}

int dummy_register_commands(struct command_context_s *cmd_ctx)
{
	return ERROR_OK;
}


int dummy_init(void)
{
	bitbang_interface = &dummy_bitbang;	

	return ERROR_OK;
}

int dummy_quit(void)
{
	return ERROR_OK;
}


void dummy_led(int on)
{
}

