#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <sys/types.h>

#include "rbtree.h"

#ifdef __cplusplus
extern "C" {
#endif

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

enum file_type
{
	F_DIR,
	F_SOUND,
	F_URL,
	F_PLAYLIST,
	F_THEME,
	F_OTHER
};

struct plist_item
{
	char *file;
	enum file_type type;	/* type of the file (F_OTHER if not read yet) */
	char *title_file;	/* title based on the file name */
	char *title_tags;	/* title based on the tags */
	struct file_tags *tags;
	short deleted;
	time_t mtime;		/* modification time */
	int queue_pos;		/* position in the queue */
};

struct plist
{
	int num;			/* Number of elements on the list */
	int allocated;		/* Number of allocated elements */
	int not_deleted;	/* Number of non-deleted items */
	struct plist_item *items;
	int serial;		/* Optional serial number of this playlist */
	int total_time;		/* Total time for files on the playlist */
	int items_with_time;	/* Number of items for which the time is set. */

	struct rb_tree *search_tree;
};

void plist_init (struct plist *plist);
int plist_add (struct plist *plist, const char *file_name);
int plist_add_from_item (struct plist *plist, const struct plist_item *item);
char *plist_get_file (const struct plist *plist, int i);
int plist_next (struct plist *plist, int num);
int plist_prev (struct plist *plist, int num);
void plist_clear (struct plist *plist);
void plist_delete (struct plist *plist, const int num);
void plist_free (struct plist *plist);
void plist_sort_fname (struct plist *plist);
int plist_find_fname (struct plist *plist, const char *file);
struct file_tags *tags_new ();
void tags_clear (struct file_tags *tags);
void tags_copy (struct file_tags *dst, const struct file_tags *src);
struct file_tags *tags_dup (const struct file_tags *tags);
void tags_free (struct file_tags *tags);
char *build_title_with_format (const struct file_tags *tags, const char *fmt);
char *build_title (const struct file_tags *tags);
int plist_count (const struct plist *plist);
void plist_set_title_tags (struct plist *plist, const int num,
		const char *title);
void plist_set_title_file (struct plist *plist, const int num,
		const char *title);
void plist_set_file (struct plist *plist, const int num, const char *file);
int plist_deleted (const struct plist *plist, const int num);
void plist_cat (struct plist *a, struct plist *b);
void update_file (struct plist_item *item);
void plist_set_item_time (struct plist *plist, const int num, const int time);
int get_item_time (const struct plist *plist, const int i);
int plist_total_time (const struct plist *plisti, int *all_files);
void plist_shuffle (struct plist *plist);
void plist_swap_first_fname (struct plist *plist, const char *fname);
struct plist_item *plist_new_item ();
void plist_free_item_fields (struct plist_item *item);
void plist_set_serial (struct plist *plist, const int serial);
int plist_get_serial (const struct plist *plist);
int plist_last (const struct plist *plist);
int plist_find_del_fname (const struct plist *plist, const char *file);
const char *plist_get_next_dead_entry (const struct plist *plist,
                                       int *last_index);
void plist_item_copy (struct plist_item *dst, const struct plist_item *src);
enum file_type plist_file_type (const struct plist *plist, const int num);
void plist_remove_common_items (struct plist *a, struct plist *b);
void plist_discard_tags (struct plist *plist);
void plist_set_tags (struct plist *plist, const int num,
		const struct file_tags *tags);
struct file_tags *plist_get_tags (const struct plist *plist, const int num);
void plist_swap_files (struct plist *plist, const char *file1,
		const char *file2);
int plist_get_position (const struct plist *plist, int num);

#ifdef __cplusplus
}
#endif

#endif
