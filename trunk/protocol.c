/*
 * MOC - music on console
 * Copyright (C) 2003,2004,2005 Damian Pietras <daper@daper.net>
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
#include <time.h>

#include "main.h"
#include "log.h"
#include "protocol.h"
#include "playlist.h"

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

	if (len < 0 || len > MAX_SEND_STRING) {
		logit ("Bad string length.");
		return NULL;
	}

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

/* Get a time_t value from the socket, return == 0 on error. */
int get_time (int sock, time_t *i)
{
	int res;
	
	res = recv (sock, i, sizeof(time_t), 0);
	if (res == -1)
		logit ("recv() failed when getting time_t: %s", strerror(errno));

	return res == sizeof(time_t) ? 1 : 0;
}

/* Send a time_t value to the socket, return == 0 on error */
int send_time (int sock, time_t i)
{
	int res;
	
	res = send (sock, &i, sizeof(time_t), 0);
	if (res == -1)
		logit ("send() failed: %s", strerror(errno));

	return res == sizeof(time_t) ? 1 : 0;
}

/* Send a playlist item to the socket. If item == NULL, send empty item mark
 * (end of playlist). Return 0 on error. */
int send_item (int sock, const struct plist_item *item)
{
	if (!item) {
		if (!send_str(sock, "")) {
			logit ("Error while sending empty item");
			return 0;
		}
		return 1;
	}

	if (!send_str(sock, item->file)) {
		logit ("Error while sending file name");
		return 0;
	}
	if (item->tags) {
		if (!send_str(sock, item->tags->title
					? item->tags->title : "")) {
			logit ("Error while sending title");
			return 0;
		}
		
		if (!send_str(sock, item->tags->artist
					? item->tags->artist : "")) {
			logit ("Error while sending artist");
			return 0;
		}
		
		if (!send_str(sock, item->tags->album
					? item->tags->album : "")) {
			logit ("Error while sending album");
			return 0;
		}
		
		if (!send_int(sock, item->tags->track)) {
			logit ("Error while sending track");
			return 0;
		}
		
		if (!send_int(sock, item->tags->filled & TAGS_TIME
				? item->tags->time : -1)) {
			logit ("Error while sending time");
			return 0;
		}
	}
	else {
		
		/* empty tags: */
		if (!send_str(sock, "")) { /* title */
			logit ("Error while sending title");
			return 0;
		}
		
		if (!send_str(sock, "")) { /* artist */
			logit ("Error while sending artist");
			return 0;
		}
		
		if (!send_str(sock, "")) { /* album */
			logit ("Error while sending album");
			return 0;
		}
		
		if (!send_int(sock, -1)) { /* track */
			logit ("Error while sending track");
			return 0;
		}
		
		if (!send_int(sock, -1)) { /* time */
			logit ("Error while sending time");
			return 0;
		}
	}

	if (!send_time(sock, item->mtime)) {
		logit ("Error while sending mtime");
		return 0;
	}

	return 1;
}

/* Get a playlist item from the server. If empty item->file is an empty string,
 * end of playlist arrived (empty item). The memory is malloc()ed. Return NULL
 * on error. */
struct plist_item *recv_item (int sock)
{
	struct plist_item *item = plist_new_item ();

	/* get the file name */
	if (!(item->file = get_str(sock))) {
		logit ("Error while receiving file name");
		free (item);
		return NULL;
	}
	
	if (item->file[0]) {
		char *title, *artist, *album;
		int track, time;
		
		if (!(title = get_str(sock))) {
			logit ("Error while receiving titile");
			free (item);
			return NULL;
		}
		
		if (!(artist = get_str(sock))) {
			logit ("Error while receiving artist");
			free (title);
			free (item);
			return NULL;
		}
		
		if (!(album = get_str(sock))) {
			logit ("Error while receiving album");
			free (title);
			free (artist);
			free (item);
			return NULL;
		}
			
		if (!get_int(sock, &track)) {
			logit ("Error while receiving ");
			free (title);
			free (artist);
			free (item);
			return NULL;
		}
		
		if (!get_int(sock, &time)) {
			logit ("Error while receiving time");
			free (title);
			free (artist);
			free (item);
			return NULL;
		}
		
		if (!get_time(sock, &item->mtime)) {
			logit ("Error while receiving mtime");
			free (title);
			free (artist);
			free (item);
			return NULL;
		}

		if (*title || *artist || *album || track != -1 || time != -1) {
			item->tags = tags_new ();

			if (*title || *artist || *album) {
				item->tags->filled |= TAGS_COMMENTS;

				if (*title)
					item->tags->title = title;
				else
					free (title);
				
				if (*artist)
					item->tags->artist = artist;
				else
					free (artist);
				
				if (*album)
					item->tags->album = album;
				else
					free (album);
			}
			else {
				free (title);
				free (artist);
				free (album);
			}
			
			if (time != -1) {
				item->tags->filled |= TAGS_TIME;
				item->tags->time = time;
			}

			item->tags->track = track;
		}
		else {
			free (title);
			free (artist);
			free (album);
		}
	}
	
	return item;
}
