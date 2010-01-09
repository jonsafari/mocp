#ifndef TAGS_CACHE_H
#define TAGS_CACHE_H

#include <pthread.h>
#include <db.h>

#include "playlist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Element of a requests queue. */
struct request_queue_node
{
	struct request_queue_node *next;
	char *file; /* file that this request is for (malloc()ed) */
	int tags_sel; /* which tags to read (TAGS_*) */
};

/* Requests queue. */
struct request_queue
{
	struct request_queue_node *head;
	struct request_queue_node *tail;
};

struct tags_cache
{
	/* BerkeleyDB's stuff for storing cache. */
	DB_ENV *db_env;
	DB *db;
	u_int32_t locker;

	int max_items;		/* maximum number of items in the cache. */
	struct request_queue queues[CLIENTS_MAX]; /* requests queues for each
						     client */
	int stop_reader_thread; /* request for stopping read thread (if
				   non-zero) */
	pthread_cond_t request_cond; /* condition for signalizing new
					requests */
	pthread_mutex_t mutex; /* mutex for all above data (except db because
				  it's thread-safe) */
	pthread_t reader_thread; /* tid of the reading thread */
};

void tags_cache_clear_queue (struct tags_cache *c, const int client_id);
void tags_cache_add_request (struct tags_cache *c, const char *file,
		const int tags_sel, const int client_id);
struct file_tags *tags_cache_get_immediate (struct tags_cache *c,
		const char *file, const int tags_sel);
void tags_cache_destroy (struct tags_cache *c);
void tags_cache_init (struct tags_cache *c, const size_t max_size);
void tags_cache_clear_up_to (struct tags_cache *c, const char *file,
		const int client_id);
void tags_cache_save (struct tags_cache *c, const char *file_name);
void tags_cache_load (struct tags_cache *c, const char *file_name);

#ifdef __cplusplus
}
#endif

#endif
