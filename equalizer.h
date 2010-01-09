#ifndef EQUALIZER_H
#define EQUALIZER_H

#ifdef __cplusplus
extern "C" {
#endif

#define TWOPI (2.0 * M_PI)

#define swap_32bit_endianess(i32) \
  ( ((i32&0x000000FF)<<24) | ((i32&0x0000FF00)<<8)| \
  ((i32&0x00FF0000)>>8) | ((i32&0xFF000000)>>24) ) 

#define swap_16bit_endianess(i16) \
  ( ((i16&0x00FF)<<8) | ((i16&0xFF00)>>8) )

#define NEWLINE 0x0A
#define CRETURN 0x0D
#define SPACE   0x20

#define EQSET_HEADER "EQSET"

#define EQUALIZER_CFG_ACTIVE "Active:"
#define EQUALIZER_CFG_PRESET "Preset:"
#define EQUALIZER_CFG_MIXIN "Mixin:"

#define EQUALIZER_SAVE_FILE "equalizer"
#define EQUALIZER_SAVE_OPTION "Equalizer_SaveState"


char *skip_line(char *s);
char *skip_whitespace(char *s);
char *word(char *s, char *buf, int *len, int nmax);
int read_float(char *s, float *f, char **endp);

typedef struct t_biquad t_biquad;

struct t_biquad
{
  float a0, a1, a2, a3, a4;
  float x1, x2, y1, y2;
  float cf, bw, gain, srate;
  int israte;
};

typedef struct t_eq_setup t_eq_setup;

struct t_eq_setup
{
  char *name;
  float preamp;
  int bcount;
  float *cf;
  float *bw;
  float *dg;
};

typedef struct t_eq_set t_eq_set;

struct t_eq_set
{
  char *name;
  int channels;
  float preamp;
  int bcount;
  t_biquad *b;
};

typedef struct t_eq_set_list t_eq_set_list;

struct t_eq_set_list
{
  t_eq_set *set;
  t_eq_set_list *prev, *next;
};

typedef struct t_active_set t_active_set;

struct t_active_set
{
  int srate;
  t_eq_set *set;
};

typedef struct t_eq_settings t_eq_settings;

struct t_eq_settings
{
  char *preset_name;
  int bcount;
  float *gain;
  t_eq_settings *next;
};

void equalizer_init();
void equalizer_shutdown();
void equalizer_process_buffer(char *buf, size_t size, const struct sound_params *sound_params);
void equalizer_refresh();
int equalizer_is_active();
int equalizer_set_active();
char *equalizer_current_eqname();
void equalizer_next();
void equalizer_prev();

#ifdef __cplusplus
}
#endif

#endif
