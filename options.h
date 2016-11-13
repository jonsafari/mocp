#ifndef OPTIONS_H
#define OPTIONS_H

#include "lists.h"

#ifdef __cplusplus
extern "C" {
#endif

enum option_type
{
	OPTION_FREE = 0,
	OPTION_INT  = 1,
	OPTION_BOOL = 2,
	OPTION_STR  = 4,
	OPTION_SYMB = 8,
	OPTION_LIST = 16,
	OPTION_ANY  = 255
};

int options_get_int (const char *name);
bool options_get_bool (const char *name);
char *options_get_str (const char *name);
char *options_get_symb (const char *name);
lists_t_strs *options_get_list (const char *name);
void options_set_int (const char *name, const int value);
void options_set_bool (const char *name, const bool value);
void options_set_str (const char *name, const char *value);
void options_set_symb (const char *name, const char *value);
void options_set_list (const char *name, const char *value, bool append);
bool options_set_pair (const char *name, const char *value, bool append);
void options_init ();
void options_parse (const char *config_file);
void options_free ();
void options_ignore_config (const char *name);
int options_check_str (const char *name, const char *val);
int options_check_symb (const char *name, const char *val);
int options_check_int (const char *name, const int val);
int options_check_bool (const char *name, const bool val);
int options_check_list (const char *name, const char *val);
int options_was_defaulted (const char *name);
enum option_type options_get_type (const char *name);

#ifdef __cplusplus
}
#endif

#endif
