/*
 * MOC - music on console
 * Copyright (C) 2003,2004 Damian Pietras <daper@daper.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include "main.h"
#include "log.h"

/* Maximal socket name. */
#define UNIX_PATH_MAX	108
#define SOCKET_NAME	"socket2"

/* Create a socket name, return NULL if the name could not be created */
char *socket_name ()
{
	char *socket_name = create_file_name (SOCKET_NAME);

	if (strlen(socket_name) > UNIX_PATH_MAX)
		fatal ("Can't create socket name.");

	return socket_name;
}

/* Get an intiger value from the socket, return == 0 on error. */
int get_int (int sock, int *i)
{
	int res;
	
	res = recv (sock, i, sizeof(int), 0);
	if (res == -1)
		logit ("recv() failed when getting int: %s", strerror(errno));

	return res == sizeof(int) ? 1 : 0;
}

/* Send an integer value to the socket, return == 0 on error */
int send_int (int sock, int i)
{
	int res;
	
	res = send (sock, &i, sizeof(int), 0);
	if (res == -1)
		logit ("send() failed: %s", strerror(errno));

	return res == sizeof(int) ? 1 : 0;
}

/* Get the string from socket, return NULL on error. The memory is malloced. */
char *get_str (int sock)
{
	int len;
	int res, nread = 0;
	char *str;
	
	if (!get_int(sock, &len))
		return NULL;

	if (len < 0 || len > MAX_SEND_STRING)
		return NULL;

	str = (char *)xmalloc (sizeof(char) * (len + 1));
	while (nread < len) {
		res = recv (sock, str + nread, len - nread, 0);
		if (res == -1) {
			logit ("recv() failed when getting string: %s",
					strerror(errno));
			free (str);
			return NULL;
		}
		if (res == 0) {
			logit ("Unexpected EOF when getting string");
			free (str);
			return NULL;
		}
		nread += res;
	}
	str[len] = 0;

	return str;
}

int send_str (int sock, const char *str)
{
	int len = strlen (str);
	
	if (!send_int(sock, strlen(str)))
		return 0;

	if (send(sock, str, len, 0) != len)
		return 0;
	
	return 1;
}

