#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "playlist.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* Used as data field in the event queue for EV_FILE_TAGS. */
struct tag_ev_response
{
	char *file;
	struct file_tags *tags;
};

/* Used as data field in the event queue for EV_PLIST_MOVE. */
struct move_ev_data
{
	/* Two files that are to be exchanged. */
	char *from;
	char *to;
};

/* Status of nonblock sending/receiving function. */
enum noblock_io_status
{
	NB_IO_OK,
	NB_IO_BLOCK,
	NB_IO_ERR
};

/* Definition of events sent by server to the client. */
#define EV_STATE	0x01 /* server has changed the state */
#define EV_CTIME	0x02 /* current time of the song has changed */
#define EV_SRV_ERROR	0x04 /* an error occurred */
#define EV_BUSY		0x05 /* another client is connected to the server */
#define EV_DATA		0x06 /* data in response to a request will arrive */
#define EV_BITRATE	0x07 /* the bitrate has changed */
#define EV_RATE		0x08 /* the rate has changed */
#define EV_CHANNELS	0x09 /* the number of channels has changed */
#define EV_EXIT		0x0a /* the server is about to exit */
#define EV_PONG		0x0b /* response for CMD_PING */
#define EV_OPTIONS	0x0c /* the options has changed */
#define EV_SEND_PLIST	0x0d /* request for sending the playlist */
#define EV_TAGS		0x0e /* tags for the current file have changed */
#define EV_STATUS_MSG	0x0f /* followed by a status message */
#define EV_MIXER_CHANGE	0x10 /* the mixer channel was changed */
#define EV_FILE_TAGS	0x11 /* tags in a response for tags request */
#define EV_AVG_BITRATE  0x12 /* average bitrate has changed (new song) */
#define EV_AUDIO_START	0x13 /* playing of audio has started */
#define EV_AUDIO_STOP	0x14 /* playing of audio has stopped */

/* Events caused by a client that wants to modify the playlist (see
 * CMD_CLI_PLIST* commands). */
#define EV_PLIST_ADD	0x50 /* add an item, followed by the file name */
#define EV_PLIST_DEL	0x51 /* delete an item, followed by the file name */
#define EV_PLIST_MOVE	0x52 /* move an item, followed by 2 file names */
#define EV_PLIST_CLEAR	0x53 /* clear the playlist */

/* These events, though similar to the four previous are caused by server
 * which takes care of clients' queue synchronization. */
#define EV_QUEUE_ADD	0x54
#define EV_QUEUE_DEL	0x55
#define EV_QUEUE_MOVE	0x56
#define EV_QUEUE_CLEAR	0x57

/* State of the server. */
#define STATE_PLAY	0x01
#define STATE_STOP	0x02
#define STATE_PAUSE	0x03

/* Definition of server commands. */
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
#define CMD_SEND_PLIST_EVENTS 0x1d /* request for playlist events */
#define CMD_PREV	0x20 /* start playing previous song if available */
#define CMD_SEND_PLIST	0x21 /* send the playlist to the requesting client */
#define CMD_GET_PLIST	0x22 /* get the playlist from one of the clients */
#define CMD_CAN_SEND_PLIST	0x23 /* mark the client as able to send
					playlist */
#define CMD_CLI_PLIST_ADD	0x24 /* add an item to the client's playlist */
#define CMD_CLI_PLIST_DEL	0x25 /* delete an item from the client's
					playlist */
#define CMD_CLI_PLIST_CLEAR	0x26 /* clear the client's playlist */
#define CMD_GET_SERIAL	0x27 /* get an unique serial number */
#define CMD_PLIST_SET_SERIAL	0x28 /* assign a serial number to the server's
					playlist */
#define CMD_LOCK	0x29 /* acquire a lock */
#define CMD_UNLOCK	0x2a /* release the lock */
#define CMD_PLIST_GET_SERIAL	0x2b /* get the serial number of the server's
					playlist */
#define CMD_GET_TAGS	0x2c /* get tags for the currently played file */
#define CMD_TOGGLE_MIXER_CHANNEL	0x2d /* toggle the mixer channel */
#define CMD_GET_MIXER_CHANNEL_NAME	0x2e /* get the mixer channel's name */
#define CMD_GET_FILE_TAGS	0x2f	/* get tags for the specified file */
#define CMD_ABORT_TAGS_REQUESTS	0x30	/* abort previous CMD_GET_FILE_TAGS
					   requests up to some file */
#define CMD_CLI_PLIST_MOVE	0x31	/* move an item */
#define CMD_LIST_MOVE		0x32	/* move an item */
#define CMD_GET_AVG_BITRATE	0x33	/* get the average bitrate */

#define CMD_TOGGLE_SOFTMIXER    0x34    /* toggle use of softmixer */
#define CMD_TOGGLE_EQUALIZER    0x35    /* toggle use of equalizer */
#define CMD_EQUALIZER_REFRESH   0x36    /* refresh EQ-presets */
#define CMD_EQUALIZER_PREV      0x37    /* select previous eq-preset */
#define CMD_EQUALIZER_NEXT      0x38    /* select next eq-preset */

#define CMD_TOGGLE_MAKE_MONO    0x39    /* toggle mono mixing */
#define CMD_JUMP_TO	0x3a /* jumps to a some position in the current stream */
#define CMD_QUEUE_ADD	0x3b /* add an item to the queue */
#define CMD_QUEUE_DEL	0x3c /* delete an item from the queue */
#define CMD_QUEUE_MOVE	0x3d /* move an item in the queue */
#define CMD_QUEUE_CLEAR	0x3e /* clear the queue */
#define CMD_GET_QUEUE	0x3f /* request the queue from the server */

char *socket_name ();
int get_int (int sock, int *i);
enum noblock_io_status get_int_noblock (int sock, int *i);
int send_int (int sock, int i);
char *get_str (int sock);
int send_str (int sock, const char *str);
int get_time (int sock, time_t *i);
int send_time (int sock, time_t i);
int send_item (int sock, const struct plist_item *item);
struct plist_item *recv_item (int sock);
struct file_tags *recv_tags (int sock);
int send_tags (int sock, const struct file_tags *tags);

void event_queue_init (struct event_queue *q);
void event_queue_free (struct event_queue *q);
void free_event_data (const int type, void *data);
struct event *event_get_first (struct event_queue *q);
void event_pop (struct event_queue *q);
void event_push (struct event_queue *q, const int event, void *data);
int event_queue_empty (const struct event_queue *q);
enum noblock_io_status event_send_noblock (int sock, struct event_queue *q);
void free_tag_ev_data (struct tag_ev_response *d);
void free_move_ev_data (struct move_ev_data *m);
struct move_ev_data *move_ev_data_dup (const struct move_ev_data *m);
struct move_ev_data *recv_move_ev_data (int sock);

#ifdef __cplusplus
}
#endif

#endif
