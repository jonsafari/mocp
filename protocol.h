#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "playlist.h"

struct event
{
	int type;	/* type of the event (one of EV_*) */
	void *data;	/* optional data associated with the event */
	struct event *next;
};

struct event_queue
{
	struct event *head;
	struct event *tail;
};

/* Definition of events send by server to the client */
#define EV_STATE	0x01 /* server has changed the state */
#define EV_CTIME	0x02 /* current time of the song has changed */
#define EV_ERROR	0x04 /* an error occured */
#define EV_BUSY		0x05 /* another client is connected to the server */
#define EV_DATA		0x06 /* data in response to a request will arrive */
#define EV_BITRATE	0x07 /* the bitrate has changed */
#define EV_RATE		0x08 /* the rate has changed */
#define EV_CHANNELS	0x09 /* the number of channels has changed */
#define EV_EXIT		0x0a /* the server is about to exit */
#define EV_PONG		0x0b /* response for CMD_PING */
#define EV_OPTIONS	0x0c /* the options has changed */
#define EV_SEND_PLIST	0x0d /* Request for sending the playlist. */

/* Events caused by a client that wants to modify the playlist (see
 * CMD_CLI_PLIST* commands. */
#define EV_PLIST_ADD	0x0e /* add an item, followed by the file name */
#define EV_PLIST_DEL	0x0f /* delete an item, followed by the file name */
#define EV_PLIST_CLEAR	0x10 /* clear the playlist */

/* State of the server. */
#define STATE_PLAY	0x01
#define STATE_STOP	0x02
#define STATE_PAUSE	0x03

/* Definition of server commands */
#define CMD_PLAY	0x00 /* play the first element on the list */
#define CMD_LIST_CLEAR	0x01 /* clear the list */
#define CMD_LIST_ADD	0x02 /* add an item to the list */
#define CMD_STOP	0x04 /* stop playing */
#define CMD_PAUSE	0x05 /* pause */
#define CMD_UNPAUSE	0x06 /* unpause */
#define CMD_SET_OPTION	0x07 /* set an option */
#define CMD_GET_OPTION	0x08 /* get an option */
#define CMD_GET_CTIME	0x0d /* get the current song time */
#define CMD_GET_SNAME	0x0f /* get the stream file name */
#define CMD_NEXT	0x10 /* start playing next song if available */
#define CMD_QUIT	0x11 /* shutdown the server */
#define CMD_SEEK	0x12 /* seek in the current stream */
#define CMD_GET_STATE	0x13 /* get the state */
#define CMD_DISCONNECT	0x15 /* disconnect from the server */
#define CMD_GET_BITRATE	0x16 /* get the bitrate */
#define CMD_GET_RATE	0x17 /* get the rate */
#define CMD_GET_CHANNELS	0x18 /* get the number of channels */
#define CMD_PING	0x19 /* request for EV_PONG */
#define CMD_GET_MIXER	0x1a /* get the volume level */
#define CMD_SET_MIXER	0x1b /* set the volume level */
#define CMD_DELETE	0x1c /* delete an item from the playlist */
#define CMD_SEND_EVENTS 0x1d /* request for events */
#define CMD_GET_ERROR	0x1e /* get the error message */
#define CMD_GET_FTIME	0x1f /* get time of a file from the server */
#define CMD_PREV	0x20 /* start playing previous song if available */
#define CMD_SEND_PLIST	0x21 /* send the playlist to the requesting client */
#define CMD_GET_PLIST	0x22 /* get the playlist from one of the clients */
#define CMD_CAN_SEND_PLIST	0x23 /* mark the client as able to to send
					playlist */
#define CMD_CLI_PLIST_ADD	0x24 /* add an item to the clients playlist */
#define CMD_CLI_PLIST_DEL	0x25 /* delete an item from the clients
					playlist */
#define CMD_CLI_PLIST_CLEAR	0x26 /* clear the clients playlist */
#define CMD_GET_SERIAL	0x27 /* get an unique serial number */
#define CMD_PLIST_SET_SERIAL	0x28 /* assign a serial number to the server
					playlist */
#define CMD_LOCK	0x29 /* acquire a lock */
#define CMD_UNLOCK	0x2a /* release the lock */
#define CMD_PLIST_GET_SERIAL	0x2b /* get the serial number of the server's
					playlist */

char *socket_name ();
int get_int (int sock, int *i);
int send_int (int sock, int i);
char *get_str (int sock);
int send_str (int sock, const char *str);
int get_time (int sock, time_t *i);
int send_time (int sock, time_t i);
int send_item (int sock, const struct plist_item *item);
struct plist_item *recv_item (int sock);

void event_queue_init (struct event_queue *q);
void event_queue_free (struct event_queue *q);
struct event *event_get_first (struct event_queue *q);
void event_pop (struct event_queue *q);
void event_push (struct event_queue *q, const int event, void *data);

#endif
