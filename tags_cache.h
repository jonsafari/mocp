#ifndef TAGS_CACHE_H
#define TAGS_CACHE_H

#include <pthread.h>
#ifdef HAVE_DB_H
#include <db.h>
#endif

#include "playlist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Requests queue. */
struct request_queue_node;
struct request_queue
{
	struct request_queue_node *head;
	struct request_queue_node *tail;
};

struct tags_cache
{
	/* BerkeleyDB's stuff for storing cache. */
#ifdef HAVE_DB_H
	DB_ENV *db_env;
	DB *db;
	u_int32_t locker;
#endif

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

/* Administrative functions: */
void tags_cache_init (struct tags_cache *c, size_t max_size);
void tags_cache_destroy (struct tags_cache *c);

/* Request queue manipulation functions: */
void tags_cache_clear_queue (struct tags_cache *c, int client_id);
void tags_cache_clear_up_to (struct tags_cache *c, const char *file,
                                                      int client_id);

/* Cache DB manipulation functions: */
void tags_cache_load (struct tags_cache *c, const char *cache_dir);
void tags_cache_save (struct tags_cache *c, const char *cache_dir);
void tags_cache_add_request (struct tags_cache *c, const char *file,
                                        int tags_sel, int client_id);
struct file_tags *tags_cache_get_immediate (struct tags_cache *c,
                                  const char *file, int tags_sel);

#ifdef __cplusplus
}
#endif

#endif
