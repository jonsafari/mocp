#ifndef LISTS_H
#define LISTS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lists_s_strs;
typedef struct lists_s_strs lists_t_strs;
typedef int lists_t_compare (const void *, const void *);

/* List administration functions. */
lists_t_strs *lists_strs_new (int reserve);
void lists_strs_free (lists_t_strs *list);
int lists_strs_size (const lists_t_strs *list);
_Bool lists_strs_empty (const lists_t_strs *list);

/* List member access functions. */
char *lists_strs_at (const lists_t_strs *list, int index);
char *lists_strs_cat (const lists_t_strs *list);

/* List mutating functions. */
void lists_strs_sort (lists_t_strs *list, lists_t_compare *compare);
void lists_strs_reverse (lists_t_strs *list);

/* Ownership transferring functions. */
void lists_strs_push (lists_t_strs *list, char *s);
char *lists_strs_pop (lists_t_strs *list);
char *lists_strs_swap (lists_t_strs *list, int index, char *s);

/* Ownership preserving functions. */
void lists_strs_append (lists_t_strs *list, char *s);
void lists_strs_remove (lists_t_strs *list);
void lists_strs_replace (lists_t_strs *list, int index, char *s);

#ifdef __cplusplus
}
#endif

#endif
