#ifndef PROTOCOL_H
#define PROTOCOL_H

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
#define CMD_GET_STIME	0x0e /* get the stream time */
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

char *socket_name ();
int get_int (int sock, int *i);
int send_int (int sock, int i);
char *get_str (int sock);
int send_str (int sock, const char *str);

#endif
