#ifndef PLAYLIST_FILE_H
#define PLAYLIST_FILE_H

int plist_load_m3u (struct plist *plist, const char *fname);
int is_plist_file (char *name);

#endif
