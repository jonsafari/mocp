#ifndef PLAYLIST_H
#define PLAYLIST_H

#include <pthread.h>

struct file_tags
{
	char *title;
	char *artist;
	char *album;
	int track;
};

struct plist_item
{
	char *file;
	char *title;
	struct file_tags *tags;
	short deleted;
};

struct plist
{
	int num;		/* Number of elements on the list */
	int allocated;		/* Number of allocated elements */
	pthread_mutex_t mutex;
	struct plist_item *items;
};

void plist_init (struct plist *plist);
int plist_add (struct plist *plist, const char *file_name);
char *plist_get_file (struct plist *plist, int i);
int plist_next (struct plist *plist, int num);
void plist_clear (struct plist *plist);
void plist_delete (struct plist *plist, const int num);
void plist_free (struct plist *plist);
int plist_find (struct plist *plist, int num);
void plist_sort_fname (struct plist *plist);
int plist_find_fname (const struct plist *plist, const char *file);
struct file_tags *tags_new ();
void tags_free (struct file_tags *tags);
char *build_title (const struct file_tags *tags);

#endif
