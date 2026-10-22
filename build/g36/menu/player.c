/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* The G36 media player engine. See player.h.
 *
 * Three threads: one reads the file and sorts its packets into a video and a sound
 * queue, one decodes video into a short queue of frames, and one decodes sound and
 * writes it to ALSA. The sound sets the clock; the menu shows each video frame when
 * the clock reaches it (a file without sound runs on the system clock). Seeking bumps
 * a serial number, so everything queued before the seek is thrown away. */
#define _GNU_SOURCE
#include "player.h"
#include "subenc.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <strings.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include <alsa/asoundlib.h>
#include "../board.h"

#define OUT_RATE     48000
#define VQ_FULL      96            /* packets read ahead */
#define AQ_FULL      192
#define QUEUE_BYTES  (16 << 20)
#define FQ_MAX       4             /* decoded video frames waiting */
#define VIDEO_LEAD   0.02          /* a frame reaches the panel one refresh after it's sent */
#define READY_WAIT   30.0          /* longest the sound waits for the picture after a seek (only a
                                      * broken file should ever take this long) */
#define MAX_TRACKS   16

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- packet queues */

typedef struct pnode
{
   AVPacket *pkt;               /* NULL = the end of the file */
   int serial;
   struct pnode *next;
} pnode_t;

typedef struct
{
   pnode_t *head, *tail;
   int n, bytes;
} pktq_t;

static struct
{
   pthread_mutex_t m;
   pthread_cond_t cond;         /* any change: packets, frames, pause, seek, quit */
   pthread_t demux_th, video_th, audio_th;
   int threads;                 /* bits: 1 demux, 2 video, 4 audio */
   int quit;

   AVFormatContext *fmt;
   int vi, ai;
   AVCodecContext *vdec, *adec;
   AVRational vtb, atb;
   pktq_t vq, aq;
   int serial;                  /* bumped by each seek */
   double seek_at;              /* when the last seek (or opening) began, for the log */
   int seek_req;
   double seek_to;              /* the position asked for (shown until playing catches up) */
   double floor;                /* frames before this (after a seek) are skipped */
   int demux_eof, video_done, audio_done;

   /* decoded video frames, oldest first */
   AVFrame *fq[FQ_MAX];
   double fq_pts[FQ_MAX];
   int fq_serial[FQ_MAX];
   int fq_n;
   int shown_serial;            /* a frame of this serial is on screen */
   int ready_serial;            /* the video has a frame for this serial (the sound may start) */
   int n_shown, n_dropped;      /* frames, for the log */
   double lag_sum, lag_max;     /* how late shown frames were (s), for the log */
   double shown_at;
   double last_pts;
   gfx_video_t *vbuf;
   struct SwsContext *sws;

   /* the clock: position clk_pts at time clk_time, running unless paused */
   int clk_valid;
   double clk_pts, clk_time;
   int paused;

   snd_pcm_t *pcm;
   int can_pause;

   player_info_t info;
   char err[160];
   int open;

   double speed;                /* 1 = normal; the clock runs at this rate */
   char path[1400];
   int audio_pick;              /* reopening for another sound track: its place in the list */

   /* the tracks to choose from */
   int na, a_stream[MAX_TRACKS];
   char a_name[MAX_TRACKS][64];
   int ns, s_stream[MAX_TRACKS];          /* -1 = a file beside the video */
   char s_file[MAX_TRACKS][1400], s_name[MAX_TRACKS][64];
   int sub_cur;                 /* in the list, -1 = off */
   int si;                      /* the file's subtitle stream being read, -1 = none */
   AVCodecContext *sdec;
   int sub_w, sub_h;            /* the frame picture subtitles are placed on */

   /* subtitles waiting or showing; the menu's thread frees them (it draws the pictures) */
   struct cue
   {
      double start, end;
      char *text;
      image_t *img;
      int x, y, w, h, serial;   /* serial 0: from a file beside the video (kept through seeks) */
   } *cue;                      /* grows as needed: a film's file is read all at once */
   int ncue, cue_cap;
   unsigned cue_seq;
} P = { .m = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER };

static void q_put(pktq_t *q, AVPacket *pkt, int serial)
{
   pnode_t *n = malloc(sizeof(*n));
   if (!n)
   {
      av_packet_free(&pkt);
      return;
   }
   n->pkt = pkt;
   n->serial = serial;
   n->next = NULL;
   if (q->tail)
      q->tail->next = n;
   else
      q->head = n;
   q->tail = n;
   q->n++;
   q->bytes += pkt ? pkt->size : 0;
}

static void q_clear(pktq_t *q)
{
   while (q->head)
   {
      pnode_t *n = q->head;
      q->head = n->next;
      av_packet_free(&n->pkt);
      free(n);
   }
   q->tail = NULL;
   q->n = q->bytes = 0;
}

/* the next packet, waiting for one (called locked); 0 = quit */
static int q_get(pktq_t *q, AVPacket **pkt, int *serial)
{
   pnode_t *n;
   while (!P.quit && !q->head)
      pthread_cond_wait(&P.cond, &P.m);
   if (P.quit)
      return 0;
   n = q->head;
   q->head = n->next;
   if (!q->head)
      q->tail = NULL;
   q->n--;
   q->bytes -= n->pkt ? n->pkt->size : 0;
   *pkt = n->pkt;
   *serial = n->serial;
   free(n);
   pthread_cond_broadcast(&P.cond);
   return 1;
}

/* ---------------------------------------------------------------- clock */

static double clock_now(void)   /* locked */
{
   double v = P.clk_pts;
   if (!P.paused)
      v += (now_s() - P.clk_time) * P.speed;
   return v;
}

static void clock_set(double pts)   /* locked */
{
   P.clk_pts = pts;
   P.clk_time = now_s();
   P.clk_valid = 1;
}

/* ---------------------------------------------------------------- subtitles */

/* the words of an ASS line ("ReadOrder,Layer,Style,Name,MarginL,MarginR,MarginV,Effect,Text"),
 * without the styling in {braces}; \N is a new line */
static void ass_words(const char *a, char *out, size_t sz)
{
   size_t o = 0;
   int commas = 0, skip = !strncmp(a, "Dialogue:", 9) ? 9 : 8;
   while (*a && commas < skip)
      if (*a++ == ',')
         commas++;
   while (*a && o + 2 < sz)
   {
      if (*a == '{')
      {
         const char *e = strchr(a, '}');
         if (!e)
            break;
         a = e + 1;
         continue;
      }
      if (*a == '\\' && (a[1] == 'N' || a[1] == 'n'))
      {
         out[o++] = '\n';
         a += 2;
         continue;
      }
      if (*a == '\\' && a[1] == 'h')
      {
         out[o++] = ' ';
         a += 2;
         continue;
      }
      if (*a == '\r')
      {
         a++;
         continue;
      }
      out[o++] = *a++;
   }
   while (o && (out[o - 1] == '\n' || out[o - 1] == ' '))
      o--;
   out[o] = 0;
}

/* a picture subtitle (indexed colours) as an image */
static image_t *sub_bitmap(const AVSubtitleRect *r)
{
   image_t *img;
   const uint32_t *pal = (const uint32_t *)r->data[1];
   int x, y;
   if (r->w <= 0 || r->h <= 0 || !r->data[0] || !pal || !(img = malloc(sizeof(*img))))
      return NULL;
   img->w = r->w;
   img->h = r->h;
   if (!(img->px = malloc((size_t)r->w * r->h * 4)))
   {
      free(img);
      return NULL;
   }
   for (y = 0; y < r->h; y++)
      for (x = 0; x < r->w; x++)
         img->px[(size_t)y * r->w + x] = pal[r->data[0][(size_t)y * r->linesize[0] + x]];
   return img;
}

static void cue_free(struct cue *c)
{
   free(c->text);
   image_free(c->img);
   c->text = NULL;
   c->img = NULL;
}

/* a decoded subtitle into the list (locked). Picture subtitles often have no end: they
 * last until the next one. */
static void cue_add(const AVSubtitle *sub, double pts, double dur, int serial)
{
   double start = pts + sub->start_display_time / 1000.0, end;
   unsigned i;
   int k;
   if (sub->end_display_time > sub->start_display_time && sub->end_display_time < 3600000)
      end = pts + sub->end_display_time / 1000.0;
   else
      end = dur > 0 ? pts + dur : 1e9;
   /* open-ended pictures stop where this one starts */
   for (k = 0; k < P.ncue; k++)
      if (P.cue[k].img && P.cue[k].end >= 1e9 && P.cue[k].start < start && P.cue[k].serial == serial)
         P.cue[k].end = start;
   for (i = 0; i < sub->num_rects; i++)
   {
      AVSubtitleRect *r = sub->rects[i];
      struct cue c = { start, end, NULL, NULL, 0, 0, 0, 0, serial };
      if (r->type == SUBTITLE_BITMAP)
      {
         if (!(c.img = sub_bitmap(r)))
            continue;
         c.x = r->x;
         c.y = r->y;
         c.w = r->w;
         c.h = r->h;
      }
      else
      {
         char words[600];
         if (r->type == SUBTITLE_ASS && r->ass)
            ass_words(r->ass, words, sizeof(words));
         else
            snprintf(words, sizeof(words), "%s", r->text ? r->text : "");
         if (!words[0] || !(c.text = strdup(words)))
            continue;
      }
      if (P.ncue >= P.cue_cap)
      {
         int cap = P.cue_cap ? P.cue_cap * 2 : 256;
         struct cue *bigger = realloc(P.cue, (size_t)cap * sizeof(*bigger));
         if (!bigger)
         {
            cue_free(&c);
            return;
         }
         P.cue = bigger;
         P.cue_cap = cap;
      }
      P.cue[P.ncue++] = c;
   }
}

/* the name of a track, from its tags */
static void track_name(AVStream *st, int n, const char *kind, char *out, size_t sz)
{
   AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
   AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
   const char *l = lang && lang->value && strcmp(lang->value, "und") ? lang->value : NULL;
   if (title && title->value && title->value[0])
      snprintf(out, sz, "%s%s%s", title->value, l ? " - " : "", l ? l : "");
   else if (l)
      snprintf(out, sz, "%s %d (%s)", kind, n, l);
   else
      snprintf(out, sz, "%s %d", kind, n);
}

/* subtitle files beside the video: Name.srt, Name.en.srt, Name.ass, ... */
static void find_sub_files(const char *path)
{
   char dir[1400], base[400], *slash, *dot;
   DIR *d;
   struct dirent *de;
   snprintf(dir, sizeof(dir), "%s", path);
   if (!(slash = strrchr(dir, '/')))
      return;
   *slash = 0;
   snprintf(base, sizeof(base), "%s", slash + 1);
   if ((dot = strrchr(base, '.')))
      *dot = 0;
   if (!(d = opendir(dir)))
      return;
   while ((de = readdir(d)) && P.ns < MAX_TRACKS)
   {
      const char *n = de->d_name, *ext = strrchr(n, '.');
      size_t bl = strlen(base);
      if (n[0] == '.' || !ext || strncmp(n, base, bl) || (n[bl] != '.'))
         continue;
      if (strcasecmp(ext, ".srt") && strcasecmp(ext, ".ass") && strcasecmp(ext, ".ssa") && strcasecmp(ext, ".vtt"))
         continue;
      P.s_stream[P.ns] = -1;
      snprintf(P.s_file[P.ns], sizeof(P.s_file[0]), "%s/%s", dir, n);
      /* the part after the video's name: "en.srt", or just "srt" */
      snprintf(P.s_name[P.ns], sizeof(P.s_name[0]), "File: %s", n + bl + 1);
      P.ns++;
   }
   closedir(d);
}

/* The file's bytes if they are not UTF-8 text: a UTF-8 copy is written to /tmp for FFmpeg to read
 * instead (name in tmp); see subenc.h. Returns the path FFmpeg should open. */
static const char *sub_file_as_utf8(const char *file, char *tmp, size_t tmp_sz)
{
   FILE *f = fopen(file, "rb");
   unsigned char *buf, *conv;
   const char *ext = strrchr(file, '.');
   long size;
   size_t got, n;
   tmp[0] = 0;
   if (!f)
      return file;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (size <= 0 || size > 8 * 1024 * 1024 || !(buf = malloc((size_t)size)))
   {
      fclose(f);
      return file;
   }
   got = fread(buf, 1, (size_t)size, f);
   fclose(f);
   conv = subenc_to_utf8(buf, got, &n);
   free(buf);
   if (conv)
   {
      snprintf(tmp, tmp_sz, "/tmp/g36-sub%s", ext && strlen(ext) < 8 ? ext : ".srt");
      f = fopen(tmp, "wb");
      if (f && fwrite(conv, 1, n, f) == n)
      {
         fclose(f);
         free(conv);
         return tmp;
      }
      if (f)
         fclose(f);
      tmp[0] = 0;
      free(conv);
   }
   return file;
}

/* every subtitle in a file beside the video, read at once */
static int load_sub_file(const char *file)
{
   char tmp[64];
   const char *use = sub_file_as_utf8(file, tmp, sizeof(tmp));
   AVFormatContext *fmt = NULL;
   AVCodecContext *dec = NULL;
   const AVCodec *codec;
   AVPacket *pkt = av_packet_alloc();
   int n = 0;
   if (!pkt || avformat_open_input(&fmt, use, NULL, NULL) < 0)
      goto done;
   if (avformat_find_stream_info(fmt, NULL) < 0 || fmt->nb_streams < 1 ||
         !(codec = avcodec_find_decoder(fmt->streams[0]->codecpar->codec_id)) ||
         !(dec = avcodec_alloc_context3(codec)) ||
         avcodec_parameters_to_context(dec, fmt->streams[0]->codecpar) < 0)
      goto done;
   dec->pkt_timebase = fmt->streams[0]->time_base;
   if (avcodec_open2(dec, codec, NULL) < 0)
      goto done;
   while (av_read_frame(fmt, pkt) >= 0)
   {
      AVSubtitle sub;
      int got = 0;
      AVRational tb = fmt->streams[0]->time_base;
      if (avcodec_decode_subtitle2(dec, &sub, &got, pkt) >= 0 && got)
      {
         double pts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(tb) : 0;
         pthread_mutex_lock(&P.m);
         cue_add(&sub, pts, pkt->duration * av_q2d(tb), 0);
         pthread_mutex_unlock(&P.m);
         avsubtitle_free(&sub);
         n++;
      }
      av_packet_unref(pkt);
   }
done:
   avcodec_free_context(&dec);
   if (fmt)
      avformat_close_input(&fmt);
   av_packet_free(&pkt);
   if (tmp[0])
      unlink(tmp);
   return n;
}

/* ---------------------------------------------------------------- reading */

static void *demux_main(void *arg)
{
   (void)arg;
   pthread_mutex_lock(&P.m);
   while (!P.quit)
   {
      AVPacket *pkt;
      int r, serial;

      if (P.seek_req)
      {
         int64_t ts = (int64_t)(P.seek_to * AV_TIME_BASE);
         double to = P.seek_to;
         P.seek_req = 0;
         pthread_mutex_unlock(&P.m);
         /* the keyframe at or before the spot; frames up to the spot are decoded and skipped */
         r = avformat_seek_file(P.fmt, -1, INT64_MIN, ts, ts, 0);
         if (r < 0)
            r = av_seek_frame(P.fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
         pthread_mutex_lock(&P.m);
         q_clear(&P.vq);
         q_clear(&P.aq);
         P.serial++;
         P.floor = to;
         P.seek_at = now_s();
         P.demux_eof = 0;
         P.video_done = P.vi < 0;
         P.audio_done = P.ai < 0;
         P.clk_valid = 0;
         pthread_cond_broadcast(&P.cond);
         if (r < 0)
            fprintf(stderr, "player: seek to %.1f failed\n", to);
         continue;
      }
      if (P.demux_eof || P.vq.bytes + P.aq.bytes > QUEUE_BYTES ||
            (P.vq.n >= VQ_FULL && (P.ai < 0 || P.aq.n >= 8)) ||
            (P.aq.n >= AQ_FULL && (P.vi < 0 || P.vq.n >= 8)))
      {
         /* nothing to do until something is used up (or a seek) */
         struct timespec ts;
         clock_gettime(CLOCK_REALTIME, &ts);
         ts.tv_nsec += 20 * 1000000;
         if (ts.tv_nsec >= 1000000000)
         {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
         }
         pthread_cond_timedwait(&P.cond, &P.m, &ts);
         continue;
      }
      serial = P.serial;
      pthread_mutex_unlock(&P.m);

      pkt = av_packet_alloc();
      r = pkt ? av_read_frame(P.fmt, pkt) : AVERROR(ENOMEM);

      pthread_mutex_lock(&P.m);
      if (serial != P.serial || P.seek_req)
      {
         av_packet_free(&pkt);   /* read before a seek: not wanted */
         continue;
      }
      if (r < 0)
      {
         /* the end (or an unreadable rest): let the decoders finish what they have */
         av_packet_free(&pkt);
         if (P.vi >= 0)
            q_put(&P.vq, NULL, serial);
         if (P.ai >= 0)
            q_put(&P.aq, NULL, serial);
         P.demux_eof = 1;
         if (r != AVERROR_EOF)
            fprintf(stderr, "player: read stopped (%d)\n", r);
      }
      else if (pkt->stream_index == P.vi)
         q_put(&P.vq, pkt, serial);
      else if (pkt->stream_index == P.ai)
         q_put(&P.aq, pkt, serial);
      else
      {
         if (pkt->stream_index == P.si && P.sdec)
         {
            /* a subtitle: decoded now (it's quick) and kept until its time comes */
            AVSubtitle sub;
            int got = 0;
            AVRational tb = P.fmt->streams[P.si]->time_base;
            if (avcodec_decode_subtitle2(P.sdec, &sub, &got, pkt) >= 0 && got)
            {
               double pts = sub.pts != AV_NOPTS_VALUE ? sub.pts / (double)AV_TIME_BASE
                     : pkt->pts != AV_NOPTS_VALUE ? pkt->pts * av_q2d(tb) : 0;
               cue_add(&sub, pts, pkt->duration * av_q2d(tb), serial);
               avsubtitle_free(&sub);
            }
         }
         av_packet_free(&pkt);
      }
      pthread_cond_broadcast(&P.cond);
   }
   pthread_mutex_unlock(&P.m);
   return NULL;
}

/* ---------------------------------------------------------------- video */

static void video_frame_in(AVFrame *f, int serial)   /* locked */
{
   double pts;
   int64_t ts = f->best_effort_timestamp;

   if (ts == AV_NOPTS_VALUE)
      pts = P.last_pts + 1.0 / 30;
   else
      pts = ts * av_q2d(P.vtb);
   P.last_pts = pts;
   if (serial != P.serial || pts < P.floor - 0.02)
      return;   /* old, or before the spot a seek asked for */
   while (!P.quit && serial == P.serial && P.fq_n >= FQ_MAX)
      pthread_cond_wait(&P.cond, &P.m);
   if (P.quit || serial != P.serial)
      return;
   P.fq[P.fq_n] = av_frame_clone(f);
   if (!P.fq[P.fq_n])
      return;
   P.fq_pts[P.fq_n] = pts;
   P.fq_serial[P.fq_n] = serial;
   P.fq_n++;
   if (P.ready_serial != serial)
   {
      if (now_s() - P.seek_at > 1.0)
         fprintf(stderr, "player: the picture was ready after %.1f s\n", now_s() - P.seek_at);
      P.vdec->skip_frame = AVDISCARD_DEFAULT;
      P.ready_serial = serial;
      pthread_cond_broadcast(&P.cond);
   }
}

static void *video_main(void *arg)
{
   AVFrame *f = av_frame_alloc();
   int cur = -1;
   (void)arg;

   pthread_mutex_lock(&P.m);
   while (f)
   {
      AVPacket *pkt;
      int serial, r, eof;
      if (!q_get(&P.vq, &pkt, &serial))
         break;
      pthread_mutex_unlock(&P.m);
      eof = !pkt;
      if (serial != cur)
      {
         avcodec_flush_buffers(P.vdec);
         P.vdec->skip_frame = AVDISCARD_DEFAULT;
         cur = serial;
      }
      r = avcodec_send_packet(P.vdec, pkt);   /* pkt NULL: the end, send what's left */
      av_packet_free(&pkt);
      if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF)
         fprintf(stderr, "player: video packet refused (%d)\n", r);
      while (avcodec_receive_frame(P.vdec, f) == 0)
      {
         pthread_mutex_lock(&P.m);
         if (f->best_effort_timestamp != AV_NOPTS_VALUE && serial == P.serial && P.ready_serial != serial)
         {
            /* after opening or a seek: decoding from the keyframe up to the spot. Those
             * frames are thrown away, so the ones nothing else needs aren't decoded at
             * all; that gets to the spot (and the sound started) sooner */
            double ahead = P.floor - f->best_effort_timestamp * av_q2d(P.vtb);
            enum AVDiscard want = ahead > 0.3 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
            if (want != P.vdec->skip_frame)
               P.vdec->skip_frame = want;
         }
         else if (f->best_effort_timestamp != AV_NOPTS_VALUE && P.clk_valid && !P.paused && serial == P.serial)
         {
            /* falling behind (a big video, or a fast speed): stop decoding the frames
             * nothing else needs until it catches up. (Not skipping to the next keyframe:
             * that can be seconds ahead, and the picture would stop while the sound caught up.) */
            double lag = clock_now() - f->best_effort_timestamp * av_q2d(P.vtb);
            enum AVDiscard want = lag > 0.1 ? AVDISCARD_NONREF : lag < 0.03 ? AVDISCARD_DEFAULT : P.vdec->skip_frame;
            if (want != P.vdec->skip_frame)
            {
               static int told;
               if (told++ < 4)
                  fprintf(stderr, "player: %s (%.2f s behind)\n",
                        want == AVDISCARD_NONREF ? "skipping some frames" : "caught up", lag);
               P.vdec->skip_frame = want;
            }
         }
         video_frame_in(f, serial);
         pthread_mutex_unlock(&P.m);
         av_frame_unref(f);
      }
      pthread_mutex_lock(&P.m);
      if (eof && serial == P.serial)
      {
         P.video_done = 1;   /* every frame is in the queue */
         pthread_cond_broadcast(&P.cond);
      }
   }
   pthread_mutex_unlock(&P.m);
   av_frame_free(&f);
   return NULL;
}

/* ---------------------------------------------------------------- sound */

static int pcm_open(void)
{
   snd_pcm_hw_params_t *hw;
   if (snd_pcm_open(&P.pcm, BOARD_PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0)
   {
      P.pcm = NULL;
      return -1;
   }
   if (snd_pcm_set_params(P.pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
            2, OUT_RATE, 1, 120000) < 0)
   {
      snd_pcm_close(P.pcm);
      P.pcm = NULL;
      return -1;
   }
   snd_pcm_hw_params_alloca(&hw);
   P.can_pause = snd_pcm_hw_params_current(P.pcm, hw) == 0 && snd_pcm_hw_params_can_pause(hw);
   return 0;
}

/* write samples, waiting out a pause; 0 when a seek or quit made them unwanted */
/* after opening or a seek, the video decodes from the keyframe before the spot, which
 * takes a moment on this chip: the sound waits for it, so both start together */
static void wait_for_picture(int serial)   /* locked */
{
   struct timespec ts;
   if (P.vi < 0 || P.ready_serial == serial || P.video_done)
      return;
   clock_gettime(CLOCK_REALTIME, &ts);
   ts.tv_sec += (time_t)READY_WAIT;
   while (!P.quit && serial == P.serial && P.ready_serial != serial && !P.video_done)
      if (pthread_cond_timedwait(&P.cond, &P.m, &ts) == ETIMEDOUT)
      {
         fprintf(stderr, "player: the picture is slow to start; playing the sound anyway\n");
         break;
      }
}

static int pcm_write(const int16_t *s, int frames, int serial, double end_pts)
{
   while (frames > 0)
   {
      snd_pcm_sframes_t r, delay = 0;
      pthread_mutex_lock(&P.m);
      wait_for_picture(serial);
      if (P.paused && !P.quit && serial == P.serial)
      {
         if (P.can_pause)
            snd_pcm_pause(P.pcm, 1);
         else
            snd_pcm_drop(P.pcm);
         while (P.paused && !P.quit && serial == P.serial)
            pthread_cond_wait(&P.cond, &P.m);
         if (P.can_pause && snd_pcm_pause(P.pcm, 0) < 0)
            snd_pcm_prepare(P.pcm);
         else if (!P.can_pause)
            snd_pcm_prepare(P.pcm);
      }
      if (P.quit || serial != P.serial)
      {
         pthread_mutex_unlock(&P.m);
         return 0;
      }
      pthread_mutex_unlock(&P.m);

      r = snd_pcm_writei(P.pcm, s, frames);
      if (r == -EPIPE || r == -ESTRPIPE || r == -EBADFD)
      {
         snd_pcm_prepare(P.pcm);
         continue;
      }
      if (r < 0)
      {
         if (snd_pcm_recover(P.pcm, (int)r, 1) < 0)
            return 0;
         continue;
      }
      s += r * 2;
      frames -= (int)r;

      /* the clock: what's playing now is the end of this write, minus what's still queued */
      if (snd_pcm_delay(P.pcm, &delay) < 0)
         delay = 0;
      pthread_mutex_lock(&P.m);
      if (serial == P.serial && !P.paused)
         clock_set(end_pts - (double)(frames + delay) / OUT_RATE * P.speed);
      pthread_mutex_unlock(&P.m);
   }
   return 1;
}

/* faster or slower sound at the same pitch: FFmpeg's atempo filter */
static AVFilterGraph *tempo_graph(double sp, AVFilterContext **src, AVFilterContext **sink)
{
   AVFilterGraph *g = avfilter_graph_alloc();
   AVFilterContext *t;
   char args[160];
   static const enum AVSampleFormat fmts[] = { AV_SAMPLE_FMT_S16, AV_SAMPLE_FMT_NONE };
   if (!g)
      return NULL;
   snprintf(args, sizeof(args), "sample_rate=%d:sample_fmt=s16:channel_layout=stereo:time_base=1/%d",
         OUT_RATE, OUT_RATE);
   if (avfilter_graph_create_filter(src, avfilter_get_by_name("abuffer"), "in", args, NULL, g) < 0)
      goto fail;
   snprintf(args, sizeof(args), "%.3f", sp);
   if (avfilter_graph_create_filter(&t, avfilter_get_by_name("atempo"), "tempo", args, NULL, g) < 0)
      goto fail;
   if (avfilter_graph_create_filter(sink, avfilter_get_by_name("abuffersink"), "out", NULL, NULL, g) < 0)
      goto fail;
   av_opt_set_int_list(*sink, "sample_fmts", fmts, AV_SAMPLE_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
   if (avfilter_link(*src, 0, t, 0) < 0 || avfilter_link(t, 0, *sink, 0) < 0 ||
         avfilter_graph_config(g, NULL) < 0)
      goto fail;
   return g;
fail:
   fprintf(stderr, "player: no speed filter; playing at normal speed\n");
   avfilter_graph_free(&g);
   return NULL;
}

static void *audio_main(void *arg)
{
   AVFilterGraph *tg = NULL;
   AVFilterContext *tsrc = NULL, *tsink = NULL;
   AVFrame *tf = av_frame_alloc();
   double t_speed = 1, t_base = 0;
   int64_t t_in = 0, t_out = 0;
   int t_serial = -1, t_told = 0;
   AVFrame *f = av_frame_alloc();
   SwrContext *swr = NULL;
   int64_t swr_layout = 0;
   int swr_rate = 0, swr_fmt = -1, cur = -1;
   int16_t *out = NULL;
   int out_cap = 0;
   (void)arg;

   pthread_mutex_lock(&P.m);
   while (f)
   {
      AVPacket *pkt;
      int serial, r, eof;
      if (!q_get(&P.aq, &pkt, &serial))
         break;
      pthread_mutex_unlock(&P.m);
      eof = !pkt;
      if (serial != cur)
      {
         /* after a seek: forget the old sound, including what ALSA still holds */
         avcodec_flush_buffers(P.adec);
         if (cur >= 0)
         {
            snd_pcm_drop(P.pcm);
            snd_pcm_prepare(P.pcm);
         }
         if (swr)
            swr_init(swr);
         cur = serial;
      }
      r = avcodec_send_packet(P.adec, pkt);
      av_packet_free(&pkt);
      if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF)
         fprintf(stderr, "player: sound packet refused (%d)\n", r);
      while (avcodec_receive_frame(P.adec, f) == 0)
      {
         int64_t layout = f->channel_layout ? (int64_t)f->channel_layout
               : av_get_default_channel_layout(f->channels);
         double pts = f->best_effort_timestamp == AV_NOPTS_VALUE ? 0
               : f->best_effort_timestamp * av_q2d(P.atb);
         int n;
         pthread_mutex_lock(&P.m);
         r = serial != P.serial || pts + (double)f->nb_samples / f->sample_rate < P.floor - 0.02;
         pthread_mutex_unlock(&P.m);
         if (r)
         {
            av_frame_unref(f);
            continue;   /* before the spot a seek asked for */
         }
         if (!swr || layout != swr_layout || f->sample_rate != swr_rate || f->format != swr_fmt)
         {
            swr_free(&swr);
            swr = swr_alloc_set_opts(NULL, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, OUT_RATE,
                  layout, f->format, f->sample_rate, 0, NULL);
            if (!swr || swr_init(swr) < 0)
            {
               fprintf(stderr, "player: can't convert this sound\n");
               swr_free(&swr);
               av_frame_unref(f);
               continue;
            }
            swr_layout = layout;
            swr_rate = f->sample_rate;
            swr_fmt = f->format;
         }
         n = swr_get_out_samples(swr, f->nb_samples);
         if (n > out_cap)
         {
            free(out);
            out = malloc((size_t)n * 4);
            out_cap = out ? n : 0;
         }
         if (out)
         {
            uint8_t *o = (uint8_t *)out;
            double sp;
            n = swr_convert(swr, &o, out_cap, (const uint8_t **)f->extended_data, f->nb_samples);
            pthread_mutex_lock(&P.m);
            sp = P.speed;
            pthread_mutex_unlock(&P.m);
            if (sp != 1 && (!tg || t_speed != sp || t_serial != serial))
            {
               /* a new speed, or after a seek: a fresh filter from here */
               avfilter_graph_free(&tg);
               tg = tempo_graph(sp, &tsrc, &tsink);
               t_speed = sp;
               t_serial = serial;
               t_base = pts;
               t_in = t_out = 0;
               t_told = 0;
            }
            else if (sp == 1 && tg)
               avfilter_graph_free(&tg);
            if (n > 0 && tg && tf)
            {
               /* through the filter; where the sound is up to counts the samples out */
               AVFrame *in = av_frame_alloc();
               if (in)
               {
                  in->format = AV_SAMPLE_FMT_S16;
                  in->channel_layout = AV_CH_LAYOUT_STEREO;
                  in->channels = 2;
                  in->sample_rate = OUT_RATE;
                  in->nb_samples = n;
                  in->pts = t_in;
                  t_in += n;
                  if (av_frame_get_buffer(in, 0) == 0)
                  {
                     memcpy(in->data[0], out, (size_t)n * 4);
                     if (av_buffersrc_add_frame(tsrc, in) < 0)
                        fprintf(stderr, "player: speed filter refused sound\n");
                  }
                  av_frame_free(&in);
               }
               while (av_buffersink_get_frame(tsink, tf) >= 0)
               {
                  t_out += tf->nb_samples;
                  if (t_in >= OUT_RATE * 5 && !t_told && (t_told = 1))
                     fprintf(stderr, "player: speed %.2f: %lld sound samples in, %lld out\n",
                           t_speed, (long long)t_in, (long long)t_out);
                  r = pcm_write((const int16_t *)tf->data[0], tf->nb_samples, serial,
                        t_base + (double)t_out / OUT_RATE * t_speed);
                  av_frame_unref(tf);
                  if (!r)
                     break;
               }
            }
            else if (n > 0)
               pcm_write(out, n, serial, pts + (double)f->nb_samples / f->sample_rate);
         }
         av_frame_unref(f);
      }
      pthread_mutex_lock(&P.m);
      if (eof && serial == P.serial)
      {
         /* everything handed to ALSA; done once it has played out */
         pthread_mutex_unlock(&P.m);
         snd_pcm_drain(P.pcm);
         snd_pcm_prepare(P.pcm);
         pthread_mutex_lock(&P.m);
         if (serial == P.serial)
            P.audio_done = 1;
         pthread_cond_broadcast(&P.cond);
      }
   }
   pthread_mutex_unlock(&P.m);
   avfilter_graph_free(&tg);
   av_frame_free(&tf);
   swr_free(&swr);
   free(out);
   av_frame_free(&f);
   return NULL;
}

/* ---------------------------------------------------------------- opening */

static AVCodecContext *open_decoder(int si, int threads)
{
   AVStream *st = P.fmt->streams[si];
   const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
   AVCodecContext *c;
   if (!codec || !(c = avcodec_alloc_context3(codec)))
      return NULL;
   if (avcodec_parameters_to_context(c, st->codecpar) < 0)
   {
      avcodec_free_context(&c);
      return NULL;
   }
   c->pkt_timebase = st->time_base;
   if (threads)
   {
      c->thread_count = threads;
      c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
      if (c->height > 720)
      {
         /* bigger than 720p is more than this chip can keep up with: trade some
          * picture quality for speed */
         c->skip_loop_filter = AVDISCARD_ALL;
         c->flags2 |= AV_CODEC_FLAG2_FAST;
      }
   }
   if (avcodec_open2(c, codec, NULL) < 0)
   {
      avcodec_free_context(&c);
      return NULL;
   }
   return c;
}

static void tag(const char *key, char *out, size_t sz)
{
   AVDictionaryEntry *e = av_dict_get(P.fmt->metadata, key, NULL, 0);
   if (!e && P.ai >= 0)
      e = av_dict_get(P.fmt->streams[P.ai]->metadata, key, NULL, 0);   /* Ogg, Opus */
   snprintf(out, sz, "%s", e && e->value ? e->value : "");
}

/* the album art: a picture inside the file, or cover.jpg (and the like) beside it */
static image_t *find_cover(const char *path)
{
   static const char *const names[] = { "cover.jpg", "cover.png", "folder.jpg", "folder.png",
      "front.jpg", "front.png", "Cover.jpg", "Folder.jpg", "AlbumArt.jpg" };
   unsigned i;
   char dir[1100], p[1300], *slash;
   for (i = 0; i < P.fmt->nb_streams; i++)
   {
      AVStream *st = P.fmt->streams[i];
      if ((st->disposition & AV_DISPOSITION_ATTACHED_PIC) && st->attached_pic.size > 0)
      {
         image_t *img = image_load_mem(st->attached_pic.data, st->attached_pic.size);
         if (img)
            return img;
      }
   }
   snprintf(dir, sizeof(dir), "%s", path);
   if (!(slash = strrchr(dir, '/')))
      return NULL;
   *slash = 0;
   for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
   {
      snprintf(p, sizeof(p), "%s/%s", dir, names[i]);
      if (access(p, R_OK) == 0)
         return image_load(p);
   }
   return NULL;
}

/* Sound only: the file is opened without its picture (no video is read or decoded, only the sound), like a
 * song; the menu turns the screen off. It stays on from one file to the next until the menu clears it. */
static int sound_only;

static int open_file(const char *path, double start_at, int audio_pick)
{
   int hide_video = 0;
   unsigned i;
   int r;

   player_close();
   snprintf(P.path, sizeof(P.path), "%s", path);
   P.audio_pick = audio_pick;
   if (P.speed <= 0)
      P.speed = 1;
   P.na = P.ns = 0;
   P.sub_cur = P.si = -1;
   memset(&P.info, 0, sizeof(P.info));
   P.err[0] = 0;
   P.vi = P.ai = -1;
   P.serial = 1;
   P.quit = P.seek_req = P.demux_eof = 0;
   P.paused = P.clk_valid = 0;
   P.fq_n = 0;
   P.shown_serial = P.ready_serial = 0;
   P.n_shown = P.n_dropped = 0;
   P.floor = 0;
   P.last_pts = 0;
   P.seek_at = now_s();
   P.seek_to = 0;

   av_log_set_level(AV_LOG_ERROR);
   if ((r = avformat_open_input(&P.fmt, path, NULL, NULL)) < 0)
   {
      snprintf(P.err, sizeof(P.err), "This file can't be opened.");
      fprintf(stderr, "player: open %s failed (%d)\n", path, r);
      return -1;
   }
   if (avformat_find_stream_info(P.fmt, NULL) < 0)
      fprintf(stderr, "player: no stream info for %s\n", path);

   for (i = 0; i < P.fmt->nb_streams; i++)
   {
      AVStream *st = P.fmt->streams[i];
      st->discard = AVDISCARD_ALL;
   }
   /* the picture (not album art) and the sound */
   for (i = 0; i < P.fmt->nb_streams; i++)
   {
      AVStream *st = P.fmt->streams[i];
      if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
      {
         r = av_find_best_stream(P.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
         P.vi = r >= 0 && !(P.fmt->streams[r]->disposition & AV_DISPOSITION_ATTACHED_PIC) ? r : (int)i;
         break;
      }
   }
   r = av_find_best_stream(P.fmt, AVMEDIA_TYPE_AUDIO, -1, P.vi, NULL, 0);
   P.ai = r >= 0 ? r : -1;
   if (sound_only && P.vi >= 0 && P.ai >= 0)
   {
      /* no picture: the video stream stays discarded (not even read), the file still counts as a video */
      P.vi = -1;
      hide_video = 1;
   }
   /* the tracks to choose from */
   for (i = 0; i < P.fmt->nb_streams; i++)
   {
      AVStream *st = P.fmt->streams[i];
      enum AVMediaType ty = st->codecpar->codec_type;
      if (ty == AVMEDIA_TYPE_AUDIO && P.na < MAX_TRACKS)
      {
         P.a_stream[P.na] = (int)i;
         track_name(st, P.na + 1, "Track", P.a_name[P.na], sizeof(P.a_name[0]));
         P.na++;
      }
      else if (ty == AVMEDIA_TYPE_SUBTITLE && P.ns < MAX_TRACKS && avcodec_find_decoder(st->codecpar->codec_id))
      {
         P.s_stream[P.ns] = (int)i;
         P.s_file[P.ns][0] = 0;
         track_name(st, P.ns + 1, "Subtitles", P.s_name[P.ns], sizeof(P.s_name[0]));
         P.ns++;
      }
   }
   if (audio_pick >= 0 && audio_pick < P.na)
      P.ai = P.a_stream[audio_pick];
   find_sub_files(path);

   if (P.vi >= 0 && !(P.vdec = open_decoder(P.vi, 3)))
   {
      fprintf(stderr, "player: no decoder for the video in %s\n", path);
      P.vi = -1;
   }
   if (P.ai >= 0 && !(P.adec = open_decoder(P.ai, 0)))
   {
      fprintf(stderr, "player: no decoder for the sound in %s\n", path);
      P.ai = -1;
   }
   if (P.ai >= 0 && pcm_open() < 0)
   {
      fprintf(stderr, "player: sound output busy; playing without sound\n");
      avcodec_free_context(&P.adec);
      P.ai = -1;
   }
   if (P.vi < 0 && P.ai < 0)
   {
      snprintf(P.err, sizeof(P.err), "There's no video or sound here that this can play.");
      player_close();
      return -1;
   }
   if (P.vi >= 0)
   {
      AVStream *st = P.fmt->streams[P.vi];
      AVRational sar = av_guess_sample_aspect_ratio(P.fmt, st, NULL);
      st->discard = AVDISCARD_DEFAULT;
      P.vtb = st->time_base;
      P.info.has_video = 1;
      P.info.video_w = P.vdec->width;
      P.info.video_h = P.vdec->height;
      if (sar.num > 0 && sar.den > 0)
         P.info.video_w = (int)((int64_t)P.vdec->width * sar.num / sar.den);
      P.info.src_w = P.vdec->width;
      P.info.src_h = P.vdec->height;
      P.sub_w = P.vdec->width;
      P.sub_h = P.vdec->height;
      {
         AVRational fr = av_guess_frame_rate(P.fmt, st, NULL);
         P.info.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0;
      }
      snprintf(P.info.vcodec, sizeof(P.info.vcodec), "%s", avcodec_get_name(P.vdec->codec_id));
      P.vbuf = gfx_video_new(P.vdec->width, P.vdec->height);
      if (!P.vbuf)
      {
         snprintf(P.err, sizeof(P.err), "This video is too big for the handheld.");
         player_close();
         return -1;
      }
   }
   if (P.ai >= 0)
   {
      P.fmt->streams[P.ai]->discard = AVDISCARD_DEFAULT;
      P.atb = P.fmt->streams[P.ai]->time_base;
      P.info.has_audio = 1;
      snprintf(P.info.acodec, sizeof(P.info.acodec), "%s", avcodec_get_name(P.adec->codec_id));
   }
   if (hide_video)
      P.info.has_video = 1;
   P.video_done = P.vi < 0;
   P.audio_done = P.ai < 0;
   P.info.duration = P.fmt->duration > 0 ? P.fmt->duration / (double)AV_TIME_BASE : 0;
   tag("title", P.info.title, sizeof(P.info.title));
   tag("artist", P.info.artist, sizeof(P.info.artist));
   if (!P.info.artist[0])
      tag("album_artist", P.info.artist, sizeof(P.info.artist));
   tag("album", P.info.album, sizeof(P.info.album));
   if (!P.info.has_video)
      P.info.cover = find_cover(path);

   if (start_at > 1)
   {
      P.seek_req = 1;
      P.seek_to = start_at;
   }
   P.open = 1;
   if (pthread_create(&P.demux_th, NULL, demux_main, NULL) == 0)
      P.threads |= 1;
   if (P.vi >= 0 && pthread_create(&P.video_th, NULL, video_main, NULL) == 0)
      P.threads |= 2;
   if (P.ai >= 0 && pthread_create(&P.audio_th, NULL, audio_main, NULL) == 0)
      P.threads |= 4;
   fprintf(stderr, "player: %s: %s%s%s, %.0f s\n", path,
         P.vi >= 0 ? avcodec_get_name(P.vdec->codec_id) : "",
         P.vi >= 0 && P.ai >= 0 ? " + " : "", P.ai >= 0 ? avcodec_get_name(P.adec->codec_id) : "",
         P.info.duration);
   if (P.vi >= 0)
      fprintf(stderr, "player: video %dx%d (shown %dx%d)\n", P.vdec->width, P.vdec->height,
            P.info.video_w, P.info.video_h);
   return 0;
}

int player_open(const char *path, double start_at)
{
   return open_file(path, start_at, -1);
}

int player_sound_only(void)
{
   return sound_only;
}

/* Sound only on or off: the file is reopened where it was (like a change of sound track), without the
 * picture or with it again. A file with no sound can't be played that way. */
void player_set_sound_only(int on)
{
   char path[1400];
   double pos;
   int paused, sub, track;
   on = on ? 1 : 0;
   if (on == sound_only)
      return;
   if (on && P.open && P.ai < 0)
      return;
   sound_only = on;
   if (!P.open)
      return;
   snprintf(path, sizeof(path), "%s", P.path);
   pos = player_position();
   paused = P.paused;
   sub = P.sub_cur;
   track = player_audio_current();
   if (open_file(path, pos > 1 ? pos : 1.01, track) < 0)
      open_file(path, pos > 1 ? pos : 1.01, -1);
   if (sub >= 0)
      player_set_sub(sub);
   if (paused)
      player_set_paused(1);
}

void player_close(void)
{
   int i;
   pthread_mutex_lock(&P.m);
   P.quit = 1;
   pthread_cond_broadcast(&P.cond);
   pthread_mutex_unlock(&P.m);
   if (P.pcm)
      snd_pcm_drop(P.pcm);   /* wakes a blocked write */
   if (P.threads & 1)
      pthread_join(P.demux_th, NULL);
   if (P.threads & 2)
      pthread_join(P.video_th, NULL);
   if (P.threads & 4)
      pthread_join(P.audio_th, NULL);
   P.threads = 0;
   q_clear(&P.vq);
   q_clear(&P.aq);
   for (i = 0; i < P.fq_n; i++)
      av_frame_free(&P.fq[i]);
   P.fq_n = 0;
   avcodec_free_context(&P.vdec);
   avcodec_free_context(&P.adec);
   if (P.fmt)
      avformat_close_input(&P.fmt);
   if (P.pcm)
   {
      snd_pcm_close(P.pcm);
      P.pcm = NULL;
   }
   gfx_video_free(P.vbuf);
   P.vbuf = NULL;
   sws_freeContext(P.sws);
   P.sws = NULL;
   image_free(P.info.cover);
   P.info.cover = NULL;
   avcodec_free_context(&P.sdec);
   P.si = P.sub_cur = -1;
   for (i = 0; i < P.ncue; i++)
      cue_free(&P.cue[i]);
   P.ncue = 0;
   P.open = 0;
}

int player_is_open(void)
{
   return P.open;
}

const char *player_error(void)
{
   return P.err;
}

const player_info_t *player_info(void)
{
   return &P.info;
}

/* ---------------------------------------------------------------- controls */

double player_position(void)
{
   double v;
   pthread_mutex_lock(&P.m);
   v = P.seek_req || !P.clk_valid ? P.seek_to : clock_now();
   pthread_mutex_unlock(&P.m);
   if (v < 0)
      v = 0;
   if (P.info.duration > 0 && v > P.info.duration)
      v = P.info.duration;
   return v;
}

int player_paused(void)
{
   return P.paused;
}

void player_set_paused(int paused)
{
   pthread_mutex_lock(&P.m);
   if (paused != P.paused)
   {
      if (P.clk_valid)
         clock_set(clock_now());   /* freeze, or carry on from, where it is */
      P.paused = paused;
      pthread_cond_broadcast(&P.cond);
   }
   pthread_mutex_unlock(&P.m);
}

void player_seek(double s)
{
   if (s < 0)
      s = 0;
   if (P.info.duration > 0 && s > P.info.duration - 1)
      s = P.info.duration - 1 > 0 ? P.info.duration - 1 : 0;
   pthread_mutex_lock(&P.m);
   P.seek_to = s;
   P.seek_req = 1;
   P.clk_valid = 0;
   pthread_cond_broadcast(&P.cond);
   pthread_mutex_unlock(&P.m);
}

void player_seek_by(double d)
{
   double base;
   pthread_mutex_lock(&P.m);
   /* while a seek is still on its way, the next press adds to it */
   base = P.seek_req || !P.clk_valid ? P.seek_to : clock_now();
   pthread_mutex_unlock(&P.m);
   player_seek(base + d);
}

int player_waiting(void)
{
   int w;
   pthread_mutex_lock(&P.m);
   w = P.open && P.vi >= 0 && !P.video_done && (P.seek_req || P.ready_serial != P.serial);
   pthread_mutex_unlock(&P.m);
   return w;
}

int player_ended(void)
{
   int e;
   pthread_mutex_lock(&P.m);
   e = P.open && !P.seek_req && P.demux_eof && P.audio_done && P.video_done &&
         !P.vq.head && !P.aq.head && !P.fq_n;
   pthread_mutex_unlock(&P.m);
   return e;
}

double player_speed(void)
{
   return P.speed > 0 ? P.speed : 1;
}

void player_set_speed(double sp)
{
   if (sp < 0.5)
      sp = 0.5;
   if (sp > 2)
      sp = 2;
   pthread_mutex_lock(&P.m);
   if (P.clk_valid)
      clock_set(clock_now());   /* the time so far at the old speed */
   P.speed = sp;
   pthread_cond_broadcast(&P.cond);
   pthread_mutex_unlock(&P.m);
}

int player_audio_count(void)
{
   return P.open ? P.na : 0;
}

const char *player_audio_name(int i)
{
   return i >= 0 && i < P.na ? P.a_name[i] : "";
}

int player_audio_current(void)
{
   int i;
   for (i = 0; i < P.na; i++)
      if (P.a_stream[i] == P.ai)
         return i;
   return -1;
}

void player_set_audio(int i)
{
   char path[1400];
   double pos;
   int paused, sub;
   if (!P.open || i < 0 || i >= P.na || P.a_stream[i] == P.ai)
      return;
   /* reopen where it was, with the other track */
   snprintf(path, sizeof(path), "%s", P.path);
   pos = player_position();
   paused = P.paused;
   sub = P.sub_cur;
   if (open_file(path, pos > 1 ? pos : 1.01, i) < 0)
   {
      open_file(path, pos > 1 ? pos : 1.01, -1);
      return;
   }
   if (sub >= 0)
      player_set_sub(sub);
   if (paused)
      player_set_paused(1);
}

int player_sub_count(void)
{
   return P.open ? P.ns : 0;
}

const char *player_sub_name(int i)
{
   return i >= 0 && i < P.ns ? P.s_name[i] : "";
}

int player_sub_current(void)
{
   return P.sub_cur;
}

void player_set_sub(int i)
{
   int k, embedded;
   if (!P.open || i >= P.ns || i == P.sub_cur)
      return;
   pthread_mutex_lock(&P.m);
   if (P.si >= 0)
      P.fmt->streams[P.si]->discard = AVDISCARD_ALL;
   avcodec_free_context(&P.sdec);
   P.si = -1;
   for (k = 0; k < P.ncue; k++)
      cue_free(&P.cue[k]);
   P.ncue = 0;
   P.cue_seq++;
   P.sub_cur = i < 0 ? -1 : i;
   embedded = i >= 0 && P.s_stream[i] >= 0;
   if (embedded)
   {
      AVStream *st = P.fmt->streams[P.s_stream[i]];
      const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
      AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : NULL;
      if (c && avcodec_parameters_to_context(c, st->codecpar) >= 0)
      {
         c->pkt_timebase = st->time_base;
         if (avcodec_open2(c, codec, NULL) >= 0)
         {
            P.sdec = c;
            P.si = P.s_stream[i];
            st->discard = AVDISCARD_DEFAULT;
            /* pictures are placed on this frame (a DVD's is 720 x 480) */
            P.sub_w = c->width > 0 ? c->width : P.info.src_w;
            P.sub_h = c->height > 0 ? c->height : P.info.src_h;
         }
         else
            avcodec_free_context(&c);
      }
      else
         avcodec_free_context(&c);
   }
   pthread_mutex_unlock(&P.m);
   if (embedded && P.sdec)
      player_seek(player_position());   /* read again from here, so the current line shows */
   else if (i >= 0 && !embedded)
   {
      P.sub_w = P.info.src_w;
      P.sub_h = P.info.src_h;
      if (load_sub_file(P.s_file[i]) == 0)
         fprintf(stderr, "player: nothing readable in %s\n", P.s_file[i]);
   }
}

int player_subtitle(player_sub_t *out)
{
   double t;
   int k, n = 0, w = 0;
   unsigned id = 0;
   memset(out, 0, sizeof(*out));
   if (!P.open || P.sub_cur < 0)
      return 0;
   t = player_position();
   pthread_mutex_lock(&P.m);
   /* first, let go of what's done with: old lines, and ones from before a seek */
   for (k = 0; k < P.ncue; k++)
   {
      struct cue *c = &P.cue[k];
      if ((c->serial && c->serial != P.serial) || (c->serial && c->end < t - 1))
      {
         cue_free(c);
         P.cue[k--] = P.cue[--P.ncue];
      }
   }
   for (k = 0; k < P.ncue; k++)
   {
      struct cue *c = &P.cue[k];
      if (c->start > t || c->end <= t)
         continue;
      id = id * 31 + (unsigned)(uintptr_t)c + (unsigned)(c->start * 1000);
      if (c->img)
      {
         out->img = c->img;
         out->x = c->x;
         out->y = c->y;
         out->w = c->w;
         out->h = c->h;
      }
      else if (c->text && w < (int)sizeof(out->text) - 2)
         w += snprintf(out->text + w, sizeof(out->text) - w, "%s%s", w ? "\n" : "", c->text);
      n++;
   }
   pthread_mutex_unlock(&P.m);
   out->canvas_w = P.sub_w;
   out->canvas_h = P.sub_h;
   out->id = id ^ P.cue_seq;
   return n > 0;
}

/* ---------------------------------------------------------------- video frames */

static void copy_frame(AVFrame *f)
{
   uint8_t *pl[3];
   int st[3];
   gfx_video_planes(P.vbuf, pl, st);
   if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P)
   {
      av_image_copy_plane(pl[0], st[0], f->data[0], f->linesize[0], f->width, f->height);
      av_image_copy_plane(pl[1], st[1], f->data[1], f->linesize[1], (f->width + 1) / 2, (f->height + 1) / 2);
      av_image_copy_plane(pl[2], st[2], f->data[2], f->linesize[2], (f->width + 1) / 2, (f->height + 1) / 2);
      return;
   }
   /* anything else (10-bit, 4:2:2, ...) is converted first */
   P.sws = sws_getCachedContext(P.sws, f->width, f->height, f->format, f->width, f->height,
         AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, NULL, NULL, NULL);
   if (P.sws)
      sws_scale(P.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, pl, st);
}

int player_video_frame(void)
{
   AVFrame *take = NULL;
   int i;
   double now = now_s();

   if (!P.open || P.vi < 0)
      return 0;
   pthread_mutex_lock(&P.m);
   while (P.fq_n)
   {
      double pts = P.fq_pts[0];
      int drop = 0;
      if (P.fq_serial[0] != P.serial)
         drop = 1;
      else if (P.shown_serial != P.serial)
      {
         /* the first frame after opening or a seek shows at once (even paused) */
         take = P.fq[0];
         P.shown_serial = P.serial;
         P.shown_at = now;
         if (P.ai < 0)
            clock_set(pts);
      }
      else if (!P.clk_valid)
      {
         /* the sound hasn't started: wait, but not forever (a file whose sound won't play) */
         if (now - P.shown_at > 1.0)
            clock_set(pts);
         break;
      }
      else if (pts <= clock_now() + VIDEO_LEAD)
      {
         /* due; but skip it if the next one is due too (decoding fell behind) */
         if (P.fq_n > 1 && P.fq_serial[1] == P.serial && P.fq_pts[1] <= clock_now() + VIDEO_LEAD)
            drop = 1;
         else
         {
            double late = clock_now() - pts;
            take = P.fq[0];
            P.lag_sum += late;
            if (late > P.lag_max)
               P.lag_max = late;
         }
      }
      else
         break;

      if (drop || take)
      {
         if (drop)
         {
            av_frame_free(&P.fq[0]);
            P.n_dropped++;
         }
         for (i = 1; i < P.fq_n; i++)
         {
            P.fq[i - 1] = P.fq[i];
            P.fq_pts[i - 1] = P.fq_pts[i];
            P.fq_serial[i - 1] = P.fq_serial[i];
         }
         P.fq_n--;
         pthread_cond_broadcast(&P.cond);
      }
      if (take)
         break;
   }
   pthread_mutex_unlock(&P.m);

   if (!take)
      return 0;
   copy_frame(take);
   av_frame_free(&take);
   P.n_shown++;
   return 1;
}

gfx_video_t *player_video(void)
{
   return P.vbuf;
}

int player_wait_ms(int max_ms)
{
   int ms = max_ms;
   pthread_mutex_lock(&P.m);
   if (P.fq_n && P.clk_valid && !P.paused)
   {
      double d = (P.fq_pts[0] - VIDEO_LEAD - clock_now()) * 1000;
      ms = d < 0 ? 0 : d < max_ms ? (int)d : max_ms;
   }
   pthread_mutex_unlock(&P.m);
   return ms;
}

void player_stats(int *shown, int *dropped)
{
   *shown = P.n_shown;
   *dropped = P.n_dropped;
   if (P.n_shown)
      fprintf(stderr, "player: frames %.0f ms late on average, %.0f ms at most\n",
            P.lag_sum / P.n_shown * 1000, P.lag_max * 1000);
   P.n_shown = P.n_dropped = 0;
   P.lag_sum = P.lag_max = 0;
}

/* ---------------------------------------------------------------- pictures for the list */

/* A frame about a quarter of the way into a video, fitted into max_w x max_h and saved
 * as a JPEG at out. Only keyframes are decoded, so it's quick even at 1080p. Uses its
 * own FFmpeg contexts: it runs on the menu's picture thread, beside the player. */
int video_thumbnail(const char *path, const char *out, int max_w, int max_h)
{
   AVFormatContext *fmt = NULL;
   AVCodecContext *dec = NULL, *enc = NULL;
   const AVCodec *codec, *jpeg;
   struct SwsContext *sws = NULL;
   AVFrame *f = av_frame_alloc(), *pic = av_frame_alloc();
   AVPacket *pkt = av_packet_alloc();
   int vi = -1, r = -1, got = 0, tries = 0, w, h;
   unsigned i;
   FILE *fo;

   if (!f || !pic || !pkt || avformat_open_input(&fmt, path, NULL, NULL) < 0)
      goto done;
   if (avformat_find_stream_info(fmt, NULL) < 0)
      goto done;
   for (i = 0; i < fmt->nb_streams; i++)
   {
      AVStream *st = fmt->streams[i];
      st->discard = AVDISCARD_ALL;
      if (vi < 0 && st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC))
         vi = (int)i;
   }
   if (vi < 0 || !(codec = avcodec_find_decoder(fmt->streams[vi]->codecpar->codec_id)) ||
         !(dec = avcodec_alloc_context3(codec)) ||
         avcodec_parameters_to_context(dec, fmt->streams[vi]->codecpar) < 0)
      goto done;
   dec->thread_count = 1;
   dec->skip_frame = AVDISCARD_NONKEY;
   dec->skip_loop_filter = AVDISCARD_ALL;
   if (avcodec_open2(dec, codec, NULL) < 0)
      goto done;
   fmt->streams[vi]->discard = AVDISCARD_NONKEY;
   if (fmt->duration > 0)
   {
      int64_t ts = fmt->duration / 4;
      if (avformat_seek_file(fmt, -1, INT64_MIN, ts, ts, 0) < 0)
         av_seek_frame(fmt, -1, ts, AVSEEK_FLAG_BACKWARD);
   }
   while (!got && tries++ < 400 && av_read_frame(fmt, pkt) >= 0)
   {
      if (pkt->stream_index == vi && avcodec_send_packet(dec, pkt) >= 0)
         got = avcodec_receive_frame(dec, f) == 0;
      av_packet_unref(pkt);
   }
   if (!got)
   {
      avcodec_send_packet(dec, NULL);
      got = avcodec_receive_frame(dec, f) == 0;
   }
   if (!got)
      goto done;

   /* the right shape (pixel aspect), fitted; even sizes for the JPEG */
   {
      AVRational sar = av_guess_sample_aspect_ratio(fmt, fmt->streams[vi], f);
      double dw = f->width, dh = f->height, s;
      if (sar.num > 0 && sar.den > 0)
         dw = dw * sar.num / sar.den;
      s = max_w / dw < max_h / dh ? max_w / dw : max_h / dh;
      w = (int)(dw * s) & ~1;
      h = (int)(dh * s) & ~1;
   }
   if (!(jpeg = avcodec_find_encoder(AV_CODEC_ID_MJPEG)) || !(enc = avcodec_alloc_context3(jpeg)))
      goto done;
   enc->width = w;
   enc->height = h;
   enc->pix_fmt = AV_PIX_FMT_YUVJ420P;
   enc->time_base = (AVRational){ 1, 25 };
   enc->flags |= AV_CODEC_FLAG_QSCALE;
   enc->global_quality = FF_QP2LAMBDA * 3;
   if (avcodec_open2(enc, jpeg, NULL) < 0)
      goto done;
   pic->format = AV_PIX_FMT_YUVJ420P;
   pic->width = w;
   pic->height = h;
   pic->quality = enc->global_quality;
   if (av_frame_get_buffer(pic, 0) < 0)
      goto done;
   sws = sws_getContext(f->width, f->height, f->format, w, h, AV_PIX_FMT_YUVJ420P, SWS_BILINEAR, NULL, NULL, NULL);
   if (!sws)
      goto done;
   sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, pic->data, pic->linesize);
   if (avcodec_send_frame(enc, pic) < 0 || avcodec_receive_packet(enc, pkt) < 0)
      goto done;
   {
      char tmp[1100];
      snprintf(tmp, sizeof(tmp), "%s.tmp", out);
      if ((fo = fopen(tmp, "wb")))
      {
         int ok = fwrite(pkt->data, 1, pkt->size, fo) == (size_t)pkt->size;
         ok &= fclose(fo) == 0;
         r = ok && rename(tmp, out) == 0 ? 0 : -1;
         if (r)
            unlink(tmp);
      }
   }
   av_packet_unref(pkt);
done:
   sws_freeContext(sws);
   avcodec_free_context(&enc);
   avcodec_free_context(&dec);
   if (fmt)
      avformat_close_input(&fmt);
   av_packet_free(&pkt);
   av_frame_free(&f);
   av_frame_free(&pic);
   return r;
}
