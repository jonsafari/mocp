#ifndef FILE_TYPES_H
#define FILE_TYPES_H

#include "playlist.h"
#include "buf.h"

typedef void (*play_func_t)(const char *, struct buf *out_buf);
typedef void (*info_func_t)(const char *, struct file_tags *);

info_func_t get_info_func (char *file);
play_func_t get_play_func (char *file);
int is_sound_file (char *name);

#endif
