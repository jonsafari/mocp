#ifndef LISTS_H
#define LISTS_H

struct lists_s_strs;
typedef struct lists_s_strs lists_t_strs;
typedef int lists_t_compare (const void *, const void *);

lists_t_strs *lists_strs_new (int reserve);
void lists_strs_free (lists_t_strs *list);
int lists_strs_size (const lists_t_strs *list);
void lists_strs_append (lists_t_strs *list, char *str);
char *lists_strs_at (const lists_t_strs *list, int index);
void lists_strs_sort (lists_t_strs *list, lists_t_compare *compare);

#endif