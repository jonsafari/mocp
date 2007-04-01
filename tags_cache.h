#ifndef TAGS_CACHE_H
#define TAGS_CACHE_H

#include <pthread.h>

#include "playlist.h"

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

/* Element of the cache pool. */
struct cache_list_node
{
	struct cache_list_node *next;
	char *file;
	time_t mod_time;		/* last modification time of the file */
	struct file_tags *tags;
	size_t size; /* number of bytes allocated for this node (file name and
			tags) */
	int during_operation; /* If set to != 0 there is operation pending on
				 this node (reading tags). */
};

/* List of items in the cache - olders are first. */
struct cache_list
{
	struct cache_list_node *head;
	struct cache_list_node *tail;
};

struct tags_cache
{
	struct rb_tree search_tree; /* search tree used for fast searching
				       by file name */
	struct cache_list cache;
	struct request_queue queues[CLIENTS_MAX]; /* requests queues for each
						     client */
	size_t size; /* number of bytes allocated for the cache */
	size_t max_size; /* maximum allowed cache size */
	int stop_reader_thread; /* request for stopping read thread (if
				   non-zero) */
	pthread_cond_t request_cond; /* condition for signalizing new
					requests */
	pthread_cond_t response_cond; /* condition for signalizing a cache
					 node read */
	pthread_mutex_t mutex; /* mutex for all above data */
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

#endif
