#ifndef RBTREE_H
#define RBTREE_H

#ifdef __cplusplus
extern "C" {
#endif

enum rb_color { RB_RED, RB_BLACK };

struct rb_node
{
	struct rb_node *left;
	struct rb_node *right;
	struct rb_node *parent;
	enum rb_color color;
	void *data;
};

struct rb_tree
{
	struct rb_node *root;

	/* compare function for two data elements */
	int (*cmp_func)(const void *a, const void *b, void *adata);

	/* compare function for data element and a key value */
	int (*cmp_key_func)(const void *key, const void *data, void *adata);

	/* pointer to additional data passed to compare functions */
	void *adata;
};

void rb_clear (struct rb_tree *t);
void rb_init_tree (struct rb_tree *t,
		int (*cmp_func)(const void *a, const void *b, void *adata),
		int (*cmp_key_func)(const void *key, const void *data,
			void *adata),
		void *adata);
void rb_delete (struct rb_tree *t, const void *key);
struct rb_node *rb_next (struct rb_node *x);
struct rb_node *rb_min (struct rb_tree *t);
int rb_is_null (const struct rb_node *n);
struct rb_node *rb_search (struct rb_tree *t, const void *key);
void rb_insert (struct rb_tree *t, void *data);

#ifdef __cplusplus
}
#endif

#endif
