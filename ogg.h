#ifndef OGG_H
#define OGG_H

#include "buf.h"

void ogg_play (const char *file, struct buf *out_buf);
void ogg_info (const char *file_name, struct file_tags *info);

#endif
