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

#include "server.h"

#include "log.h"
#include "telnet_server.h"
#include "target.h"

#include <command.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

service_t *services = NULL;

/* shutdown_openocd == 1: exit the main event loop, and quit the debugger */
static int shutdown_openocd = 0;
int handle_shutdown_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc);

int add_connection(service_t *service, command_context_t *cmd_ctx)
{
	unsigned int address_size;
	connection_t *c, **p;
	int retval;
#ifndef _WIN32
	int flag=1;
#endif
	
	c = malloc(sizeof(connection_t));
	c->fd = -1;
	memset(&c->sin, 0, sizeof(c->sin));
	c->cmd_ctx = copy_command_context(cmd_ctx);
	c->service = service;
	c->input_pending = 0;
	c->priv = NULL;
	c->next = NULL;

	address_size = sizeof(c->sin);
#ifndef _WIN32
	int segsize=65536;
	setsockopt(service->fd, IPPROTO_TCP, TCP_MAXSEG,  &segsize, sizeof(int));
	int window_size = 128 * 1024;	

	/* These setsockopt()s must happen before the accept() */

	setsockopt(service->fd, SOL_SOCKET, SO_SNDBUF,
		 (char *) &window_size, sizeof(window_size));

	setsockopt(service->fd, SOL_SOCKET, SO_RCVBUF,
		 (char *) &window_size, sizeof(window_size));
	
#endif
	c->fd = accept(service->fd, (struct sockaddr *)&service->sin, &address_size);
#ifndef _WIN32
	// This increases performance dramatically for e.g. GDB load which
	// does not have a sliding window protocol.
    retval=setsockopt(c->fd,            /* socket affected */
                            IPPROTO_TCP,     /* set option at TCP level */
                            TCP_NODELAY,     /* name of option */
                            (char *) &flag,  /* the cast is historical
                                                    cruft */
                            sizeof(int));    /* length of option value */
    setsockopt(c->fd, IPPROTO_TCP, TCP_MAXSEG,  &segsize, sizeof(int));
#endif	
				
				
	LOG_INFO("accepting '%s' connection from %i", service->name, c->sin.sin_port);
	if ((retval = service->new_connection(c)) == ERROR_OK)
	{
	}
	else
	{
		close_socket(c->fd);
		LOG_ERROR("attempted '%s' connection rejected", service->name);
		free(c);
		return retval;
	}
	
	/* add to the end of linked list */
	for (p = &service->connections; *p; p = &(*p)->next);
	*p = c;
	
	service->max_connections--;
	
	return ERROR_OK;
}

int remove_connection(service_t *service, connection_t *connection)
{
	connection_t **p = &service->connections;
	connection_t *c;
	
	/* find connection */
	while((c = *p))
	{		
		if (c->fd == connection->fd)
		{	
			service->connection_closed(c);
			close_socket(c->fd);
			command_done(c->cmd_ctx);
			
			/* delete connection */
			*p = c->next;
			free(c);
			
			service->max_connections++;
			break;
		}
		
		/* redirect p to next list pointer */
		p = &(*p)->next;		
	}
	
	return ERROR_OK;
}

int add_service(char *name, enum connection_type type, unsigned short port, int max_connections, new_connection_handler_t new_connection_handler, input_handler_t input_handler, connection_closed_handler_t connection_closed_handler, void *priv)
{
	service_t *c, **p;
	int so_reuseaddr_option = 1;
	
	c = malloc(sizeof(service_t));
	
	c->name = strdup(name);
	c->type = type;
	c->port = port;
	c->max_connections = max_connections;
	c->fd = -1;
	c->connections = NULL;
	c->new_connection = new_connection_handler;
	c->input = input_handler;
	c->connection_closed = connection_closed_handler;
	c->priv = priv;
	c->next = NULL;
	
	if ((c->fd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
	{
		LOG_ERROR("error creating socket: %s", strerror(errno));
		exit(-1);
	}
	
	setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, (void*)&so_reuseaddr_option, sizeof(int));
	
	socket_nonblock(c->fd);
	
	memset(&c->sin, 0, sizeof(c->sin));
	c->sin.sin_family = AF_INET;
	c->sin.sin_addr.s_addr = INADDR_ANY;
	c->sin.sin_port = htons(port);
	
	if (bind(c->fd, (struct sockaddr *)&c->sin, sizeof(c->sin)) == -1)
	{
		LOG_ERROR("couldn't bind to socket: %s", strerror(errno));
		exit(-1);
	}
	
	if (listen(c->fd, 1) == -1)
	{
		LOG_ERROR("couldn't listen on socket: %s", strerror(errno));
		exit(-1);
	}
	
	/* add to the end of linked list */
	for (p = &services; *p; p = &(*p)->next);
	*p = c;
	
	return ERROR_OK;
}

int remove_service(unsigned short port)
{
	service_t **p = &services;
	service_t *c;
	
	/* find service */
	while((c = *p))
	{		
		if (c->port == port)
		{	
			if (c->name)
				free(c->name);
			
			if (c->priv)
				free(c->priv);
			
			/* delete service */
			*p = c->next;
			free(c);
		}

		/* redirect p to next list pointer */
		p = &(*p)->next;
	}
	
	return ERROR_OK;
}

int remove_services()
{
	service_t *c = services;

	/* loop service */
	while(c)
	{
		service_t *next = c->next;

		if (c->name)
			free(c->name);

		if (c->priv)
			free(c->priv);

		/* delete service */
		free(c);

		/* remember the last service for unlinking */
		c = next;
	}

	services = NULL;
	
	return ERROR_OK;
}

extern void lockBigLock();
extern void unlockBigLock();

int server_loop(command_context_t *command_context)
{
	service_t *service;

	/* used in select() */
	fd_set read_fds;
	struct timeval tv;
	int fd_max;
	
	/* used in accept() */
	int retval;
	
#ifndef _WIN32
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		LOG_ERROR("couldn't set SIGPIPE to SIG_IGN");
#endif

	// This function is reentrant(workaround for configuration problems)
	static int lockCount=0;
	if (lockCount++==0)
	{
		lockBigLock();
	}
	
	/* do regular tasks after at most 10ms */
	tv.tv_sec = 0;
	tv.tv_usec = 10000;
	
	while(!shutdown_openocd)
	{
		/* monitor sockets for acitvity */
		fd_max = 0;
		FD_ZERO(&read_fds);

		/* add service and connection fds to read_fds */
		for (service = services; service; service = service->next)
		{
			if (service->fd != -1)
			{
				/* listen for new connections */
				FD_SET(service->fd, &read_fds);

				if (service->fd > fd_max)
					fd_max = service->fd;
			}
			
			if (service->connections)
			{
				connection_t *c;
				
				for (c = service->connections; c; c = c->next)
				{
					/* check for activity on the connection */
					FD_SET(c->fd, &read_fds);
					if (c->fd > fd_max)
						fd_max = c->fd;
				}
			}
		}
		
#ifndef _WIN32
#ifndef BUILD_ECOSBOARD
		/* add STDIN to read_fds */
		FD_SET(fileno(stdin), &read_fds);
#endif
#endif

		// Only while we're sleeping we'll let others run
		unlockBigLock();
		retval = select(fd_max + 1, &read_fds, NULL, NULL, &tv);
		lockBigLock();

		if (retval == -1)
		{
#ifdef _WIN32

			errno = WSAGetLastError();

			if (errno == WSAEINTR)
				FD_ZERO(&read_fds);
			else
			{
				LOG_ERROR("error during select: %s", strerror(errno));
				exit(-1);
			}
#else

			if (errno == EINTR)
			{
				FD_ZERO(&read_fds);
			}
			else
			{
				LOG_ERROR("error during select: %s", strerror(errno));
				exit(-1);
			}
#endif
		}
		
		target_call_timer_callbacks();

		if (retval == 0)
		{
			/* do regular tasks after at most 100ms */
			tv.tv_sec = 0;
			tv.tv_usec = 10000;
			FD_ZERO(&read_fds); /* eCos leaves read_fds unchanged in this case!  */
		}
		
		for (service = services; service; service = service->next)
		{
			/* handle new connections on listeners */
			if ((service->fd != -1) 
				&& (FD_ISSET(service->fd, &read_fds))) 
			{
				if (service->max_connections > 0)
				{
					add_connection(service, command_context);
				}
				else
				{
					struct sockaddr_in sin;
					unsigned int address_size = sizeof(sin);
					int tmp_fd;
					tmp_fd = accept(service->fd, (struct sockaddr *)&service->sin, &address_size);
					close_socket(tmp_fd);
					LOG_INFO("rejected '%s' connection, no more connections allowed", service->name);
				}
			}
			
			/* handle activity on connections */
			if (service->connections)
			{
				connection_t *c;
				
				for (c = service->connections; c;)
				{
					if ((FD_ISSET(c->fd, &read_fds)) || c->input_pending)
					{
						if (service->input(c) != ERROR_OK)
						{
							connection_t *next = c->next;
							remove_connection(service, c);
							LOG_INFO("dropped '%s' connection", service->name);
							c = next;
							continue;
						}
					}
					c = c->next;
				}
			}
		}
		
#ifndef _WIN32
#ifndef BUILD_ECOSBOARD
		if (FD_ISSET(fileno(stdin), &read_fds))
		{
			if (getc(stdin) == 'x')
			{
				shutdown_openocd = 1;
			}
		}
#endif
#else
		MSG msg;
		while (PeekMessage(&msg,NULL,0,0,PM_REMOVE))
		{
			if (msg.message == WM_QUIT)
				shutdown_openocd = 1;
		}
#endif
	}
	if (--lockCount==0)
	{
		unlockBigLock();
	}
	
	return ERROR_OK;
}

#ifdef _WIN32
BOOL WINAPI ControlHandler(DWORD dwCtrlType)
{
    shutdown_openocd = 1;
    return TRUE;
}

void sig_handler(int sig) {
    shutdown_openocd = 1;
}
#endif

int server_init()
{
#ifdef _WIN32
	WORD wVersionRequested;
	WSADATA wsaData;

	wVersionRequested = MAKEWORD( 2, 2 );

	if (WSAStartup(wVersionRequested, &wsaData) != 0)
	{
		LOG_ERROR("Failed to Open Winsock");
		exit(-1);
	}

	SetConsoleCtrlHandler( ControlHandler, TRUE );

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	signal(SIGBREAK, sig_handler);
	signal(SIGABRT, sig_handler);
#endif

	
	return ERROR_OK;
}

int server_quit()
{
	remove_services();

#ifdef _WIN32
	WSACleanup();
	SetConsoleCtrlHandler( ControlHandler, FALSE );
#endif

	return ERROR_OK;
}

int server_register_commands(command_context_t *context)
{
	register_command(context, NULL, "shutdown", handle_shutdown_command,
					 COMMAND_ANY, "shut the server down");
	
	return ERROR_OK;
}

/* tell the server we want to shut down */
int handle_shutdown_command(struct command_context_s *cmd_ctx, char *cmd, char **args, int argc)
{
	shutdown_openocd = 1;

	return ERROR_COMMAND_CLOSE_CONNECTION;
}


