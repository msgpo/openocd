/***************************************************************************
 *   Copyright (C) 2007 by Dominic Rath                                    *
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

#include <stdlib.h>

#include "log.h"

#include "flash.h"
#include "cfi.h"
#include "non_cfi.h"

/* non-CFI compatible flashes */
non_cfi_t non_cfi_flashes[] = {
	{
		.mfr = CFI_MFR_SST,
		.id = 0xd4,
		.pri_id = 0x02,
		.dev_size = 0x10,
		.interface_desc = 0x0,
		.max_buf_write_size = 0x0,
		.num_erase_regions = 1,
		.erase_region_info =
		{
			0x0010000f,
			0x00000000
		}
	},
	{
		.mfr = CFI_MFR_SST,
		.id = 0xd5,
		.pri_id = 0x02,
		.dev_size = 0x11,
		.interface_desc = 0x0,
		.max_buf_write_size = 0x0,
		.num_erase_regions = 1,
		.erase_region_info =
		{
			0x0010001f,
			0x00000000
		}
	},
	{
		.mfr = CFI_MFR_SST,
		.id = 0xd6,
		.pri_id = 0x02,
		.dev_size = 0x12,
		.interface_desc = 0x0,
		.max_buf_write_size = 0x0,
		.num_erase_regions = 1,
		.erase_region_info =
		{
			0x0010003f,
			0x00000000
		}
	},
	{
		.mfr = CFI_MFR_SST,
		.id = 0xd7,
		.pri_id = 0x02,
		.dev_size = 0x13,
		.interface_desc = 0x0,
		.max_buf_write_size = 0x0,
		.num_erase_regions = 1,
		.erase_region_info =
		{
			0x0010007f,
			0x00000000
		}
	},
	{
		.mfr = 0,
		.id = 0,
	}
};

void cfi_fixup_non_cfi(flash_bank_t *bank, void *param)
{
	cfi_flash_bank_t *cfi_info = bank->driver_priv;
	non_cfi_t *non_cfi = non_cfi_flashes;
	
	while (non_cfi->mfr)
	{
		if ((cfi_info->manufacturer == non_cfi->mfr)
			&& (cfi_info->device_id == non_cfi->id))
		{
			break;
		}
		non_cfi++;
	}
	
	cfi_info->not_cfi = 1;
	
	/* fill in defaults for non-critical data */
	cfi_info->vcc_min = 0x0;
	cfi_info->vcc_max = 0x0;
	cfi_info->vpp_min = 0x0;
	cfi_info->vpp_max = 0x0;
	cfi_info->word_write_timeout_typ = 0x0;
	cfi_info->buf_write_timeout_typ = 0x0;
	cfi_info->block_erase_timeout_typ = 0x0;
	cfi_info->chip_erase_timeout_typ = 0x0;
	cfi_info->word_write_timeout_max = 0x0;
	cfi_info->buf_write_timeout_max = 0x0;
	cfi_info->block_erase_timeout_max = 0x0;
	cfi_info->chip_erase_timeout_max = 0x0;
	
	cfi_info->qry[0] = 'Q';
	cfi_info->qry[1] = 'R';
	cfi_info->qry[2] = 'Y';
	
	cfi_info->pri_id = non_cfi->pri_id;
	cfi_info->pri_addr = 0x0;
	cfi_info->alt_id = 0x0;
	cfi_info->alt_addr = 0x0;
	cfi_info->alt_ext = NULL;
	
	cfi_info->interface_desc = non_cfi->interface_desc;
	cfi_info->max_buf_write_size = non_cfi->max_buf_write_size;
	cfi_info->num_erase_regions = non_cfi->num_erase_regions;
	cfi_info->erase_region_info = non_cfi->erase_region_info;
	
	if (cfi_info->pri_id == 0x2)
	{
		cfi_spansion_pri_ext_t *pri_ext = malloc(sizeof(cfi_spansion_pri_ext_t));

		pri_ext->pri[0] = 'P';
		pri_ext->pri[1] = 'R';
		pri_ext->pri[2] = 'I';
		
		pri_ext->major_version = '1';
		pri_ext->minor_version = '0';
		
		pri_ext->SiliconRevision = 0x0;
		pri_ext->EraseSuspend = 0x0;
		pri_ext->EraseSuspend = 0x0;
		pri_ext->BlkProt = 0x0;
		pri_ext->TmpBlkUnprotect = 0x0;
		pri_ext->BlkProtUnprot = 0x0;
		pri_ext->SimultaneousOps = 0x0;
		pri_ext->BurstMode = 0x0;
		pri_ext->PageMode = 0x0;
		pri_ext->VppMin = 0x0;
		pri_ext->VppMax = 0x0;
		pri_ext->TopBottom = 0x0;
	
		pri_ext->_reversed_geometry = 0;
		
		cfi_info->pri_ext = pri_ext;
	} else if ((cfi_info->pri_id == 0x1) || (cfi_info->pri_id == 0x3))
	{
		ERROR("BUG: non-CFI flashes using the Intel commandset are not yet supported");
		exit(-1);
	}
}
