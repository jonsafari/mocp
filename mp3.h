#ifndef MP3_H
#define MP3_H

#include "buf.h"

void mp3_play (const char *file, struct buf *out_buf);
void mp3_info (const char *file_name, struct file_tags *info);

#endif
