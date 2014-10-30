#ifndef LISTS_H
#define LISTS_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lists_strs lists_t_strs;
typedef int lists_t_compare (const void *, const void *);

/* List administration functions. */
lists_t_strs *lists_strs_new (int reserve);
void lists_strs_clear (lists_t_strs *list);
void lists_strs_free (lists_t_strs *list);
int lists_strs_size (const lists_t_strs *list);
int lists_strs_capacity (const lists_t_strs *list);
bool lists_strs_empty (const lists_t_strs *list);

/* List member access functions. */
char *lists_strs_at (const lists_t_strs *list, int index);

/* List mutating functions. */
void lists_strs_sort (lists_t_strs *list, lists_t_compare *compare);
void lists_strs_reverse (lists_t_strs *list);

/* Ownership transferring functions. */
void lists_strs_push (lists_t_strs *list, char *s);
char *lists_strs_pop (lists_t_strs *list);
char *lists_strs_swap (lists_t_strs *list, int index, char *s);

/* Ownership preserving functions. */
void lists_strs_append (lists_t_strs *list, const char *s);
void lists_strs_remove (lists_t_strs *list);
void lists_strs_replace (lists_t_strs *list, int index, char *s);

/* Helper functions. */
int lists_strs_split (lists_t_strs *list, const char *s, const char *delim);
int lists_strs_tokenise (lists_t_strs *list, const char *s);
char *lists_strs_fmt (const lists_t_strs *list, const char *fmt);
char *lists_strs_cat (const lists_t_strs *list);
char **lists_strs_save (const lists_t_strs *list);
int lists_strs_load (lists_t_strs *list, const char **saved);
int lists_strs_find (lists_t_strs *list, const char *sought);
bool lists_strs_exists (lists_t_strs *list, const char *sought);

#ifdef __cplusplus
}
#endif

#endif
