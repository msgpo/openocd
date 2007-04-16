/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
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

#include "arm966e.h"

#include "arm7_9_common.h"
#include "register.h"
#include "target.h"
#include "armv4_5.h"
#include "embeddedice.h"
#include "log.h"
#include "jtag.h"
#include "arm_jtag.h"

#include <stdlib.h>
#include <string.h>

#if 0
#define _DEBUG_INSTRUCTION_EXECUTION_
#endif

/* cli handling */
int arm966e_register_commands(struct command_context_s *cmd_ctx);

/* forward declarations */
int arm966e_deassert_reset(target_t *target);
int arm966e_assert_reset(target_t *target);
int arm966e_target_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc, struct target_s *target);
int arm966e_init_target(struct command_context_s *cmd_ctx, struct target_s *target);
int arm966e_quit(void);

target_type_t arm966e_target =
{
	.name = "arm966e",

	.poll = arm7_9_poll,
	.arch_state = armv4_5_arch_state,

	.halt = arm7_9_halt,
	.resume = arm7_9_resume,
	.step = arm7_9_step,

	.assert_reset = arm966e_assert_reset,
	.deassert_reset = arm966e_deassert_reset,
	.soft_reset_halt = arm7_9_soft_reset_halt,
	.prepare_reset_halt = arm7_9_prepare_reset_halt,

	.get_gdb_reg_list = armv4_5_get_gdb_reg_list,

	.read_memory = arm7_9_read_memory,
	.write_memory = arm7_9_write_memory,
	.bulk_write_memory = arm7_9_bulk_write_memory,

	.run_algorithm = armv4_5_run_algorithm,
	
	.add_breakpoint = arm7_9_add_breakpoint,
	.remove_breakpoint = arm7_9_remove_breakpoint,
	.add_watchpoint = arm7_9_add_watchpoint,
	.remove_watchpoint = arm7_9_remove_watchpoint,

	.register_commands = arm966e_register_commands,
	.target_command = arm966e_target_command,
	.init_target = arm966e_init_target,
	.quit = arm966e_quit,
};

int arm966e_assert_reset(target_t *target)
{
	int retval;
	
	DEBUG("target->state: %s", target_state_strings[target->state]);
	
	if (target->state == TARGET_HALTED || target->state == TARGET_UNKNOWN)
	{
		/* assert SRST and TRST */
		/* system would get ouf sync if we didn't reset test-logic, too */
		if ((retval = jtag_add_reset(1, 1)) != ERROR_OK)
		{
			if (retval == ERROR_JTAG_RESET_CANT_SRST)
			{
				WARNING("can't assert srst");
				return retval;
			}
			else
			{
				ERROR("unknown error");
				exit(-1);
			}
		}
		jtag_add_sleep(5000);
		if ((retval = jtag_add_reset(0, 1)) != ERROR_OK)
		{
			if (retval == ERROR_JTAG_RESET_WOULD_ASSERT_TRST)
			{
				WARNING("srst resets test logic, too");
				retval = jtag_add_reset(1, 1);
			}
		}
	}
	else
	{
		if ((retval = jtag_add_reset(0, 1)) != ERROR_OK)
		{
			if (retval == ERROR_JTAG_RESET_WOULD_ASSERT_TRST)
			{
				WARNING("srst resets test logic, too");
				retval = jtag_add_reset(1, 1);
			}
			
			if (retval == ERROR_JTAG_RESET_CANT_SRST)
			{
				WARNING("can't assert srst");
				return retval;
			}
			else if (retval != ERROR_OK)
			{
				ERROR("unknown error");
				exit(-1);
			}
		}
	}
	
	target->state = TARGET_RESET;
	jtag_add_sleep(50000);
	
	armv4_5_invalidate_core_regs(target);
	
	return ERROR_OK;
}

int arm966e_deassert_reset(target_t *target)
{
	arm7_9_deassert_reset( target );
	
	return ERROR_OK;
}

int arm966e_init_target(struct command_context_s *cmd_ctx, struct target_s *target)
{
	arm9tdmi_init_target(cmd_ctx, target);
		
	return ERROR_OK;
}

int arm966e_quit(void)
{
	
	return ERROR_OK;
}

int arm966e_init_arch_info(target_t *target, arm966e_common_t *arm966e, int chain_pos, char *variant)
{
	arm9tdmi_common_t *arm9tdmi = &arm966e->arm9tdmi_common;
	
	arm9tdmi_init_arch_info(target, arm9tdmi, chain_pos, variant);

	arm9tdmi->arch_info = arm966e;
	arm966e->common_magic = ARM966E_COMMON_MAGIC;
	
	return ERROR_OK;
}

int arm966e_target_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc, struct target_s *target)
{
	int chain_pos;
	char *variant = NULL;
	arm966e_common_t *arm966e = malloc(sizeof(arm966e_common_t));
	
	if (argc < 4)
	{
		ERROR("'target arm966e' requires at least one additional argument");
		exit(-1);
	}
	
	chain_pos = strtoul(args[3], NULL, 0);
	
	if (argc >= 5)
		variant = args[4];
	
	DEBUG("chain_pos: %i, variant: %s", chain_pos, variant);
	
	arm966e_init_arch_info(target, arm966e, chain_pos, variant);

	return ERROR_OK;
}

int arm966e_get_arch_pointers(target_t *target, armv4_5_common_t **armv4_5_p, arm7_9_common_t **arm7_9_p, arm9tdmi_common_t **arm9tdmi_p, arm966e_common_t **arm966e_p)
{
	armv4_5_common_t *armv4_5 = target->arch_info;
	arm7_9_common_t *arm7_9;
	arm9tdmi_common_t *arm9tdmi;
	arm966e_common_t *arm966e;
	
	if (armv4_5->common_magic != ARMV4_5_COMMON_MAGIC)
	{
		return -1;
	}
	
	arm7_9 = armv4_5->arch_info;
	if (arm7_9->common_magic != ARM7_9_COMMON_MAGIC)
	{
		return -1;
	}
	
	arm9tdmi = arm7_9->arch_info;
	if (arm9tdmi->common_magic != ARM9TDMI_COMMON_MAGIC)
	{
		return -1;
	}
	
	arm966e = arm9tdmi->arch_info;
	if (arm966e->common_magic != ARM966E_COMMON_MAGIC)
	{
		return -1;
	}
	
	*armv4_5_p = armv4_5;
	*arm7_9_p = arm7_9;
	*arm9tdmi_p = arm9tdmi;
	*arm966e_p = arm966e;
	
	return ERROR_OK;
}

int arm966e_read_cp15(target_t *target, int reg_addr, u32 *value)
{
	armv4_5_common_t *armv4_5 = target->arch_info;
	arm7_9_common_t *arm7_9 = armv4_5->arch_info;
	arm_jtag_t *jtag_info = &arm7_9->jtag_info;
	scan_field_t fields[3];
	u8 reg_addr_buf = reg_addr & 0x3f;
	u8 nr_w_buf = 0;
	
	jtag_add_end_state(TAP_RTI);
	arm_jtag_scann(jtag_info, 0xf);
	arm_jtag_set_instr(jtag_info, jtag_info->intest_instr);

	fields[0].device = jtag_info->chain_pos;
	fields[0].num_bits = 32;
	fields[0].out_value = NULL;
	fields[0].out_mask = NULL;
	fields[0].in_value = NULL;
	fields[0].in_check_value = NULL;
	fields[0].in_check_mask = NULL;
	fields[0].in_handler = NULL;
	fields[0].in_handler_priv = NULL;

	fields[1].device = jtag_info->chain_pos;
	fields[1].num_bits = 6;
	fields[1].out_value = &reg_addr_buf;
	fields[1].out_mask = NULL;
	fields[1].in_value = NULL;
	fields[1].in_check_value = NULL;
	fields[1].in_check_mask = NULL;
	fields[1].in_handler = NULL;
	fields[1].in_handler_priv = NULL;

	fields[2].device = jtag_info->chain_pos;
	fields[2].num_bits = 1;
	fields[2].out_value = &nr_w_buf;
	fields[2].out_mask = NULL;
	fields[2].in_value = NULL;
	fields[2].in_check_value = NULL;
	fields[2].in_check_mask = NULL;
	fields[2].in_handler = NULL;
	fields[2].in_handler_priv = NULL;
	
	jtag_add_dr_scan(3, fields, -1);

	fields[0].in_value = (u8*)value;

	jtag_add_dr_scan(3, fields, -1);

	return ERROR_OK;
}

int arm966e_write_cp15(target_t *target, int reg_addr, u32 value)
{
	armv4_5_common_t *armv4_5 = target->arch_info;
	arm7_9_common_t *arm7_9 = armv4_5->arch_info;
	arm_jtag_t *jtag_info = &arm7_9->jtag_info;
	scan_field_t fields[3];
	u8 reg_addr_buf = reg_addr & 0x3f;
	u8 nr_w_buf = 1;
	
	jtag_add_end_state(TAP_RTI);
	arm_jtag_scann(jtag_info, 0xf);
	arm_jtag_set_instr(jtag_info, jtag_info->intest_instr);

	fields[0].device = jtag_info->chain_pos;
	fields[0].num_bits = 32;
	fields[0].out_value = (u8*)&value;
	fields[0].out_mask = NULL;
	fields[0].in_value = NULL;
	fields[0].in_check_value = NULL;
	fields[0].in_check_mask = NULL;
	fields[0].in_handler = NULL;
	fields[0].in_handler_priv = NULL;

	fields[1].device = jtag_info->chain_pos;
	fields[1].num_bits = 6;
	fields[1].out_value = &reg_addr_buf;
	fields[1].out_mask = NULL;
	fields[1].in_value = NULL;
	fields[1].in_check_value = NULL;
	fields[1].in_check_mask = NULL;
	fields[1].in_handler = NULL;
	fields[1].in_handler_priv = NULL;

	fields[2].device = jtag_info->chain_pos;
	fields[2].num_bits = 1;
	fields[2].out_value = &nr_w_buf;
	fields[2].out_mask = NULL;
	fields[2].in_value = NULL;
	fields[2].in_check_value = NULL;
	fields[2].in_check_mask = NULL;
	fields[2].in_handler = NULL;
	fields[2].in_handler_priv = NULL;
	
	jtag_add_dr_scan(3, fields, -1);

	return ERROR_OK;
}

int arm966e_handle_cp15_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc)
{
	int retval;
	target_t *target = get_current_target(cmd_ctx);
	armv4_5_common_t *armv4_5;
	arm7_9_common_t *arm7_9;
	arm9tdmi_common_t *arm9tdmi;
	arm966e_common_t *arm966e;
	arm_jtag_t *jtag_info;

	if (arm966e_get_arch_pointers(target, &armv4_5, &arm7_9, &arm9tdmi, &arm966e) != ERROR_OK)
	{
		command_print(cmd_ctx, "current target isn't an ARM966e target");
		return ERROR_OK;
	}
	
	jtag_info = &arm7_9->jtag_info;
	
	if (target->state != TARGET_HALTED)
	{
		command_print(cmd_ctx, "target must be stopped for \"%s\" command", cmd);
		return ERROR_OK;
	}

	/* one or more argument, access a single register (write if second argument is given */
	if (argc >= 1)
	{
		int address = strtoul(args[0], NULL, 0);

		if (argc == 1)
		{
			u32 value;
			if ((retval = arm966e_read_cp15(target, address, &value)) != ERROR_OK)
			{
				command_print(cmd_ctx, "couldn't access reg %i", address);
				return ERROR_OK;
			}
			jtag_execute_queue();
			
			command_print(cmd_ctx, "%i: %8.8x", address, value);
		}
		else if (argc == 2)
		{
			u32 value = strtoul(args[1], NULL, 0);
			if ((retval = arm966e_write_cp15(target, address, value)) != ERROR_OK)
			{
				command_print(cmd_ctx, "couldn't access reg %i", address);
				return ERROR_OK;
			}
			command_print(cmd_ctx, "%i: %8.8x", address, value);
		}
	}

	return ERROR_OK;
}

int arm966e_register_commands(struct command_context_s *cmd_ctx)
{
	int retval;
	command_t *arm966e_cmd;
	
	retval = arm7_9_register_commands(cmd_ctx);
	arm966e_cmd = register_command(cmd_ctx, NULL, "arm966e", NULL, COMMAND_ANY, "arm966e specific commands");
	register_command(cmd_ctx, arm966e_cmd, "cp15", arm966e_handle_cp15_command, COMMAND_EXEC, "display/modify cp15 register <num> [value]");
	
	return ERROR_OK;
}
