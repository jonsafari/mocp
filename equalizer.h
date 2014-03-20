#ifndef EQUALIZER_H
#define EQUALIZER_H

#ifdef __cplusplus
extern "C" {
#endif

void equalizer_init();
void equalizer_shutdown();
void equalizer_process_buffer(char *buf, size_t size, const struct sound_params *sound_params);
void equalizer_refresh();
int equalizer_is_active();
int equalizer_set_active(int active);
char *equalizer_current_eqname();
void equalizer_next();
void equalizer_prev();

#ifdef __cplusplus
}
#endif

#endif
