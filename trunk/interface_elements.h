#ifndef INTERFACE_ELEMENTS_H
#define INTERFACE_ELEMENTS_H

#include "files.h"
#include "keys.h"

/* Interface's menus */
enum iface_menu
{
	IFACE_MENU_PLIST,
	IFACE_MENU_DIR
};

enum entry_type
{
	ENTRY_SEARCH,
	ENTRY_PLIST_SAVE,
	ENTRY_GO_DIR,
	ENTRY_GO_URL,
	ENTRY_PLIST_OVERWRITE
};

void sec_to_min (char *buff, const int seconds);

void windows_init ();
void windows_end ();
void iface_set_option_state (const char *name, const int value);
void iface_set_mixer_name (const char *name);
void iface_set_status (const char *msg);
void iface_set_dir_content (const enum iface_menu iface_menu,
		const struct plist *files,
		const struct file_list *dirs,
		const struct file_list *playlists);
void iface_update_dir_content (const enum iface_menu iface_menu,
		const struct plist *files,
		const struct file_list *dirs,
		const struct file_list *playlists);
void iface_set_curr_item_title (const char *title);
int iface_get_char ();
int iface_key_is_resize (const int ch);
void iface_menu_key (const enum key_cmd cmd);
enum file_type iface_curritem_get_type ();
int iface_in_dir_menu ();
int iface_in_plist_menu ();
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
void iface_set_mixer_value (const int value);
void iface_tick ();
void iface_switch_to_plist ();
void iface_switch_to_dir ();
void iface_add_to_plist (const struct plist *plist, const int num);
void iface_error (const char *msg);
void iface_resize ();
void iface_refresh ();
void iface_update_show_time ();
void iface_update_show_format ();
void iface_clear_plist ();
void iface_del_plist_item (const char *file);
enum entry_type iface_get_entry_type ();
int iface_in_entry ();
void iface_make_entry (const enum entry_type type);
void iface_entry_handle_key (const int ch);
void iface_entry_set_text (const char *text);
char *iface_entry_get_text ();
void iface_entry_history_add ();
void iface_entry_disable ();
void iface_entry_set_file (const char *file);
char *iface_entry_get_file ();
void iface_message (const char *msg);
void iface_disable_message ();
void iface_plist_set_total_time (const int time, const int for_all_files);
void iface_set_title (const enum iface_menu menu, const char *title);
void iface_select_file (const char *file);
int iface_in_help ();
void iface_switch_to_help ();
void iface_handle_help_key (const int ch);
void iface_toggle_layout ();

#endif
