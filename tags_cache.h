#ifndef TAGS_CACHE_H
#define TAGS_CACHE_H

#ifdef __cplusplus
extern "C" {
#endif

struct file_tags;
struct tags_cache;

/* Administrative functions: */
struct tags_cache *tags_cache_new (size_t max_size);
void tags_cache_free (struct tags_cache *c);

/* Request queue manipulation functions: */
void tags_cache_clear_queue (struct tags_cache *c, int client_id);
void tags_cache_clear_up_to (struct tags_cache *c, const char *file,
                                                      int client_id);

/* Cache DB manipulation functions: */
void tags_cache_load (struct tags_cache *c, const char *cache_dir);
void tags_cache_add_request (struct tags_cache *c, const char *file,
                                        int tags_sel, int client_id);
struct file_tags *tags_cache_get_immediate (struct tags_cache *c,
                                  const char *file, int tags_sel);

#ifdef __cplusplus
}
#endif

#endif
