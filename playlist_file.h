#ifndef PLAYLIST_FILE_H
#define PLAYLIST_FILE_H

int plist_load (struct plist *plist, const char *fname, const char *cwd);
int is_plist_file (char *name);

#endif
