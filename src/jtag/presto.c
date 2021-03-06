/***************************************************************************
 *   Copyright (C) 2007 by Pavel Chromy                                    *
 *   chromy@asix.cz                                                        *
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

#if IS_CYGWIN == 1
#include "windows.h"
#endif

#include "replacements.h"

/* project specific includes */
#include "log.h"
#include "types.h"
#include "jtag.h"
#include "configuration.h"
#include "time_support.h"
#include "bitq.h"

/* system includes */
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* PRESTO access library includes */
#if BUILD_PRESTO_FTD2XX == 1
#include <ftd2xx.h>
#elif BUILD_PRESTO_LIBFTDI == 1
#include <ftdi.h>
#else
#error "BUG: either FTD2XX and LIBFTDI has to be used"
#endif


int presto_jtag_speed(int speed);
int presto_jtag_register_commands(struct command_context_s *cmd_ctx);
int presto_jtag_init(void);
int presto_jtag_quit(void);

jtag_interface_t presto_interface =
{
	.name = "presto",
	.execute_queue = bitq_execute_queue,
	.speed = presto_jtag_speed,
	.register_commands = presto_jtag_register_commands,
	.init = presto_jtag_init,
	.quit = presto_jtag_quit,
};


int presto_bitq_out(int tms, int tdi, int tdo_req);
int presto_bitq_flush(void);
int presto_bitq_sleep(unsigned long us);
int presto_bitq_reset(int trst, int srst);
int presto_bitq_in_rdy(void);
int presto_bitq_in(void);

bitq_interface_t presto_bitq =
{
	.out = presto_bitq_out,
	.flush = presto_bitq_flush,
	.sleep = presto_bitq_sleep,
	.reset = presto_bitq_reset,
	.in_rdy = presto_bitq_in_rdy,
	.in = presto_bitq_in,
};


/* -------------------------------------------------------------------------- */


#define FT_DEVICE_NAME_LEN 64
#define FT_DEVICE_SERNUM_LEN 64

#define PRESTO_VID_PID 0x0403f1a0
#define PRESTO_VID (0x0403)
#define PRESTO_PID (0xf1a0)

#define BUFFER_SIZE (64*62)

typedef struct presto_s
{
#if BUILD_PRESTO_FTD2XX == 1
	FT_HANDLE handle;
	FT_STATUS status;
#elif BUILD_PRESTO_LIBFTDI == 1
	struct ftdi_context ftdic;
	int retval;
#endif

	char serial[FT_DEVICE_SERNUM_LEN];

	u8 buff_out[BUFFER_SIZE];
	int buff_out_pos;

	u8 buff_in[BUFFER_SIZE];
	int buff_in_exp; /* expected in buffer length */
	int buff_in_len; /* length of data received */
	int buff_in_pos;

	unsigned long total_out;
	unsigned long total_in;

	int jtag_tms; /* last tms state */
	int jtag_tck; /* last tck state */

	int jtag_tdi_data;
	int jtag_tdi_count;

} presto_t;

presto_t presto_state;
presto_t *presto = &presto_state;

u8 presto_init_seq[] =
{
	0x80, 0xA0, 0xA8, 0xB0, 0xC0, 0xE0
};

int presto_write(u8 *buf, int size)
{
#if BUILD_PRESTO_FTD2XX == 1
	DWORD ftbytes;
	if ((presto->status = FT_Write(presto->handle, buf, size, &ftbytes)) != FT_OK)
	{
		LOG_ERROR("FT_Write returned: %lu", presto->status);
		return ERROR_JTAG_DEVICE_ERROR;
	}

#elif BUILD_PRESTO_LIBFTDI == 1
	u32 ftbytes;
	if ((presto->retval = ftdi_write_data(&presto->ftdic, buf, size)) < 0)
	{
		LOG_ERROR("ftdi_write_data: %s", ftdi_get_error_string(&presto->ftdic));
		return ERROR_JTAG_DEVICE_ERROR;
	}
	ftbytes = presto->retval;
#endif

	if (ftbytes != size)
	{
		LOG_ERROR("couldn't write the requested number of bytes to PRESTO (%i < %i)", ftbytes, size);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

int presto_read(u8* buf, int size)
{
#if BUILD_PRESTO_FTD2XX == 1
	DWORD ftbytes;
	if ((presto->status = FT_Read(presto->handle, buf, size, &ftbytes)) != FT_OK)
	{
		LOG_ERROR("FT_Read returned: %lu", presto->status);
		return ERROR_JTAG_DEVICE_ERROR;
	}

#elif BUILD_PRESTO_LIBFTDI == 1
	u32 ftbytes = 0;

	struct timeval timeout, now;
	gettimeofday(&timeout, NULL);
	timeval_add_time(&timeout, 1, 0); /* one second timeout */

	while (ftbytes < size)
	{
		if ((presto->retval = ftdi_read_data(&presto->ftdic, buf + ftbytes, size - ftbytes)) < 0)
		{
			LOG_ERROR("ftdi_read_data: %s", ftdi_get_error_string(&presto->ftdic));
			return ERROR_JTAG_DEVICE_ERROR;
		}
		ftbytes += presto->retval;

		gettimeofday(&now, NULL);
		if ((now.tv_sec > timeout.tv_sec) || ((now.tv_sec == timeout.tv_sec) && (now.tv_usec > timeout.tv_usec)))
			break;
	}
#endif

	if (ftbytes != size)
	{
		/* this is just a warning, there might have been timeout when detecting PRESTO, which is not fatal */
		LOG_WARNING("couldn't read the requested number of bytes from PRESTO (%i < %i)", ftbytes, size);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}

#if BUILD_PRESTO_FTD2XX == 1
int presto_open_ftd2xx(char *req_serial)
{
	int i;
	DWORD numdevs;
	DWORD vidpid;
	char devname[FT_DEVICE_NAME_LEN];
	FT_DEVICE device;

	BYTE presto_data;
	DWORD ftbytes;

	presto->handle = (FT_HANDLE)INVALID_HANDLE_VALUE;

#if IS_WIN32 == 0
	/* Add non-standard Vid/Pid to the linux driver */
	if ((presto->status = FT_SetVIDPID(PRESTO_VID, PRESTO_PID)) != FT_OK)
	{
		LOG_ERROR("couldn't add PRESTO VID/PID");
		exit(-1);
	}
#endif

	if ((presto->status = FT_ListDevices(&numdevs, NULL, FT_LIST_NUMBER_ONLY)) != FT_OK)
	{
		LOG_ERROR("FT_ListDevices failed: %i", (int)presto->status);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	LOG_DEBUG("FTDI devices available: %i", numdevs);
	for (i = 0; i < numdevs; i++)
	{
		if ((presto->status = FT_Open(i, &(presto->handle))) != FT_OK)
		{
			/* this is not fatal, the device may be legitimately open by other process, hence debug message only */
			LOG_DEBUG("FT_Open failed: %i", (int)presto->status);
			continue;
		}
		LOG_DEBUG("FTDI device %i open", i);

		if ((presto->status = FT_GetDeviceInfo(presto->handle, &device, &vidpid,
				presto->serial, devname, NULL)) == FT_OK)
		{
			if (vidpid == PRESTO_VID_PID
					&& (req_serial == NULL || !strcmp(presto->serial, req_serial)))
				break;
		}
		else
			LOG_DEBUG("FT_GetDeviceInfo failed: %i", presto->status);

		LOG_DEBUG("FTDI device %i does not match, closing", i);
		FT_Close(presto->handle);
		presto->handle = (FT_HANDLE)INVALID_HANDLE_VALUE;
	}

	if (presto->handle == (FT_HANDLE)INVALID_HANDLE_VALUE)
		return ERROR_JTAG_DEVICE_ERROR; /* presto not open, return */

	if ((presto->status = FT_SetLatencyTimer(presto->handle, 1)) != FT_OK)
		return ERROR_JTAG_DEVICE_ERROR;


	if ((presto->status = FT_SetTimeouts(presto->handle, 100, 0)) != FT_OK)
		return ERROR_JTAG_DEVICE_ERROR;

	if ((presto->status = FT_Purge(presto->handle, FT_PURGE_TX | FT_PURGE_RX)) != FT_OK)
		return ERROR_JTAG_DEVICE_ERROR;

	presto_data = 0xD0;
	if ((presto->status = FT_Write(presto->handle, &presto_data, 1, &ftbytes)) != FT_OK)
		return ERROR_JTAG_DEVICE_ERROR;

	/* delay between first write/read turnaround (after purge?) necessary under Linux for unknown reason,
	   probably a bug in library threading */
	usleep(100000);
	if ((presto->status = FT_Read(presto->handle, &presto_data, 1, &ftbytes)) != FT_OK)
		return ERROR_JTAG_DEVICE_ERROR;

	if (ftbytes!=1)
	{
		LOG_DEBUG("PRESTO reset");

		if ((presto->status = FT_Purge(presto->handle, FT_PURGE_TX | FT_PURGE_RX)) != FT_OK)
			return ERROR_JTAG_DEVICE_ERROR;
		if ((presto->status = FT_SetBitMode(presto->handle, 0x80, 1)) != FT_OK)
			return ERROR_JTAG_DEVICE_ERROR;
		if ((presto->status = FT_SetBaudRate(presto->handle, 9600)) != FT_OK)
			return ERROR_JTAG_DEVICE_ERROR;

		presto_data = 0;
		for (i = 0; i < 4 * 62; i++)
			if ((presto->status=FT_Write(presto->handle, &presto_data, 1, &ftbytes)) != FT_OK)
				return ERROR_JTAG_DEVICE_ERROR;

		usleep(100000);

		if ((presto->status = FT_SetBitMode(presto->handle, 0x00, 0)) != FT_OK)
			return ERROR_JTAG_DEVICE_ERROR;

		if ((presto->status = FT_Purge(presto->handle, FT_PURGE_TX | FT_PURGE_RX)) != FT_OK)
			return ERROR_JTAG_DEVICE_ERROR;

		presto_data = 0xD0;
		if ((presto->status = FT_Write(presto->handle, &presto_data, 1, &ftbytes)) != FT_OK)
			return ERROR_JTAG_DEVICE_ERROR;

		/* delay between first write/read turnaround (after purge?) necessary under Linux for unknown reason,
		   probably a bug in library threading */
		usleep(100000);
		if ((presto->status = FT_Read(presto->handle, &presto_data, 1, &ftbytes)) != FT_OK)
			return ERROR_JTAG_DEVICE_ERROR;

		if (ftbytes!=1)
		{
			LOG_DEBUG("PRESTO not responding");
			return ERROR_JTAG_DEVICE_ERROR;
		}
	}

	if ((presto->status = FT_SetTimeouts(presto->handle, 0, 0)) != FT_OK)
		return ERROR_JTAG_DEVICE_ERROR;


	presto->status = FT_Write(presto->handle, &presto_init_seq, sizeof(presto_init_seq), &ftbytes);
	if (presto->status != FT_OK || ftbytes != sizeof(presto_init_seq))
		return ERROR_JTAG_DEVICE_ERROR;

	return ERROR_OK;
}

#elif BUILD_PRESTO_LIBFTDI == 1
int presto_open_libftdi(char *req_serial)
{
	u8 presto_data;

	LOG_DEBUG("searching for PRESTO using libftdi");

	/* initialize FTDI context structure */
	if (ftdi_init(&presto->ftdic) < 0)
	{
		LOG_ERROR("unable to init libftdi: %s", presto->ftdic.error_str);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	/* context, vendor id, product id */
	if (ftdi_usb_open_desc(&presto->ftdic, PRESTO_VID, PRESTO_PID, NULL, req_serial) < 0)
	{
		LOG_ERROR("unable to open PRESTO: %s", presto->ftdic.error_str);
		return ERROR_JTAG_DEVICE_ERROR;
	}

	if (ftdi_usb_reset(&presto->ftdic) < 0)
	{
		LOG_ERROR("unable to reset PRESTO device");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	if (ftdi_set_latency_timer(&presto->ftdic, 1) < 0)
	{
		LOG_ERROR("unable to set latency timer");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	if (ftdi_usb_purge_buffers(&presto->ftdic) < 0)
	{
		LOG_ERROR("unable to purge PRESTO buffers");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	presto_data = 0xD0;
	if (presto_write(&presto_data, 1) != ERROR_OK)
	{
		LOG_ERROR("error writing to PRESTO");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	if (presto_read(&presto_data, 1) != ERROR_OK)
	{
		LOG_DEBUG("no response from PRESTO, retrying");

		if (ftdi_usb_purge_buffers(&presto->ftdic) < 0)
			return ERROR_JTAG_DEVICE_ERROR;

		presto_data = 0xD0;
		if (presto_write(&presto_data, 1) != ERROR_OK)
			return ERROR_JTAG_DEVICE_ERROR;

		if (presto_read(&presto_data, 1) != ERROR_OK)
		{
			LOG_ERROR("no response from PRESTO, giving up");
			return ERROR_JTAG_DEVICE_ERROR;
		}
	}

	if (presto_write(presto_init_seq, sizeof(presto_init_seq)) != ERROR_OK)
	{
		LOG_ERROR("error writing PRESTO init sequence");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	return ERROR_OK;
}
#endif /* BUILD_PRESTO_LIBFTDI == 1 */

int presto_open(char *req_serial)
{
	presto->buff_out_pos=0;
	presto->buff_in_pos=0;
	presto->buff_in_len=0;
	presto->buff_in_exp=0;

	presto->total_out=0;
	presto->total_in=0;

	presto->jtag_tms=0;
	presto->jtag_tck=0;
	presto->jtag_tdi_data=0;
	presto->jtag_tdi_count=0;

#if BUILD_PRESTO_FTD2XX == 1
	return presto_open_ftd2xx(req_serial);
#elif BUILD_PRESTO_LIBFTDI == 1
	return presto_open_libftdi(req_serial);
#endif
}

int presto_close(void)
{

	int result = ERROR_OK;

#if BUILD_PRESTO_FTD2XX == 1
	unsigned long ftbytes;

	if (presto->handle == (FT_HANDLE)INVALID_HANDLE_VALUE)
		return result;

	presto->status = FT_Purge(presto->handle, FT_PURGE_TX | FT_PURGE_RX);
	if (presto->status != FT_OK)
		result = ERROR_JTAG_DEVICE_ERROR;

	presto->status = FT_Write(presto->handle, &presto_init_seq, sizeof(presto_init_seq), &ftbytes);
	if (presto->status != FT_OK || ftbytes != sizeof(presto_init_seq))
		result = ERROR_JTAG_DEVICE_ERROR;

	if ((presto->status = FT_SetLatencyTimer(presto->handle, 16)) != FT_OK)
		result = ERROR_JTAG_DEVICE_ERROR;

	if ((presto->status = FT_Close(presto->handle)) != FT_OK)
		result = ERROR_JTAG_DEVICE_ERROR;
	else
		presto->handle = (FT_HANDLE)INVALID_HANDLE_VALUE;

#elif BUILD_PRESTO_LIBFTDI == 1

	if ((presto->retval = ftdi_write_data(&presto->ftdic, presto_init_seq, sizeof(presto_init_seq))) != sizeof(presto_init_seq))
		result = ERROR_JTAG_DEVICE_ERROR;

	if ((presto->retval = ftdi_set_latency_timer(&presto->ftdic, 16)) < 0)
		result = ERROR_JTAG_DEVICE_ERROR;

	if ((presto->retval = ftdi_usb_close(&presto->ftdic)) < 0)
		result = ERROR_JTAG_DEVICE_ERROR;
	else
		ftdi_deinit(&presto->ftdic);
#endif

	return result;
}


int presto_flush(void)
{
	if (presto->buff_out_pos == 0)
		return ERROR_OK;

#if BUILD_PRESTO_FTD2XX == 1
	if (presto->status != FT_OK)
#elif BUILD_PRESTO_LIBFTDI == 1
	if (presto->retval < 0)
#endif
	{
		LOG_DEBUG("error in previous communication, canceling I/O operation");
		return ERROR_JTAG_DEVICE_ERROR;
	}

	if (presto_write(presto->buff_out, presto->buff_out_pos) != ERROR_OK)
	{
		presto->buff_out_pos = 0;
		return ERROR_JTAG_DEVICE_ERROR;
	}

	presto->total_out += presto->buff_out_pos;
	presto->buff_out_pos = 0;

	if (presto->buff_in_exp == 0)
		return ERROR_OK;

	presto->buff_in_pos = 0;
	presto->buff_in_len = 0;

	if (presto_read(presto->buff_in, presto->buff_in_exp) != ERROR_OK)
	{
		presto->buff_in_exp = 0;
		return ERROR_JTAG_DEVICE_ERROR;
	}

	presto->total_in += presto->buff_in_exp;
	presto->buff_in_len = presto->buff_in_exp;
	presto->buff_in_exp = 0;

	return ERROR_OK;
}


int presto_sendbyte(int data)
{
	if (data == EOF) return presto_flush();

	if (presto->buff_out_pos < BUFFER_SIZE)
	{
		presto->buff_out[presto->buff_out_pos++] = (u8)data;
		if (((data & 0xC0) == 0x40) || ((data & 0xD0)== 0xD0))
			presto->buff_in_exp++;
	}
	else
		return ERROR_JTAG_DEVICE_ERROR;

#if BUILD_PRESTO_FTD2XX == 1
	if (presto->buff_out_pos >= BUFFER_SIZE)
#elif BUILD_PRESTO_LIBFTDI == 1
	/* libftdi does not do background read, be sure that USB IN buffer does not overflow (128 bytes only!) */
	if (presto->buff_out_pos >= BUFFER_SIZE || presto->buff_in_exp==128)
#endif
		return presto_flush();

	return ERROR_OK;
}


int presto_getbyte(void)
{
	if (presto->buff_in_pos < presto->buff_in_len)
		return presto->buff_in[presto->buff_in_pos++];

	if (presto->buff_in_exp == 0)
		return -1;

	if (presto_flush() != ERROR_OK)
		return -1;

	if (presto->buff_in_pos<presto->buff_in_len)
		return presto->buff_in[presto->buff_in_pos++];

	return -1;
}


/* -------------------------------------------------------------------------- */


int presto_bitq_out(int tms, int tdi, int tdo_req)
{
	unsigned char cmdparam;

	if (presto->jtag_tck == 0)
	{
		presto_sendbyte(0xA4);
		presto->jtag_tck = 1;
	}

	else if (!tdo_req && tms == presto->jtag_tms)
	{
		if (presto->jtag_tdi_count == 0)
			presto->jtag_tdi_data = (tdi != 0);
		else
			presto->jtag_tdi_data |= (tdi != 0) << presto->jtag_tdi_count;

		if (++presto->jtag_tdi_count == 4)
		{
			presto->jtag_tdi_data |= (presto->jtag_tdi_count - 1) << 4;
			presto_sendbyte(presto->jtag_tdi_data);
			presto->jtag_tdi_count = 0;
		}
		return 0;
	}

	if (presto->jtag_tdi_count)
	{
		presto->jtag_tdi_data |= (presto->jtag_tdi_count - 1) << 4;
		presto_sendbyte(presto->jtag_tdi_data);
		presto->jtag_tdi_count = 0;
	}

	if (tdi)
		cmdparam = 0x0B;
	else
		cmdparam = 0x0A;

	presto_sendbyte( 0xC0 | cmdparam);

	if (tms != presto->jtag_tms)
	{
		if (tms)
			presto_sendbyte(0xEC);
		else
			presto_sendbyte(0xE8);
		presto->jtag_tms = tms;
	}

	if (tdo_req)
		presto_sendbyte(0xD4 | cmdparam);
	else
		presto_sendbyte(0xC4|cmdparam);

	return 0;
}


int presto_bitq_flush(void)
{
	if (presto->jtag_tdi_count)
	{
		presto->jtag_tdi_data |= (presto->jtag_tdi_count - 1) << 4;
		presto_sendbyte(presto->jtag_tdi_data);
		presto->jtag_tdi_count = 0;
	}

	presto_sendbyte(0xCA);
	presto->jtag_tck = 0;

	presto_sendbyte(0xA0);

	return presto_flush();
}


int presto_bitq_in_rdy(void)
{
	if (presto->buff_in_pos>=presto->buff_in_len)
		return 0;
	return presto->buff_in_len-presto->buff_in_pos;
}


int presto_bitq_in(void)
{
	if (presto->buff_in_pos>=presto->buff_in_len)
		return -1;
	if (presto->buff_in[presto->buff_in_pos++]&0x08) return 1;
	return 0;
}


int presto_bitq_sleep(unsigned long us)
{
	long waits;

	if (us > 100000)
	{
		presto_bitq_flush();
		jtag_sleep(us);
		return 0;
	}

	waits = us / 170 + 2;
	while (waits--)
		presto_sendbyte(0x80);

	return 0;
}


int presto_bitq_reset(int trst, int srst)
{
	unsigned char cmd;

	cmd = 0xE8;
	if (presto->jtag_tms)
		cmd |= 0x04;

	if (trst || srst)
		cmd |= 0x02;

	presto_sendbyte(cmd);
	return 0;
}


/* -------------------------------------------------------------------------- */

char *presto_speed_text[4] =
{
	"3 MHz",
	"1.5 MHz",
	"750 kHz",
	"93.75 kHz"
};

int presto_jtag_speed(int speed)
{

	if ((speed < 0) || (speed > 3))
	{
		LOG_INFO("valid speed values: 0 (3 MHz), 1 (1.5 MHz), 2 (750 kHz) and 3 (93.75 kHz)");
		return ERROR_INVALID_ARGUMENTS;
	}

	LOG_INFO("setting speed to %d, max. TCK freq. is %s", speed, presto_speed_text[speed]);
	return presto_sendbyte(0xA8 | speed);
}


char *presto_serial;

int presto_handle_serial_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc)
{
	if (argc == 1)
	{
		if (presto_serial)
			free(presto_serial);
		presto_serial = strdup(args[0]);
	}
	else
	{
		LOG_ERROR("expected exactly one argument to presto_serial <serial-number>");
	}

	return ERROR_OK;
}


int presto_jtag_register_commands(struct command_context_s *cmd_ctx)
{
	register_command(cmd_ctx, NULL, "presto_serial", presto_handle_serial_command,
		COMMAND_CONFIG, NULL);
	return ERROR_OK;
}


int presto_jtag_init(void)
{
	if (presto_open(presto_serial) != ERROR_OK)
	{
		presto_close();
		if (presto_serial != NULL)
			LOG_ERROR("Cannot open PRESTO, serial number '%s'", presto_serial);
		else
			LOG_ERROR("Cannot open PRESTO");
		return ERROR_JTAG_INIT_FAILED;
	}
	LOG_INFO("PRESTO open, serial number '%s'", presto->serial);

	/* use JTAG speed setting from configuration file */
	presto_jtag_speed(jtag_speed);

	bitq_interface = &presto_bitq;
	return ERROR_OK;
}


int presto_jtag_quit(void)
{
	bitq_cleanup();
	presto_close();
	LOG_INFO("PRESTO closed");

	if (presto_serial)
	{
		free(presto_serial);
		presto_serial = NULL;
	}

	return ERROR_OK;
}
