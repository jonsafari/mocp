#ifndef OPTIONS_H
#define OPTIONS_H

char *options_get_str (const char *name);
int options_get_int (const char *name);
void option_set_int (const char *name, const int value);
void option_set_str (const char *name, const char *value);
void options_init ();
void options_parse (const char *config_file);
void options_free ();
void option_ignore_config (const char *name);
int check_str_option (const char *name, const char *val);
int check_int_option (const char *name, const int val);

#endif
