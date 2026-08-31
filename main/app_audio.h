// Audio output and the step sequencer that feeds it.
//
// The sequencer runs inside the audio task so note starts land on the exact
// sample, not on a block boundary. The UI task only ever hands over a new
// program text; parsing happens under a mutex and never in the render path.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define APP_SEQ_MAX_LINES 16
#define APP_SEQ_MAX_STEPS 64

// Starts the codec, the I2S channel and the audio task.
esp_err_t app_audio_start(void);

// Replaces the running program. text is the whole editor buffer; lines that do
// not parse are reported through app_audio_get_error and otherwise ignored, so
// a typo never stops the music.
void app_audio_eval(char const* text);

// An evaluation does not take effect in the middle of a bar. The new program is
// parsed immediately and swapped in at the next cycle boundary, which is what
// makes editing while playing sound deliberate rather than accidental.
bool app_audio_pending(void);

void  app_audio_set_playing(bool playing);
bool  app_audio_is_playing(void);
void  app_audio_set_bpm(float bpm);
float app_audio_get_bpm(void);
void  app_audio_set_master(float gain);
float app_audio_get_master(void);

// Playhead position in sixteenth steps since the transport started.
int64_t app_audio_get_step(void);
int     app_audio_get_voices(void);
// Fraction of the audio budget used by the last render, 0..1 and up.
float   app_audio_get_load(void);

// Parse status of the last app_audio_eval. Empty string means no error.
char const* app_audio_get_error(void);
// Number of lines the last app_audio_eval turned into playing parts.
int         app_audio_get_line_count(void);
// Step length of a parsed line, 0 if that editor line is not a part.
int         app_audio_get_line_steps(int editor_line);

// Read the parsed program, for drawing the step lanes.
int  app_audio_get_part_count(void);
int  app_audio_get_part_steps(int part);
bool app_audio_get_part_step_on(int part, int step);
int  app_audio_get_part_editor_line(int part);
void app_audio_get_part_name(int part, char* out, size_t len);
