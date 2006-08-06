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

#include "replacements.h"

#include "cfi.h"

#include "flash.h"
#include "target.h"
#include "log.h"
#include "armv4_5.h"
#include "algorithm.h"
#include "binarybuffer.h"
#include "types.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int cfi_register_commands(struct command_context_s *cmd_ctx);
int cfi_flash_bank_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc, struct flash_bank_s *bank);
int cfi_erase(struct flash_bank_s *bank, int first, int last);
int cfi_protect(struct flash_bank_s *bank, int set, int first, int last);
int cfi_write(struct flash_bank_s *bank, u8 *buffer, u32 offset, u32 count);
int cfi_probe(struct flash_bank_s *bank);
int cfi_erase_check(struct flash_bank_s *bank);
int cfi_protect_check(struct flash_bank_s *bank);
int cfi_info(struct flash_bank_s *bank, char *buf, int buf_size);

int cfi_handle_part_id_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc);

#define CFI_MAX_BUS_WIDTH	4
#define CFI_MAX_CHIP_WIDTH	4

flash_driver_t cfi_flash =
{
	.name = "cfi",
	.register_commands = cfi_register_commands,
	.flash_bank_command = cfi_flash_bank_command,
	.erase = cfi_erase,
	.protect = cfi_protect,
	.write = cfi_write,
	.probe = cfi_probe,
	.erase_check = cfi_erase_check,
	.protect_check = cfi_protect_check,
	.info = cfi_info
};

inline u32 flash_address(flash_bank_t *bank, int sector, u32 offset)
{
	/* while the sector list isn't built, only accesses to sector 0 work */
	if (sector == 0)
		return bank->base + offset * bank->bus_width;
	else
	{
		if (!bank->sectors)
		{
			ERROR("BUG: sector list not yet built");
			exit(-1);
		}
		return bank->base + bank->sectors[sector].offset + offset * bank->bus_width;
	}

}

void cfi_command(flash_bank_t *bank, u8 cmd, u8 *cmd_buf)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	int i;
	
	if (cfi_info->target->endianness == TARGET_LITTLE_ENDIAN)
	{
		for (i = bank->bus_width; i > 0; i--)
		{
			*cmd_buf++ = (i & (bank->chip_width - 1)) ? 0x0 : cmd;
		}
	}
	else
	{
		for (i = 1; i <= bank->bus_width; i++)
		{
			*cmd_buf++ = (i & (bank->chip_width - 1)) ? 0x0 : cmd;
		}
	}
}

/* read unsigned 8-bit value from the bank
 * flash banks are expected to be made of similar chips
 * the query result should be the same for all
 */
u8 cfi_query_u8(flash_bank_t *bank, int sector, u32 offset)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	u8 data[CFI_MAX_BUS_WIDTH];
		
	target->type->read_memory(target, flash_address(bank, sector, offset), bank->bus_width, 1, data);
	
	if (cfi_info->target->endianness == TARGET_LITTLE_ENDIAN)
		return data[0];
	else
		return data[bank->bus_width - 1];
}

/* read unsigned 8-bit value from the bank
 * in case of a bank made of multiple chips,
 * the individual values are ORed
 */
u8 cfi_get_u8(flash_bank_t *bank, int sector, u32 offset)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	u8 data[CFI_MAX_BUS_WIDTH];
	int i;
	
	target->type->read_memory(target, flash_address(bank, sector, offset), bank->bus_width, 1, data);
	
	if (cfi_info->target->endianness == TARGET_LITTLE_ENDIAN)
	{
		for (i = 0; i < bank->bus_width / bank->chip_width; i++)
			data[0] |= data[i];
		
		return data[0];
	}
	else
	{
		u8 value = 0;
		for (i = 0; i < bank->bus_width / bank->chip_width; i++)
			value |= data[bank->bus_width - 1 - i];
		
		return value;
	}
}

u16 cfi_query_u16(flash_bank_t *bank, int sector, u32 offset)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	u8 data[CFI_MAX_BUS_WIDTH * 2];
	
	target->type->read_memory(target, flash_address(bank, sector, offset), bank->bus_width, 2, data);

	if (cfi_info->target->endianness == TARGET_LITTLE_ENDIAN)
		return data[0] | data[bank->bus_width] << 8;
	else
		return data[bank->bus_width - 1] | data[(2 * bank->bus_width) - 1] << 8;
}

u32 cfi_query_u32(flash_bank_t *bank, int sector, u32 offset)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	u8 data[CFI_MAX_BUS_WIDTH * 4];
	
	target->type->read_memory(target, flash_address(bank, sector, offset), bank->bus_width, 4, data);
	
	if (cfi_info->target->endianness == TARGET_LITTLE_ENDIAN)
		return data[0] | data[bank->bus_width] << 8 | data[bank->bus_width * 2] << 16 | data[bank->bus_width * 3] << 24;
	else
		return data[bank->bus_width - 1] | data[(2* bank->bus_width) - 1] << 8 | 
				data[(3 * bank->bus_width) - 1] << 16 | data[(4 * bank->bus_width) - 1] << 24;
}

void cfi_intel_clear_status_register(flash_bank_t *bank)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	u8 command[8];
	
	if (target->state != TARGET_HALTED)
	{
		ERROR("BUG: attempted to clear status register while target wasn't halted");
		exit(-1);
	}
	
	cfi_command(bank, 0x50, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
}

u8 cfi_intel_wait_status_busy(flash_bank_t *bank, int timeout)
{
	u8 status;
	
	while ((!((status = cfi_get_u8(bank, 0, 0x0)) & 0x80)) && (timeout-- > 0))
	{
		DEBUG("status: 0x%x", status);
		usleep(1000);
	}
	
	DEBUG("status: 0x%x", status);

	if (status != 0x80)
	{
		ERROR("status register: 0x%x", status);
		if (status & 0x2)
			ERROR("Block Lock-Bit Detected, Operation Abort");
		if (status & 0x4)
			ERROR("Program suspended");
		if (status & 0x8)
			ERROR("Low Programming Voltage Detected, Operation Aborted");
		if (status & 0x10)
			ERROR("Program Error / Error in Setting Lock-Bit");
		if (status & 0x20)
			ERROR("Error in Block Erasure or Clear Lock-Bits");
		if (status & 0x40)
			ERROR("Block Erase Suspended");
		
		cfi_intel_clear_status_register(bank);
	}
	
	return status;
}
int cfi_read_intel_pri_ext(flash_bank_t *bank)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	cfi_intel_pri_ext_t *pri_ext = malloc(sizeof(cfi_intel_pri_ext_t));
	target_t *target = cfi_info->target;
	u8 command[8];

	cfi_info->pri_ext = pri_ext;
	
	pri_ext->pri[0] = cfi_query_u8(bank, 0, cfi_info->pri_addr + 0);
	pri_ext->pri[1] = cfi_query_u8(bank, 0, cfi_info->pri_addr + 1);
	pri_ext->pri[2] = cfi_query_u8(bank, 0, cfi_info->pri_addr + 2);
	
	if ((pri_ext->pri[0] != 'P') || (pri_ext->pri[1] != 'R') || (pri_ext->pri[2] != 'I'))
	{
		cfi_command(bank, 0xf0, command);
		target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
		cfi_command(bank, 0xff, command);
		target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
		return ERROR_FLASH_BANK_INVALID;
	}
	
	pri_ext->major_version = cfi_query_u8(bank, 0, cfi_info->pri_addr + 3);
	pri_ext->minor_version = cfi_query_u8(bank, 0, cfi_info->pri_addr + 4);
	
	DEBUG("pri: '%c%c%c', version: %c.%c", pri_ext->pri[0], pri_ext->pri[1], pri_ext->pri[2], pri_ext->major_version, pri_ext->minor_version);
	
	pri_ext->feature_support = cfi_query_u32(bank, 0, cfi_info->pri_addr + 5);
	pri_ext->suspend_cmd_support = cfi_query_u8(bank, 0, cfi_info->pri_addr + 9);
	pri_ext->blk_status_reg_mask = cfi_query_u16(bank, 0, cfi_info->pri_addr + 0xa);
	
	DEBUG("feature_support: 0x%x, suspend_cmd_support: 0x%x, blk_status_reg_mask: 0x%x", pri_ext->feature_support, pri_ext->suspend_cmd_support, pri_ext->blk_status_reg_mask);
	
	pri_ext->vcc_optimal = cfi_query_u8(bank, 0, cfi_info->pri_addr + 0xc);
	pri_ext->vpp_optimal = cfi_query_u8(bank, 0, cfi_info->pri_addr + 0xd);
	
	DEBUG("Vcc opt: %1.1x.%1.1x, Vpp opt: %1.1x.%1.1x",
		  (pri_ext->vcc_optimal & 0xf0) >> 4, pri_ext->vcc_optimal & 0x0f,
		  (pri_ext->vpp_optimal & 0xf0) >> 4, pri_ext->vpp_optimal & 0x0f);
	
	pri_ext->num_protection_fields = cfi_query_u8(bank, 0, cfi_info->pri_addr + 0xe);
	if (pri_ext->num_protection_fields != 1)
	{
		WARNING("expected one protection register field, but found %i", pri_ext->num_protection_fields);
	}
	
	pri_ext->prot_reg_addr = cfi_query_u16(bank, 0, cfi_info->pri_addr + 0xf);
	pri_ext->fact_prot_reg_size = cfi_query_u8(bank, 0, cfi_info->pri_addr + 0x11);
	pri_ext->user_prot_reg_size = cfi_query_u8(bank, 0, cfi_info->pri_addr + 0x12);

	DEBUG("protection_fields: %i, prot_reg_addr: 0x%x, factory pre-programmed: %i, user programmable: %i", pri_ext->num_protection_fields, pri_ext->prot_reg_addr, 1 << pri_ext->fact_prot_reg_size, 1 << pri_ext->user_prot_reg_size);
	
	return ERROR_OK;
}

int cfi_intel_info(struct flash_bank_s *bank, char *buf, int buf_size)
{
	int printed;
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	cfi_intel_pri_ext_t *pri_ext = cfi_info->pri_ext;
		
	printed = snprintf(buf, buf_size, "\nintel primary algorithm extend information:\n");
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "pri: '%c%c%c', version: %c.%c\n", pri_ext->pri[0], pri_ext->pri[1], pri_ext->pri[2], pri_ext->major_version, pri_ext->minor_version);
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "feature_support: 0x%x, suspend_cmd_support: 0x%x, blk_status_reg_mask: 0x%x\n", pri_ext->feature_support, pri_ext->suspend_cmd_support, pri_ext->blk_status_reg_mask);
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "Vcc opt: %1.1x.%1.1x, Vpp opt: %1.1x.%1.1x\n",
		(pri_ext->vcc_optimal & 0xf0) >> 4, pri_ext->vcc_optimal & 0x0f,
		(pri_ext->vpp_optimal & 0xf0) >> 4, pri_ext->vpp_optimal & 0x0f);
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "protection_fields: %i, prot_reg_addr: 0x%x, factory pre-programmed: %i, user programmable: %i\n", pri_ext->num_protection_fields, pri_ext->prot_reg_addr, 1 << pri_ext->fact_prot_reg_size, 1 << pri_ext->user_prot_reg_size);
	
	return ERROR_OK;
}

int cfi_register_commands(struct command_context_s *cmd_ctx)
{
	command_t *cfi_cmd = register_command(cmd_ctx, NULL, "cfi", NULL, COMMAND_ANY, NULL);
	/*	
	register_command(cmd_ctx, cfi_cmd, "part_id", cfi_handle_part_id_command, COMMAND_EXEC,
					 "print part id of cfi flash bank <num>");
	*/
	return ERROR_OK;
}

/* flash_bank cfi <base> <size> <chip_width> <bus_width> <target#>
 */
int cfi_flash_bank_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc, struct flash_bank_s *bank)
{
	cfi_flash_bank_t *cfi_info;
	
	if (argc < 6)
	{
		WARNING("incomplete flash_bank cfi configuration");
		return ERROR_FLASH_BANK_INVALID;
	}
	
	if ((strtoul(args[4], NULL, 0) > CFI_MAX_CHIP_WIDTH)
		|| (strtoul(args[3], NULL, 0) > CFI_MAX_BUS_WIDTH))
	{
		ERROR("chip and bus width have to specified in byte");
		return ERROR_FLASH_BANK_INVALID;
	}
	
	cfi_info = malloc(sizeof(cfi_flash_bank_t));
	bank->driver_priv = cfi_info;
	
	cfi_info->target = get_target_by_num(strtoul(args[5], NULL, 0));
	if (!cfi_info->target)
	{
		ERROR("no target '%i' configured", args[5]);
		exit(-1);
	}

	/* bank wasn't probed yet */
	cfi_info->qry[0] = -1;
	
	return ERROR_OK;
}

int cfi_intel_erase(struct flash_bank_s *bank, int first, int last)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	cfi_intel_pri_ext_t *pri_ext = cfi_info->pri_ext;
	target_t *target = cfi_info->target;
	u8 command[8];
	int i;
	
	cfi_intel_clear_status_register(bank);

	for (i = first; i <= last; i++)
	{
		cfi_command(bank, 0x20, command);
		target->type->write_memory(target, flash_address(bank, i, 0x0), bank->bus_width, 1, command);
			
		cfi_command(bank, 0xd0, command);
		target->type->write_memory(target, flash_address(bank, i, 0x0), bank->bus_width, 1, command);
		
		if (cfi_intel_wait_status_busy(bank, 1000 * (1 << cfi_info->block_erase_timeout_typ)) == 0x80)
			bank->sectors[i].is_erased = 1;
		else
		{
			cfi_command(bank, 0xff, command);
			target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
			
			ERROR("couldn't erase block %i of flash bank at base 0x%x", i, bank->base);
			return ERROR_FLASH_OPERATION_FAILED;
		}
	}
	
	cfi_command(bank, 0xff, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
	
	return ERROR_OK;
}
	
int cfi_erase(struct flash_bank_s *bank, int first, int last)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	
	if (cfi_info->target->state != TARGET_HALTED)
	{
		return ERROR_TARGET_NOT_HALTED;
	}
	
	if ((first < 0) || (last < first) || (last >= bank->num_sectors))
	{
		return ERROR_FLASH_SECTOR_INVALID;
	}

	if (cfi_info->qry[0] != 'Q')
		return ERROR_FLASH_BANK_NOT_PROBED;
	
	switch(cfi_info->pri_id)
	{
		case 1:
		case 3:
			return cfi_intel_erase(bank, first, last);
			break;
		default:
			ERROR("cfi primary command set %i unsupported", cfi_info->pri_id);
			break;
	}
	
	return ERROR_OK;
}

int cfi_intel_protect(struct flash_bank_s *bank, int set, int first, int last)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	cfi_intel_pri_ext_t *pri_ext = cfi_info->pri_ext;
	target_t *target = cfi_info->target;
	u8 command[8];
	int i;
	
	if (!(pri_ext->feature_support & 0x28))
		return ERROR_FLASH_OPERATION_FAILED;
	
	cfi_intel_clear_status_register(bank);

	for (i = first; i <= last; i++)
	{
		cfi_command(bank, 0x60, command);
		target->type->write_memory(target, flash_address(bank, i, 0x0), bank->bus_width, 1, command);
		if (set)
		{
			cfi_command(bank, 0x01, command);
			target->type->write_memory(target, flash_address(bank, i, 0x0), bank->bus_width, 1, command);
			bank->sectors[i].is_protected = 1;
		}
		else
		{
			cfi_command(bank, 0xd0, command);
			target->type->write_memory(target, flash_address(bank, i, 0x0), bank->bus_width, 1, command);
			bank->sectors[i].is_protected = 0;
		}
		
		cfi_intel_wait_status_busy(bank, 100);
	}
	
	/* if the device doesn't support individual block lock bits set/clear,
	 * all blocks have been unlocked in parallel, so we set those that should be protected
	 */
	if ((!set) && (!(pri_ext->feature_support & 0x20)))
	{
		for (i = 0; i < bank->num_sectors; i++)
		{
			cfi_intel_clear_status_register(bank);
			cfi_command(bank, 0x60, command);
			target->type->write_memory(target, flash_address(bank, i, 0x0), bank->bus_width, 1, command);
			if (bank->sectors[i].is_protected == 1)
			{
				cfi_command(bank, 0x01, command);
				target->type->write_memory(target, flash_address(bank, i, 0x0), bank->bus_width, 1, command);
			}
			
			cfi_intel_wait_status_busy(bank, 100);
		}
	}
	
	cfi_command(bank, 0xff, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
	
	return ERROR_OK;
}

int cfi_protect(struct flash_bank_s *bank, int set, int first, int last)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	
	if (cfi_info->target->state != TARGET_HALTED)
	{
		return ERROR_TARGET_NOT_HALTED;
	}
	
	if ((first < 0) || (last < first) || (last >= bank->num_sectors))
	{
		return ERROR_FLASH_SECTOR_INVALID;
	}
	
	if (cfi_info->qry[0] != 'Q')
		return ERROR_FLASH_BANK_NOT_PROBED;
	
	switch(cfi_info->pri_id)
	{
		case 1:
		case 3:
			cfi_intel_protect(bank, set, first, last);
			break;
		default:
			ERROR("cfi primary command set %i unsupported", cfi_info->pri_id);
			break;
	}
	
	return ERROR_OK;
}

void cfi_add_byte(struct flash_bank_s *bank, u8 *word, u8 byte)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;

	int i;
	
	if (target->endianness == TARGET_LITTLE_ENDIAN)
	{
		/* shift bytes */
		for (i = 0; i < bank->bus_width - 1; i++)
			word[i] = word[i + 1];
		word[bank->bus_width - 1] = byte;
	}
	else
	{
		/* shift bytes */
		for (i = bank->bus_width - 1; i > 0; i--)
			word[i] = word[i - 1];
		word[0] = byte;
	}
}

int cfi_intel_write_block(struct flash_bank_s *bank, u8 *buffer, u32 address, u32 count)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	cfi_intel_pri_ext_t *pri_ext = cfi_info->pri_ext;
	target_t *target = cfi_info->target;
	reg_param_t reg_params[5];
	armv4_5_algorithm_t armv4_5_info;
	working_area_t *source;
	u32 buffer_size = 32768;
	u8 write_command[CFI_MAX_BUS_WIDTH];
	int i;
	int retval;
	
	u32 word_32_code[] = {
		0xe4904004,   /* loop:	ldr r4, [r0], #4 */
		0xe5813000,   /* 		str r3, [r1] */
		0xe5814000,   /* 		str r4, [r1] */
		0xe5914000,   /* busy	ldr r4, [r1] */
		0xe3140080,   /*		tst r4, #0x80 */
		0x0afffffc,   /*		beq busy */
		0xe314007f,   /* 		tst r4, #0x7f */
		0x1a000003,   /*		bne done */
		0xe2522001,   /*		subs r2, r2, #1 */
		0x0a000001,   /* 		beq done */
		0xe2811004,   /*		add r1, r1 #4 */
		0xeafffff3,   /* 		b loop */
		0xeafffffe,   /* done:	b -2 */
	};
	
	u32 word_16_code[] = {
		0xe0d040b2,   /* loop:	ldrh r4, [r0], #2 */
		0xe1c130b0,   /* 		strh r3, [r1] */
		0xe1c140b0,   /* 		strh r4, [r1] */
		0xe1d140b0,   /* busy	ldrh r4, [r1] */
		0xe3140080,   /*		tst r4, #0x80 */
		0x0afffffc,   /*		beq busy */
		0xe314007f,   /* 		tst r4, #0x7f */
		0x1a000003,   /*		bne done */
		0xe2522001,   /*		subs r2, r2, #1 */
		0x0a000001,   /* 		beq done */
		0xe2811002,   /*		add r1, r1 #2 */
		0xeafffff3,   /* 		b loop */
		0xeafffffe,   /* done:	b -2 */
	};
	
	u32 word_8_code[] = {
		0xe4d04001,   /* loop:	ldrb r4, [r0], #1 */
		0xe5c13000,   /* 		strb r3, [r1] */
		0xe5c14000,   /* 		strb r4, [r1] */
		0xe5d14000,   /* busy	ldrb r4, [r1] */
		0xe3140080,   /*		tst r4, #0x80 */
		0x0afffffc,   /*		beq busy */
		0xe314007f,   /* 		tst r4, #0x7f */
		0x1a000003,   /*		bne done */
		0xe2522001,   /*		subs r2, r2, #1 */
		0x0a000001,   /* 		beq done */
		0xe2811001,   /*		add r1, r1 #1 */
		0xeafffff3,   /* 		b loop */
		0xeafffffe,   /* done:	b -2 */
	};
	
	cfi_intel_clear_status_register(bank);
		
	armv4_5_info.common_magic = ARMV4_5_COMMON_MAGIC;
	armv4_5_info.core_mode = ARMV4_5_MODE_SVC;
	armv4_5_info.core_state = ARMV4_5_STATE_ARM;
			
	/* flash write code */
	if (!cfi_info->write_algorithm)
	{
		if (target_alloc_working_area(target, 4 * 13, &cfi_info->write_algorithm) != ERROR_OK)
		{
			WARNING("no working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		};
			
		/* write algorithm code to working area */
		if (bank->bus_width == 1)
		{
			target_write_buffer(target, cfi_info->write_algorithm->address, 13 * 4, (u8*)word_8_code);
		}
		else if (bank->bus_width == 2)
		{
			target_write_buffer(target, cfi_info->write_algorithm->address, 13 * 4, (u8*)word_16_code);
		}	
		else if (bank->bus_width == 4)
		{
			target_write_buffer(target, cfi_info->write_algorithm->address, 13 * 4, (u8*)word_32_code);
		}
		else
		{
			return ERROR_FLASH_OPERATION_FAILED;
		}
	}
	
	while (target_alloc_working_area(target, buffer_size, &source) != ERROR_OK)
	{
		buffer_size /= 2;
		if (buffer_size <= 256)
		{
			/* if we already allocated the writing code, but failed to get a buffer, free the algorithm */
			if (cfi_info->write_algorithm)
				target_free_working_area(target, cfi_info->write_algorithm);
			
			WARNING("no large enough working area available, can't do block memory writes");
			return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
		}
	};

	init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
	init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
	init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);
	init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);
	init_reg_param(&reg_params[4], "r4", 32, PARAM_IN);

	while (count > 0)
	{
		u32 thisrun_count = (count > buffer_size) ? buffer_size : count;
		
		target_write_buffer(target, source->address, thisrun_count, buffer);
		
		buf_set_u32(reg_params[0].value, 0, 32, source->address);
		buf_set_u32(reg_params[1].value, 0, 32, address);
		buf_set_u32(reg_params[2].value, 0, 32, thisrun_count / bank->bus_width);
		cfi_command(bank, 0x40, write_command);
		buf_set_u32(reg_params[3].value, 0, 32, buf_get_u32(write_command, 0, 32));
	
		if ((retval = target->type->run_algorithm(target, 0, NULL, 5, reg_params, cfi_info->write_algorithm->address, cfi_info->write_algorithm->address + (12 * 4), 10000, &armv4_5_info)) != ERROR_OK)
		{
			cfi_intel_clear_status_register(bank);
			return ERROR_FLASH_OPERATION_FAILED;
		}
	
		if (buf_get_u32(reg_params[4].value, 0, 32) != 0x80)
		{
			/* read status register (outputs debug inforation) */
			cfi_intel_wait_status_busy(bank, 100);
			cfi_intel_clear_status_register(bank);
			return ERROR_FLASH_OPERATION_FAILED;
		}
		
		buffer += thisrun_count;
		address += thisrun_count;
		count -= thisrun_count;
	}
	
	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);

	return ERROR_OK;
}

int cfi_intel_write_word(struct flash_bank_s *bank, u8 *word, u32 address)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	cfi_intel_pri_ext_t *pri_ext = cfi_info->pri_ext;
	target_t *target = cfi_info->target;
	u8 command[8];
	
	cfi_intel_clear_status_register(bank);
	cfi_command(bank, 0x40, command);
	target->type->write_memory(target, address, bank->bus_width, 1, command);
	
	target->type->write_memory(target, address, bank->bus_width, 1, word);
	
	if (cfi_intel_wait_status_busy(bank, 1000 * (1 << cfi_info->word_write_timeout_max)) != 0x80)
	{
		cfi_command(bank, 0xff, command);
		target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
		
		ERROR("couldn't write word at base 0x%x, address %x", bank->base, address);
		return ERROR_FLASH_OPERATION_FAILED;
	}
	
	return ERROR_OK;
}

int cfi_write_word(struct flash_bank_s *bank, u8 *word, u32 address)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	
	switch(cfi_info->pri_id)
	{
		case 1:
		case 3:
			return cfi_intel_write_word(bank, word, address);
			break;
		default:
			ERROR("cfi primary command set %i unsupported", cfi_info->pri_id);
			break;
	}
	
	return ERROR_FLASH_OPERATION_FAILED;
}

int cfi_write(struct flash_bank_s *bank, u8 *buffer, u32 offset, u32 count)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	u32 address = bank->base + offset;	/* address of first byte to be programmed */
	u32 write_p, copy_p;
	int align;	/* number of unaligned bytes */
	u8 current_word[CFI_MAX_BUS_WIDTH * 4];	/* word (bus_width size) currently being programmed */
	int i;
	int retval;
	
	if (cfi_info->target->state != TARGET_HALTED)
	{
		return ERROR_TARGET_NOT_HALTED;
	}
	
	if (offset + count > bank->size)
		return ERROR_FLASH_DST_OUT_OF_BANK;
	
	if (cfi_info->qry[0] != 'Q')
		return ERROR_FLASH_BANK_NOT_PROBED;

	/* start at the first byte of the first word (bus_width size) */
	write_p = address & ~(bank->bus_width - 1);
	if ((align = address - write_p) != 0)
	{
		for (i = 0; i < bank->bus_width; i++)
			current_word[i] = 0;
		copy_p = write_p;

		/* copy bytes before the first write address */
		for (i = 0; i < align; ++i, ++copy_p)
		{
			u8 byte;
			target->type->read_memory(target, copy_p, 1, 1, &byte);
			cfi_add_byte(bank, current_word, byte);
		}

		/* add bytes from the buffer */
		for (; (i < bank->bus_width) && (count > 0); i++)
		{
			cfi_add_byte(bank, current_word, *buffer++);
			count--;
			copy_p++;
		}

		/* if the buffer is already finished, copy bytes after the last write address */
		for (; (count == 0) && (i < bank->bus_width); ++i, ++copy_p)
		{
			u8 byte;
			target->type->read_memory(target, copy_p, 1, 1, &byte);
			cfi_add_byte(bank, current_word, byte);
		}
		
		retval = cfi_write_word(bank, current_word, write_p);
		if (retval != ERROR_OK)
			return retval;
		write_p = copy_p;
	}
	
	/* handle blocks of bus_size aligned bytes */
	switch(cfi_info->pri_id)
	{
		/* try block writes (fails without working area) */
		case 1:
		case 3:
			retval = cfi_intel_write_block(bank, buffer, write_p, count);
			break;
		default:
			ERROR("cfi primary command set %i unsupported", cfi_info->pri_id);
			break;
	}
	if (retval != ERROR_OK)
	{
		if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE)
		{
			/* fall back to memory writes */
			while (count > bank->bus_width)
			{
				for (i = 0; i < bank->bus_width; i++)
					current_word[i] = 0;
			
				for (i = 0; i < bank->bus_width; i++)
				{
					cfi_add_byte(bank, current_word, *buffer++);
				}
			
				retval = cfi_write_word(bank, current_word, write_p);
				if (retval != ERROR_OK)
					return retval;
				write_p += bank->bus_width;
				count -= bank->bus_width;
			}
		}
		else
			return retval;
	}
	
	/* handle unaligned tail bytes */
	if (count > 0)
	{
		copy_p = write_p;
		for (i = 0; i < bank->bus_width; i++)
			current_word[i] = 0;
		
		for (i = 0; (i < bank->bus_width) && (count > 0); ++i, ++copy_p)
		{
			cfi_add_byte(bank, current_word, *buffer++);
			count--;
		}
		for (; i < bank->bus_width; ++i, ++copy_p)
		{
			u8 byte;
			target->type->read_memory(target, copy_p, 1, 1, &byte);
			cfi_add_byte(bank, current_word, byte);
		}
		retval = cfi_write_word(bank, current_word, write_p);
		if (retval != ERROR_OK)
			return retval;
	}
	
	/* return to read array mode */
	cfi_command(bank, 0xf0, current_word);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, current_word);
	cfi_command(bank, 0xff, current_word);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, current_word);
	
	return ERROR_OK;
}

int cfi_probe(struct flash_bank_s *bank)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	u8 command[8];
	

	cfi_command(bank, 0x98, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x55), bank->bus_width, 1, command);
	
	cfi_info->qry[0] = cfi_query_u8(bank, 0, 0x10);
	cfi_info->qry[1] = cfi_query_u8(bank, 0, 0x11);
	cfi_info->qry[2] = cfi_query_u8(bank, 0, 0x12);
	
	if ((cfi_info->qry[0] != 'Q') || (cfi_info->qry[1] != 'R') || (cfi_info->qry[2] != 'Y'))
	{
		cfi_command(bank, 0xf0, command);
		target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
		cfi_command(bank, 0xff, command);
		target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
		return ERROR_FLASH_BANK_INVALID;
	}
	
	cfi_info->pri_id = cfi_query_u16(bank, 0, 0x13);
	cfi_info->pri_addr = cfi_query_u16(bank, 0, 0x15);
	cfi_info->alt_id = cfi_query_u16(bank, 0, 0x17);
	cfi_info->alt_addr = cfi_query_u16(bank, 0, 0x19);
	
	DEBUG("qry: '%c%c%c', pri_id: 0x%4.4x, pri_addr: 0x%4.4x, alt_id: 0x%4.4x, alt_addr: 0x%4.4x", cfi_info->qry[0], cfi_info->qry[1], cfi_info->qry[2], cfi_info->pri_id, cfi_info->pri_addr, cfi_info->alt_id, cfi_info->alt_addr);
	
	cfi_info->vcc_min = cfi_query_u8(bank, 0, 0x1b);
	cfi_info->vcc_max = cfi_query_u8(bank, 0, 0x1c);
	cfi_info->vpp_min = cfi_query_u8(bank, 0, 0x1d);
	cfi_info->vpp_max = cfi_query_u8(bank, 0, 0x1e);
	cfi_info->word_write_timeout_typ = cfi_query_u8(bank, 0, 0x1f);
	cfi_info->buf_write_timeout_typ = cfi_query_u8(bank, 0, 0x20);
	cfi_info->block_erase_timeout_typ = cfi_query_u8(bank, 0, 0x21);
	cfi_info->chip_erase_timeout_typ = cfi_query_u8(bank, 0, 0x22);
	cfi_info->word_write_timeout_max = cfi_query_u8(bank, 0, 0x23);
	cfi_info->buf_write_timeout_max = cfi_query_u8(bank, 0, 0x24);
	cfi_info->block_erase_timeout_max = cfi_query_u8(bank, 0, 0x25);
	cfi_info->chip_erase_timeout_max = cfi_query_u8(bank, 0, 0x26);
	
	DEBUG("Vcc min: %1.1x.%1.1x, Vcc max: %1.1x.%1.1x, Vpp min: %1.1x.%1.1x, Vpp max: %1.1x.%1.1x",
		(cfi_info->vcc_min & 0xf0) >> 4, cfi_info->vcc_min & 0x0f,
		(cfi_info->vcc_max & 0xf0) >> 4, cfi_info->vcc_max & 0x0f,
		(cfi_info->vpp_min & 0xf0) >> 4, cfi_info->vpp_min & 0x0f,
		(cfi_info->vpp_max & 0xf0) >> 4, cfi_info->vpp_max & 0x0f);
	DEBUG("typ. word write timeout: %u, typ. buf write timeout: %u, typ. block erase timeout: %u, typ. chip erase timeout: %u", 1 << cfi_info->word_write_timeout_typ, 1 << cfi_info->buf_write_timeout_typ,
		1 << cfi_info->block_erase_timeout_typ, 1 << cfi_info->chip_erase_timeout_typ);
	DEBUG("max. word write timeout: %u, max. buf write timeout: %u, max. block erase timeout: %u, max. chip erase timeout: %u", (1 << cfi_info->word_write_timeout_max) * (1 << cfi_info->word_write_timeout_typ),
		(1 << cfi_info->buf_write_timeout_max) * (1 << cfi_info->buf_write_timeout_typ),
		(1 << cfi_info->block_erase_timeout_max) * (1 << cfi_info->block_erase_timeout_typ),
		(1 << cfi_info->chip_erase_timeout_max) * (1 << cfi_info->chip_erase_timeout_typ));
	
	cfi_info->dev_size = cfi_query_u8(bank, 0, 0x27);
	cfi_info->interface_desc = cfi_query_u16(bank, 0, 0x28);
	cfi_info->max_buf_write_size = cfi_query_u16(bank, 0, 0x2a);
	cfi_info->num_erase_regions = cfi_query_u8(bank, 0, 0x2c);
	
	DEBUG("size: 0x%x, interface desc: %i, max buffer write size: %x", 1 << cfi_info->dev_size, cfi_info->interface_desc, cfi_info->max_buf_write_size);
	
	if (1 << cfi_info->dev_size != bank->size)
	{
		WARNING("configuration specifies 0x%x size, but a 0x%x size flash was found", bank->size, 1 << cfi_info->dev_size);
	}
	
	if (cfi_info->num_erase_regions)
	{
		int i;
		int num_sectors = 0;
		int sector = 0;
		u32 offset = 0;
		cfi_info->erase_region_info = malloc(4 * cfi_info->num_erase_regions);
		
		for (i = 0; i < cfi_info->num_erase_regions; i++)
		{
			cfi_info->erase_region_info[i] = cfi_query_u32(bank, 0, 0x2d + (4 * i));
			DEBUG("erase region[%i]: %i blocks of size 0x%x", i, (cfi_info->erase_region_info[i] & 0xffff) + 1, (cfi_info->erase_region_info[i] >> 16) * 256);
			
			num_sectors += (cfi_info->erase_region_info[i] & 0xffff) + 1;
		}
		
		bank->num_sectors = num_sectors;
		bank->sectors = malloc(sizeof(flash_sector_t) * num_sectors);
		for (i = 0; i < cfi_info->num_erase_regions; i++)
		{
			int j;
			for (j = 0; j < (cfi_info->erase_region_info[i] & 0xffff) + 1; j++)
			{
				bank->sectors[sector].offset = offset;
				bank->sectors[sector].size = (cfi_info->erase_region_info[i] >> 16) * 256;
				offset += bank->sectors[sector].size;
				bank->sectors[sector].is_erased = -1;
				bank->sectors[sector].is_protected = -1;
				sector++;
			}
		}
	}
	else
	{
		cfi_info->erase_region_info = NULL;
	}
	
	switch(cfi_info->pri_id)
	{
		case 1:
		case 3:
			cfi_read_intel_pri_ext(bank);
			break;
		default:
			ERROR("cfi primary command set %i unsupported", cfi_info->pri_id);
			break;
	}
	
	/* return to read array mode */
	cfi_command(bank, 0xf0, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
	cfi_command(bank, 0xff, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);
	
	return ERROR_OK;
}

int cfi_erase_check(struct flash_bank_s *bank)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;
	int i;
	int retval;
	
	if (!cfi_info->erase_check_algorithm)
	{
		u32 erase_check_code[] =
		{
			0xe4d03001,
			0xe0022003,
			0xe2511001,
			0x1afffffb,
			0xeafffffe
		};
		
		/* make sure we have a working area */
		if (target_alloc_working_area(target, 20, &cfi_info->erase_check_algorithm) != ERROR_OK)
		{
			WARNING("no working area available, falling back to slow memory reads");
		}
		else
		{
			/* write algorithm code to working area */
			target->type->write_memory(target, cfi_info->erase_check_algorithm->address, 4, 5, (u8*)erase_check_code);
		}
	}
	
	if (!cfi_info->erase_check_algorithm)
	{
		u32 *buffer = malloc(4096);
		
		for (i = 0; i < bank->num_sectors; i++)
		{
			u32 address = bank->base + bank->sectors[i].offset;
			u32 size = bank->sectors[i].size;
			u32 check = 0xffffffffU;
			int erased = 1;
			
			while (size > 0)
			{
				u32 thisrun_size = (size > 4096) ? 4096 : size;
				int j;
				
				target->type->read_memory(target, address, 4, thisrun_size / 4, (u8*)buffer);
				
				for (j = 0; j < thisrun_size / 4; j++)
					check &= buffer[j];
				
				if (check != 0xffffffff)
				{
					erased = 0;
					break;
				}
				
				size -= thisrun_size;
				address += thisrun_size;
			}
			
			bank->sectors[i].is_erased = erased;
		}
		
		free(buffer);
	}
	else
	{	
		for (i = 0; i < bank->num_sectors; i++)
		{
			u32 address = bank->base + bank->sectors[i].offset;
			u32 size = bank->sectors[i].size;

			reg_param_t reg_params[3];
			armv4_5_algorithm_t armv4_5_info;
			
			armv4_5_info.common_magic = ARMV4_5_COMMON_MAGIC;
			armv4_5_info.core_mode = ARMV4_5_MODE_SVC;
			armv4_5_info.core_state = ARMV4_5_STATE_ARM;
			
			init_reg_param(&reg_params[0], "r0", 32, PARAM_OUT);
			buf_set_u32(reg_params[0].value, 0, 32, address);
			
			init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);
			buf_set_u32(reg_params[1].value, 0, 32, size);
			
			init_reg_param(&reg_params[2], "r2", 32, PARAM_IN_OUT);
			buf_set_u32(reg_params[2].value, 0, 32, 0xff);
			
			if ((retval = target->type->run_algorithm(target, 0, NULL, 3, reg_params, cfi_info->erase_check_algorithm->address, cfi_info->erase_check_algorithm->address + 0x10, 10000, &armv4_5_info)) != ERROR_OK)
				return ERROR_FLASH_OPERATION_FAILED;
			
			if (buf_get_u32(reg_params[2].value, 0, 32) == 0xff)
				bank->sectors[i].is_erased = 1;
			else
				bank->sectors[i].is_erased = 0;
			
			destroy_reg_param(&reg_params[0]);
			destroy_reg_param(&reg_params[1]);
			destroy_reg_param(&reg_params[2]);
		}
	}
	
	return ERROR_OK;
}

int cfi_intel_protect_check(struct flash_bank_s *bank)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	cfi_intel_pri_ext_t *pri_ext = cfi_info->pri_ext;
	target_t *target = cfi_info->target;
	u8 command[8];
	int i;
	
	/* check if block lock bits are supported on this device */
	if (!(pri_ext->blk_status_reg_mask & 0x1))
		return ERROR_FLASH_OPERATION_FAILED;

	cfi_command(bank, 0x90, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x55), bank->bus_width, 1, command);

	for (i = 0; i < bank->num_sectors; i++)
	{
		u8 block_status = cfi_get_u8(bank, i, 0x2);
		
		if (block_status & 1)
			bank->sectors[i].is_protected = 1;
		else
			bank->sectors[i].is_protected = 0;
	}

	cfi_command(bank, 0xff, command);
	target->type->write_memory(target, flash_address(bank, 0, 0x0), bank->bus_width, 1, command);

	return ERROR_OK;
}

int cfi_protect_check(struct flash_bank_s *bank)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	target_t *target = cfi_info->target;

	if (cfi_info->qry[0] != 'Q')
		return ERROR_FLASH_BANK_NOT_PROBED;
	
	switch(cfi_info->pri_id)
	{
		case 1:
		case 3:
			return cfi_intel_protect_check(bank);
			break;
		default:
			ERROR("cfi primary command set %i unsupported", cfi_info->pri_id);
			break;
	}
	
	return ERROR_OK;
}

int cfi_info(struct flash_bank_s *bank, char *buf, int buf_size)
{
	int printed;
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	
	if (cfi_info->qry[0] == -1)
	{
		printed = snprintf(buf, buf_size, "\ncfi flash bank not probed yet\n");
		return ERROR_OK;
	}
	
	printed = snprintf(buf, buf_size, "\ncfi information:\n");
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "qry: '%c%c%c', pri_id: 0x%4.4x, pri_addr: 0x%4.4x, alt_id: 0x%4.4x, alt_addr: 0x%4.4x\n", cfi_info->qry[0], cfi_info->qry[1], cfi_info->qry[2], cfi_info->pri_id, cfi_info->pri_addr, cfi_info->alt_id, cfi_info->alt_addr);
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "Vcc min: %1.1x.%1.1x, Vcc max: %1.1x.%1.1x, Vpp min: %1.1x.%1.1x, Vpp max: %1.1x.%1.1x\n", (cfi_info->vcc_min & 0xf0) >> 4, cfi_info->vcc_min & 0x0f,
	(cfi_info->vcc_max & 0xf0) >> 4, cfi_info->vcc_max & 0x0f,
	(cfi_info->vpp_min & 0xf0) >> 4, cfi_info->vpp_min & 0x0f,
	(cfi_info->vpp_max & 0xf0) >> 4, cfi_info->vpp_max & 0x0f);
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "typ. word write timeout: %u, typ. buf write timeout: %u, typ. block erase timeout: %u, typ. chip erase timeout: %u\n", 1 << cfi_info->word_write_timeout_typ, 1 << cfi_info->buf_write_timeout_typ,
		  1 << cfi_info->block_erase_timeout_typ, 1 << cfi_info->chip_erase_timeout_typ);
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "max. word write timeout: %u, max. buf write timeout: %u, max. block erase timeout: %u, max. chip erase timeout: %u\n", (1 << cfi_info->word_write_timeout_max) * (1 << cfi_info->word_write_timeout_typ),
		  (1 << cfi_info->buf_write_timeout_max) * (1 << cfi_info->buf_write_timeout_typ),
		  (1 << cfi_info->block_erase_timeout_max) * (1 << cfi_info->block_erase_timeout_typ),
		  (1 << cfi_info->chip_erase_timeout_max) * (1 << cfi_info->chip_erase_timeout_typ));
	buf += printed;
	buf_size -= printed;
	
	printed = snprintf(buf, buf_size, "size: 0x%x, interface desc: %i, max buffer write size: %x\n", 1 << cfi_info->dev_size, cfi_info->interface_desc, cfi_info->max_buf_write_size);
	buf += printed;
	buf_size -= printed;
	
	switch(cfi_info->pri_id)
	{
		case 1:
		case 3:
			cfi_intel_info(bank, buf, buf_size);
			break;
		default:
			ERROR("cfi primary command set %i unsupported", cfi_info->pri_id);
			break;
	}
	
	return ERROR_OK;
}
