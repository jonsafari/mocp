#ifndef FILES_H
#define FILES_H

#include "playlist.h"

enum file_type
{
	F_DIR,
	F_SOUND,
	F_PLAYLIST,
	F_OTHER
};

struct file_list
{
	int num;		/* Number of elements on the list */
	int allocated;		/* Number of allocated elements */
	char **items;
};

int read_directory (const char *directory, struct file_list *dirs,
		struct file_list *playlists, struct plist *plist);
void read_directory_recurr (const char *directory, struct plist *plist);
void make_titles_file (struct plist *plist);
void make_titles_tags (struct plist *plist);
void read_tags (struct plist *plist);
void resolve_path (char *buf, const int size, char *file);
struct file_tags *read_file_tags (char *file);
char *ext_pos (char *file);
void file_list_free (struct file_list *list);
struct file_list *file_list_new ();
enum file_type file_type (char *file);

#endif
