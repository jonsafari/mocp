#ifndef OPTIONS_H
#define OPTIONS_H

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
	OPTION_ANY  = 255
};

int options_get_int (const char *name);
_Bool options_get_bool (const char *name);
char *options_get_str (const char *name);
char *options_get_symb (const char *name);
void option_set_int (const char *name, const int value);
void option_set_bool (const char *name, const _Bool value);
void option_set_str (const char *name, const char *value);
void option_set_symb (const char *name, const char *value);
void options_init ();
void options_parse (const char *config_file);
void options_free ();
void option_ignore_config (const char *name);
int check_str_option (const char *name, const char *val);
int check_symb_option (const char *name, const char *val);
int check_int_option (const char *name, const int val);
int check_bool_option (const char *name, const _Bool val);
enum option_type options_get_type (const char *name);

#ifdef __cplusplus
}
#endif

#endif
