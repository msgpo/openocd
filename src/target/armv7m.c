/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2006 by Magnus Lundin                                   *
 *   lundin@mlu.mine.nu                                                    *
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

#include "armv7m.h"
#include "register.h"
#include "target.h"
#include "log.h"
#include "jtag.h"
#include "arm_jtag.h"

#include <stdlib.h>
#include <string.h>

#if 0
#define _DEBUG_INSTRUCTION_EXECUTION_
#endif

char* armv7m_mode_strings[] =
{
	"Thread", "Thread (User)", "Handler", 
};

char* armv7m_exception_strings[] =
{
	"", "Reset", "NMI", "HardFault", "MemManage", "BusFault", "UsageFault", "RESERVED", "RESERVED", "RESERVED", "RESERVED",
	"SVCall", "DebugMonitor", "RESERVED", "PendSV", "SysTick"
};

char* armv7m_core_reg_list[] =
{
	/* Registers accessed through core debug */
	"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12",
	"sp", "lr", "pc",
	"xPSR", "msp", "psp",
	/* Registers accessed through special reg 20 */
	"primask", "basepri", "faultmask", "control"
};

u8 armv7m_gdb_dummy_fp_value[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

reg_t armv7m_gdb_dummy_fp_reg =
{
	"GDB dummy floating-point register", armv7m_gdb_dummy_fp_value, 0, 1, 96, NULL, 0, NULL, 0
};

u8 armv7m_gdb_dummy_fps_value[] = {0, 0, 0, 0};

reg_t armv7m_gdb_dummy_fps_reg =
{
	"GDB dummy floating-point status register", armv7m_gdb_dummy_fps_value, 0, 1, 32, NULL, 0, NULL, 0
};

#ifdef ARMV7_GDB_HACKS
u8 armv7m_gdb_dummy_cpsr_value[] = {0, 0, 0, 0};

reg_t armv7m_gdb_dummy_cpsr_reg =
{
	"GDB dummy cpsr register", armv7m_gdb_dummy_cpsr_value, 0, 1, 32, NULL, 0, NULL, 0
};
#endif

armv7m_core_reg_t armv7m_core_reg_list_arch_info[] = 
{
	/*  CORE_GP are accesible using the core debug registers */
	{0, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{1, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{2, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{3, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{4, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{5, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{6, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{7, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{8, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{9, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{10, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{11, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{12, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{13, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},
	{14, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},	
	{15, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL},

	{16, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL}, /* xPSR */
	{17, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL}, /* MSP */
	{18, ARMV7M_REGISTER_CORE_GP, ARMV7M_MODE_ANY, NULL, NULL}, /* PSP */

	/*  CORE_SP are accesible using coreregister 20 */
	{19, ARMV7M_REGISTER_CORE_SP, ARMV7M_MODE_ANY, NULL, NULL}, /* PRIMASK */
	{20, ARMV7M_REGISTER_CORE_SP, ARMV7M_MODE_ANY, NULL, NULL}, /* BASEPRI */
	{21, ARMV7M_REGISTER_CORE_SP, ARMV7M_MODE_ANY, NULL, NULL}, /* FAULTMASK */
	{22, ARMV7M_REGISTER_CORE_SP, ARMV7M_MODE_ANY, NULL, NULL}  /* CONTROL */
};

int armv7m_core_reg_arch_type = -1;

int armv7m_restore_context(target_t *target)
{
	int i;
	
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;

	LOG_DEBUG(" ");

	if (armv7m->pre_restore_context)
		armv7m->pre_restore_context(target);
		
	for (i = ARMV7NUMCOREREGS-1; i >= 0; i--)
	{
		if (armv7m->core_cache->reg_list[i].dirty)
		{
			armv7m->write_core_reg(target, i);
		}
	}
	
	if (armv7m->post_restore_context)
		armv7m->post_restore_context(target);
		
	return ERROR_OK;		
}

/* Core state functions */
char *armv7m_exception_string(int number)
{
	static char enamebuf[32];
	
	if ((number < 0) | (number > 511))
		return "Invalid exception";
	if (number < 16)
		return armv7m_exception_strings[number];
	sprintf(enamebuf, "External Interrupt(%i)", number - 16);
	return enamebuf;
}

int armv7m_get_core_reg(reg_t *reg)
{
	int retval;
	armv7m_core_reg_t *armv7m_reg = reg->arch_info;
	target_t *target = armv7m_reg->target;
	armv7m_common_t *armv7m_target = target->arch_info;
	
	if (target->state != TARGET_HALTED)
	{
		return ERROR_TARGET_NOT_HALTED;
	}

	retval = armv7m_target->read_core_reg(target, armv7m_reg->num);
	
	return retval;
}

int armv7m_set_core_reg(reg_t *reg, u8 *buf)
{
	armv7m_core_reg_t *armv7m_reg = reg->arch_info;
	target_t *target = armv7m_reg->target;
	u32 value = buf_get_u32(buf, 0, 32);
		
	if (target->state != TARGET_HALTED)
	{
		return ERROR_TARGET_NOT_HALTED;
	}
		
	buf_set_u32(reg->value, 0, 32, value);
	reg->dirty = 1;
	reg->valid = 1;

	return ERROR_OK;
}

int armv7m_read_core_reg(struct target_s *target, int num)
{
	u32 reg_value;
	int retval;
	armv7m_core_reg_t * armv7m_core_reg;
	
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;
		
	if ((num < 0) || (num >= ARMV7NUMCOREREGS))
		return ERROR_INVALID_ARGUMENTS;

	armv7m_core_reg = armv7m->core_cache->reg_list[num].arch_info;
	retval = armv7m->load_core_reg_u32(target, armv7m_core_reg->type, armv7m_core_reg->num, &reg_value);
	buf_set_u32(armv7m->core_cache->reg_list[num].value, 0, 32, reg_value);
	armv7m->core_cache->reg_list[num].valid = 1;
	armv7m->core_cache->reg_list[num].dirty = 0;
	
	return ERROR_OK;	
}

int armv7m_write_core_reg(struct target_s *target, int num)
{
	int retval;
	u32 reg_value;
	armv7m_core_reg_t *armv7m_core_reg;
	
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;

	if ((num < 0) || (num >= ARMV7NUMCOREREGS))
		return ERROR_INVALID_ARGUMENTS;
	
	reg_value = buf_get_u32(armv7m->core_cache->reg_list[num].value, 0, 32);
	armv7m_core_reg = armv7m->core_cache->reg_list[num].arch_info;
	retval = armv7m->store_core_reg_u32(target, armv7m_core_reg->type, armv7m_core_reg->num, reg_value);
	if (retval != ERROR_OK)
	{
		LOG_ERROR("JTAG failure");
		armv7m->core_cache->reg_list[num].dirty = armv7m->core_cache->reg_list[num].valid;
		return ERROR_JTAG_DEVICE_ERROR;
	}
	LOG_DEBUG("write core reg %i value 0x%x", num , reg_value);
	armv7m->core_cache->reg_list[num].valid = 1;
	armv7m->core_cache->reg_list[num].dirty = 0;
	
	return ERROR_OK;
}

int armv7m_invalidate_core_regs(target_t *target)
{
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;
	int i;
	
	for (i = 0; i < armv7m->core_cache->num_regs; i++)
	{
		armv7m->core_cache->reg_list[i].valid = 0;
		armv7m->core_cache->reg_list[i].dirty = 0;
	}
	
	return ERROR_OK;
}

int armv7m_get_gdb_reg_list(target_t *target, reg_t **reg_list[], int *reg_list_size)
{
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;
	int i;
	
	*reg_list_size = 26;
	*reg_list = malloc(sizeof(reg_t*) * (*reg_list_size));
	
	for (i = 0; i < 16; i++)
	{
		(*reg_list)[i] = &armv7m->core_cache->reg_list[i];
	}
	
	for (i = 16; i < 24; i++)
	{
		(*reg_list)[i] = &armv7m_gdb_dummy_fp_reg;
	}
	
	(*reg_list)[24] = &armv7m_gdb_dummy_fps_reg;

#ifdef ARMV7_GDB_HACKS
	/* use dummy cpsr reg otherwise gdb may try and set the thumb bit */
	(*reg_list)[25] = &armv7m_gdb_dummy_cpsr_reg;
	
	/* ARMV7M is always in thumb mode, try to make GDB understand this
	 * if it does not support this arch */
	armv7m->core_cache->reg_list[15].value[0] |= 1;
#else
	(*reg_list)[25] = &armv7m->core_cache->reg_list[ARMV7M_xPSR];
#endif

	return ERROR_OK;
}

int armv7m_run_algorithm(struct target_s *target, int num_mem_params, mem_param_t *mem_params, int num_reg_params, reg_param_t *reg_params, u32 entry_point, u32 exit_point, int timeout_ms, void *arch_info)
{
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;
	armv7m_algorithm_t *armv7m_algorithm_info = arch_info;
	enum armv7m_mode core_mode = armv7m->core_mode;
	int retval = ERROR_OK;
	u32 pc;
	int i;
	u32 context[ARMV7NUMCOREREGS];
	
	if (armv7m_algorithm_info->common_magic != ARMV7M_COMMON_MAGIC)
	{
		LOG_ERROR("current target isn't an ARMV7M target");
		return ERROR_TARGET_INVALID;
	}
	
	if (target->state != TARGET_HALTED)
	{
		LOG_WARNING("target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}
	
	/* refresh core register cache */
	/* Not needed if core register cache is always consistent with target process state */ 
	for (i = 0; i < ARMV7NUMCOREREGS; i++)
	{
		if (!armv7m->core_cache->reg_list[i].valid)
			armv7m->read_core_reg(target, i);
		context[i] = buf_get_u32(armv7m->core_cache->reg_list[i].value, 0, 32);
	}
	
	for (i = 0; i < num_mem_params; i++)
	{
		target_write_buffer(target, mem_params[i].address, mem_params[i].size, mem_params[i].value);
	}
	
	for (i = 0; i < num_reg_params; i++)
	{
		reg_t *reg = register_get_by_name(armv7m->core_cache, reg_params[i].reg_name, 0);
		u32 regvalue;
		
		if (!reg)
		{
			LOG_ERROR("BUG: register '%s' not found", reg_params[i].reg_name);
			exit(-1);
		}
		
		if (reg->size != reg_params[i].size)
		{
			LOG_ERROR("BUG: register '%s' size doesn't match reg_params[i].size", reg_params[i].reg_name);
			exit(-1);
		}
		
		regvalue = buf_get_u32(reg_params[i].value, 0, 32);
		armv7m_set_core_reg(reg, reg_params[i].value);
	}
	
	if (armv7m_algorithm_info->core_mode != ARMV7M_MODE_ANY)
	{
		LOG_DEBUG("setting core_mode: 0x%2.2x", armv7m_algorithm_info->core_mode);
		buf_set_u32(armv7m->core_cache->reg_list[ARMV7M_CONTROL].value, 0, 1, armv7m_algorithm_info->core_mode);
		armv7m->core_cache->reg_list[ARMV7M_CONTROL].dirty = 1;
		armv7m->core_cache->reg_list[ARMV7M_CONTROL].valid = 1;
	}
	
	/* ARMV7M always runs in Thumb state */
	if ((retval = breakpoint_add(target, exit_point, 2, BKPT_SOFT)) != ERROR_OK)
	{
		LOG_ERROR("can't add breakpoint to finish algorithm execution");
		return ERROR_TARGET_FAILURE;
	}
	
	/* This code relies on the target specific  resume() and  poll()->debug_entry() 
	sequence to write register values to the processor and the read them back */
	target_resume(target, 0, entry_point, 1, 1);
	target_poll(target);
	
	while (target->state != TARGET_HALTED)
	{
		usleep(5000);
		target_poll(target);
		if ((timeout_ms -= 5) <= 0)
		{
			LOG_ERROR("timeout waiting for algorithm to complete, trying to halt target");
			target_halt(target);
			timeout_ms = 1000;
			while (target->state != TARGET_HALTED)
			{
				usleep(10000);
				target_poll(target);
				if ((timeout_ms -= 10) <= 0)
				{
					LOG_ERROR("target didn't reenter debug state, exiting");
					exit(-1);
				}
			}
			armv7m->load_core_reg_u32(target, ARMV7M_REGISTER_CORE_GP, 15, &pc);
			LOG_DEBUG("failed algoritm halted at 0x%x ", pc); 
			retval = ERROR_TARGET_TIMEOUT;
		}
	}
	
	breakpoint_remove(target, exit_point);
	
	/* Read memory values to mem_params[] */
	for (i = 0; i < num_mem_params; i++)
	{
		if (mem_params[i].direction != PARAM_OUT)
			target_read_buffer(target, mem_params[i].address, mem_params[i].size, mem_params[i].value);
	}
	
	/* Copy core register values to reg_params[] */
	for (i = 0; i < num_reg_params; i++)
	{
		if (reg_params[i].direction != PARAM_OUT)
		{
			reg_t *reg = register_get_by_name(armv7m->core_cache, reg_params[i].reg_name, 0);
		
			if (!reg)
			{
				LOG_ERROR("BUG: register '%s' not found", reg_params[i].reg_name);
				exit(-1);
			}
			
			if (reg->size != reg_params[i].size)
			{
				LOG_ERROR("BUG: register '%s' size doesn't match reg_params[i].size", reg_params[i].reg_name);
				exit(-1);
			}
			
			buf_set_u32(reg_params[i].value, 0, 32, buf_get_u32(reg->value, 0, 32));
		}
	}
	
	for (i = ARMV7NUMCOREREGS-1; i >= 0; i--)
	{
		LOG_DEBUG("restoring register %s with value 0x%8.8x", armv7m->core_cache->reg_list[i].name, context[i]);
		buf_set_u32(armv7m->core_cache->reg_list[i].value, 0, 32, context[i]);
		armv7m->core_cache->reg_list[i].valid = 1;
		armv7m->core_cache->reg_list[i].dirty = 1;
	}
	
	armv7m->core_mode = core_mode;
	
	return retval;
}

int armv7m_arch_state(struct target_s *target)
{
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;
	
	LOG_USER("target halted due to %s, current mode: %s %s\nxPSR: 0x%8.8x pc: 0x%8.8x",
		target_debug_reason_strings[target->debug_reason],
		armv7m_mode_strings[armv7m->core_mode],
		armv7m_exception_string(armv7m->exception_number),
		buf_get_u32(armv7m->core_cache->reg_list[ARMV7M_xPSR].value, 0, 32),
		buf_get_u32(armv7m->core_cache->reg_list[15].value, 0, 32));
	
	return ERROR_OK;
}

reg_cache_t *armv7m_build_reg_cache(target_t *target)
{
	/* get pointers to arch-specific information */
	armv7m_common_t *armv7m = target->arch_info;

	int num_regs = ARMV7NUMCOREREGS;
	reg_cache_t **cache_p = register_get_last_cache_p(&target->reg_cache);
	reg_cache_t *cache = malloc(sizeof(reg_cache_t));
	reg_t *reg_list = malloc(sizeof(reg_t) * num_regs);
	armv7m_core_reg_t *arch_info = malloc(sizeof(armv7m_core_reg_t) * num_regs);
	int i;
	
	if (armv7m_core_reg_arch_type == -1)
		armv7m_core_reg_arch_type = register_reg_arch_type(armv7m_get_core_reg, armv7m_set_core_reg);
		
	/* Build the process context cache */ 
	cache->name = "arm v7m registers";
	cache->next = NULL;
	cache->reg_list = reg_list;
	cache->num_regs = num_regs;
	(*cache_p) = cache;
	armv7m->core_cache = cache;
	
	for (i = 0; i < num_regs; i++)
	{
		arch_info[i] = armv7m_core_reg_list_arch_info[i];
		arch_info[i].target = target;
		arch_info[i].armv7m_common = armv7m;
		reg_list[i].name = armv7m_core_reg_list[i];
		reg_list[i].size = 32;
		reg_list[i].value = calloc(1, 4);
		reg_list[i].dirty = 0;
		reg_list[i].valid = 0;
		reg_list[i].bitfield_desc = NULL;
		reg_list[i].num_bitfields = 0;
		reg_list[i].arch_type = armv7m_core_reg_arch_type;
		reg_list[i].arch_info = &arch_info[i];
	}
	
	return cache;
}

int armv7m_init_target(struct command_context_s *cmd_ctx, struct target_s *target)
{
	armv7m_build_reg_cache(target);
	
	return ERROR_OK;
}

int armv7m_init_arch_info(target_t *target, armv7m_common_t *armv7m)
{
	/* register arch-specific functions */
	
	target->arch_info = armv7m;
	armv7m->read_core_reg = armv7m_read_core_reg;
	armv7m->write_core_reg = armv7m_write_core_reg;
	
	return ERROR_OK;
}

int armv7m_register_commands(struct command_context_s *cmd_ctx)
{
	return ERROR_OK;
}

int armv7m_checksum_memory(struct target_s *target, u32 address, u32 count, u32* checksum)
{
	working_area_t *crc_algorithm;
	armv7m_algorithm_t armv7m_info;
	reg_param_t reg_params[2];
	int retval;
	
	u16 cortex_m3_crc_code[] = {	    
		0x4602,					/* mov	r2, r0 */
		0xF04F, 0x30FF,			/* mov	r0, #0xffffffff */
		0x460B,					/* mov	r3, r1 */
		0xF04F, 0x0400,			/* mov	r4, #0 */
		0xE013,					/* b	ncomp */
								/* nbyte: */
		0x5D11,					/* ldrb	r1, [r2, r4] */
		0xF8DF, 0x7028,			/* ldr		r7, CRC32XOR */
		0xEA80, 0x6001,			/* eor		r0, r0, r1, asl #24 */
		
		0xF04F, 0x0500,			/* mov		r5, #0 */
								/* loop: */
		0x2800,					/* cmp		r0, #0 */
		0xEA4F, 0x0640,			/* mov		r6, r0, asl #1 */
		0xF105, 0x0501,			/* add		r5, r5, #1 */
		0x4630,					/* mov		r0, r6 */
		0xBFB8,					/* it		lt */
		0xEA86, 0x0007,			/* eor		r0, r6, r7 */
		0x2D08, 				/* cmp		r5, #8 */
		0xD1F4,					/* bne		loop */
		
		0xF104, 0x0401,			/* add	r4, r4, #1 */
								/* ncomp: */
		0x429C,					/* cmp	r4, r3 */
		0xD1E9,					/* bne	nbyte */
								/* end: */
		0xE7FE,					/* b	end */
		0x1DB7, 0x04C1			/* CRC32XOR:	.word 0x04C11DB7 */
	};

	int i;
	
	if (target_alloc_working_area(target, sizeof(cortex_m3_crc_code), &crc_algorithm) != ERROR_OK)
	{
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}
	
	/* convert flash writing code into a buffer in target endianness */
	for (i = 0; i < (sizeof(cortex_m3_crc_code)/sizeof(u16)); i++)
		target_write_u16(target, crc_algorithm->address + i*sizeof(u16), cortex_m3_crc_code[i]);
	
	armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
	armv7m_info.core_mode = ARMV7M_MODE_ANY;
	
	init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	
	buf_set_u32(reg_params[0].value, 0, 32, address);
	buf_set_u32(reg_params[1].value, 0, 32, count);
		
	if ((retval = target->type->run_algorithm(target, 0, NULL, 2, reg_params,
		crc_algorithm->address, crc_algorithm->address + (sizeof(cortex_m3_crc_code)-6), 20000, &armv7m_info)) != ERROR_OK)
	{
		LOG_ERROR("error executing cortex_m3 crc algorithm");
		destroy_reg_param(&reg_params[0]);
		destroy_reg_param(&reg_params[1]);
		target_free_working_area(target, crc_algorithm);
		return retval;
	}
	
	*checksum = buf_get_u32(reg_params[0].value, 0, 32);
	
	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	
	target_free_working_area(target, crc_algorithm);
	
	return ERROR_OK;
}


