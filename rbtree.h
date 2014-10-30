#ifndef RBTREE_H
#define RBTREE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef int rb_t_compare (const void *, const void *, const void *);
typedef int rb_t_compare_key (const void *, const void *, const void *);

struct rb_tree;
struct rb_node;

/* Whole-of-tree functions. */
struct rb_tree *rb_tree_new (rb_t_compare *cmp_fn,
                             rb_t_compare_key *cmp_key_fn,
                             const void *adata);
void rb_tree_clear (struct rb_tree *t);
void rb_tree_free (struct rb_tree *t);

/* Individual node functions. */
void rb_delete (struct rb_tree *t, const void *key);
struct rb_node *rb_next (struct rb_node *x);
struct rb_node *rb_min (struct rb_tree *t);
int rb_is_null (const struct rb_node *n);
const void *rb_get_data (const struct rb_node *n);
void rb_set_data (struct rb_node *n, const void *data);
struct rb_node *rb_search (struct rb_tree *t, const void *key);
void rb_insert (struct rb_tree *t, void *data);

#ifdef __cplusplus
}
#endif

#endif
