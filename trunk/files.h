#ifndef FILES_H
#define FILES_H

int read_directory (const char *directory, struct plist *plist,
		char ***dir_tab, int *ndirs);
void read_directory_recurr (const char *directory, struct plist *plist);
void free_dir_tab (char **tab, int i);
void make_titles_file (struct plist *plist);
void make_titles_tags (struct plist *plist);
void read_tags (struct plist *plist);
void resolve_path (char *buf, const int size, char *file);
struct file_tags *read_file_tags (char *file);

#endif
