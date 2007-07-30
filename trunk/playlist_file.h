#ifndef PLAYLIST_FILE_H
#define PLAYLIST_FILE_H

int plist_load (struct plist *plist, const char *fname, const char *cwd,
		const int load_serial);
int plist_save (struct plist *plist, const char *file, const char *cwd,
		const int save_serial);
int is_plist_file (const char *name);

#endif
