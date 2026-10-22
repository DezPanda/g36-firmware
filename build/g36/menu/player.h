/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* The G36 media player engine: plays one video or music file. FFmpeg reads and
 * decodes, ALSA plays the sound, and video frames are handed to the menu when the
 * sound reaches them. Only one file plays at a time. */
#ifndef G36_PLAYER_H
#define G36_PLAYER_H

#include "gfx.h"

typedef struct
{
   int has_video, has_audio;
   int video_w, video_h;        /* the picture as it should look (pixel shape applied) */
   double duration;             /* seconds, 0 if unknown */
   char title[256], artist[256], album[256];
   image_t *cover;              /* album art inside the file, or NULL */
   int src_w, src_h;            /* the frames as decoded */
   double fps;
   char vcodec[32], acodec[32];
} player_info_t;

/* Opens path and starts playing at start_at seconds. 0, or -1 with player_error(). */
int  player_open(const char *path, double start_at);
void player_close(void);
int  player_is_open(void);
const char *player_error(void);
const player_info_t *player_info(void);

double player_position(void);   /* seconds */
int  player_paused(void);
void player_set_paused(int paused);
void player_seek(double seconds);           /* to a position */
void player_seek_by(double seconds);        /* forward (+) or back (-), adding up while held */
int  player_ended(void);
int  player_waiting(void);                  /* after opening or a seek: the picture isn't ready yet */                    /* everything has been played */

/* The video: when a new frame is due, puts it in the buffer and returns 1. */
int  player_video_frame(void);
gfx_video_t *player_video(void);
/* milliseconds until the next frame is due (for the menu's wait), at most max_ms */
int  player_wait_ms(int max_ms);

/* Playback speed, 0.5 to 2 (the sound keeps its pitch). */
double player_speed(void);
void player_set_speed(double speed);

/* Sound only: no picture is read or decoded, just the sound (the menu turns the screen off).
 * Reopens the file where it was; on until set off again, also for the next files. */
int  player_sound_only(void);
void player_set_sound_only(int on);

/* Sound tracks in the file. Changing one reopens the file where it was. */
int  player_audio_count(void);
const char *player_audio_name(int i);
int  player_audio_current(void);
void player_set_audio(int i);

/* Subtitles: the file's own text or picture tracks, and .srt/.ass/.ssa/.vtt files
 * beside it with the same name (Show.srt, Show.en.srt). -1 = none shown. */
int  player_sub_count(void);
const char *player_sub_name(int i);
int  player_sub_current(void);
void player_set_sub(int i);
typedef struct
{
   char text[600];              /* lines split by \n, or "" for a picture */
   image_t *img;                /* a picture subtitle (DVD, Blu-ray), or NULL */
   int x, y, w, h;              /* the picture's place on a canvas_w x canvas_h frame */
   int canvas_w, canvas_h;
   unsigned id;                 /* changes when what's shown changes */
} player_sub_t;
/* what to show now: 1 and fills out, or 0 for nothing */
int  player_subtitle(player_sub_t *out);

/* frames shown and skipped since the last call */
void player_stats(int *shown, int *dropped);

/* For the list: a frame about a quarter of the way into a video, fitted into
 * max_w x max_h, saved as a JPEG at out. 0, or -1 if there's no picture to take. */
int video_thumbnail(const char *path, const char *out, int max_w, int max_h);

#endif
