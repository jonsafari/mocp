#ifndef INTERFACE_ELEMENTS_H
#define INTERFACE_ELEMENTS_H

#include "files.h"
#include "keys.h"

void windows_init ();
void windows_end ();
void iface_set_option_state (const char *name, const int value);
void iface_set_mixer_name (const char *name);
void iface_set_status (const char *msg);
void iface_set_dir_content (const struct plist *files,
		const struct file_list *dirs,
		const struct file_list *playlists);
void iface_set_curr_item_title (const char *title);
void iface_set_dir_title (const char *title);
int iface_get_char ();
int iface_in_help ();
int iface_key_is_resize (const int ch);
void iface_menu_key (const enum key_cmd cmd);
enum file_type iface_curritem_get_type ();
int iface_in_dir_menu ();
char *iface_get_curr_file ();
void iface_update_item (const struct plist *plist, const int n);
void iface_set_curr_time (const int time);
void iface_set_total_time (const int time);
void iface_set_state (const int state);
void iface_set_bitrate (const int bitrate);
void iface_set_rate (const int rate);
void iface_set_channels (const int channels);
void iface_set_played_file (const char *file);
void iface_set_played_file_title (const char *title);
void iface_tick ();

#endif
