#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <pthread.h>
#include <sys/types.h>

/* Flags for the info decoder function. */
enum tags_select
{
	TAGS_COMMENTS	= 0x01, /* artist, title, etc. */
	TAGS_TIME	= 0x02 /* time of the file. */
};

struct file_tags
{
	char *title;
	char *artist;
	char *album;
	int track;
	int time;
	int filled; /* Which tags are filled: TAGS_COMMENTS, TAGS_TIME. */
};

struct plist_item
{
	char *file;
	long file_hash;		/* hashed file or -1 */
	char *title;		/* points to title_file or title_tags */
	char *title_file;	/* title based on the file name */
	char *title_tags;	/* title based on the tags */
	struct file_tags *tags;
	short deleted;
	time_t mtime;		/* modification time */
};

struct plist
{
	int num;		/* Number of elements on the list */
	int allocated;		/* Number of allocated elements */
	int not_deleted;	/* Number of not deleted items */
	pthread_mutex_t mutex;
	struct plist_item *items;
};

void plist_init (struct plist *plist);
int plist_add (struct plist *plist, const char *file_name);
int plist_add_from_item (struct plist *plist, const struct plist_item *item);
char *plist_get_file (struct plist *plist, int i);
int plist_next (struct plist *plist, int num);
int plist_prev (struct plist *plist, int num);
void plist_clear (struct plist *plist);
void plist_delete (struct plist *plist, const int num);
void plist_free (struct plist *plist);
void plist_sort_fname (struct plist *plist);
int plist_find_fname (struct plist *plist, const char *file);
struct file_tags *tags_new ();
void tags_free (struct file_tags *tags);
char *build_title (const struct file_tags *tags);
int plist_count (struct plist *plist);
void plist_set_title_tags (struct plist *plist, const int num,
		const char *title);
void plist_set_title_file (struct plist *plist, const int num,
		const char *title);
void plist_set_file (struct plist *plist, const int num, const char *file);
int plist_deleted (const struct plist *plist, const int num);
void plist_cat (struct plist *a, const struct plist *b);
void sync_plists_data (struct plist *dst, struct plist *src);
void update_item_time (struct plist_item *item, const int time);
void update_file (struct plist_item *item);
int get_item_time (const struct plist *plist, const int i);
int plist_total_time (const struct plist *plisti, int *all_files);
void plist_shuffle (struct plist *plist);
void plist_swap_first_fname (struct plist *plist, const char *fname);
struct plist_item *plist_new_item ();
void plist_free_item_fields (struct plist_item *item);

#endif
