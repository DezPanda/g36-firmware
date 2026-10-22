/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* Display/input check for the G36 menu core: a button checklist that fills in as
 * buttons are pressed, labelled corners and colour bars, and the frame rate.
 * Exits when Start + B are held together, or after 90 s. */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <linux/input.h>

#include "gfx.h"
#include "input.h"

#define FONT "/oem/release/assets/default/raw/fonts/OpenSans_Semibold.ttf"

static volatile int quit;
static void on_signal(int s) { (void)s; quit = 1; }

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static struct { const char *name; int code; int seen; } btn[] = {
   { "A", BTN_SOUTH }, { "B", BTN_EAST }, { "X", BTN_NORTH }, { "Y", BTN_WEST },
   { "L", BTN_TL }, { "R", BTN_TR }, { "Select", BTN_SELECT }, { "Start", BTN_START },
   { "Menu", KEY_MENU }, { "Up", KEY_UP }, { "Down", KEY_DOWN }, { "Left", KEY_LEFT },
   { "Right", KEY_RIGHT }, { "Vol +", KEY_VOLUMEUP }, { "Vol -", KEY_VOLUMEDOWN }, { "Power", KEY_POWER },
};
#define NBTN (int)(sizeof(btn) / sizeof(btn[0]))

int main(int argc, char **argv)
{
   canvas_t *c;
   font_t *big, *small;
   image_t *img = argc > 1 ? image_load(argv[1]) : NULL;
   char other[64] = "";
   double start, last_fps;
   int frames = 0, fps = 0, i;

   setvbuf(stderr, NULL, _IONBF, 0);
   signal(SIGTERM, on_signal);
   signal(SIGINT, on_signal);

   if (gfx_init() < 0)
   {
      fprintf(stderr, "gfxtest: display init failed\n");
      return 1;
   }
   input_open();
   c = gfx_canvas();
   big   = font_load(FONT, 34);
   small = font_load(FONT, 20);

   start = last_fps = now_s();
   while (!quit && now_s() - start < 90)
   {
      key_event_t ev;
      char buf[96];
      int x0 = 230, y0 = 92;

      while (input_next(&ev, 0))
         if (ev.value == 1)
         {
            int known = 0;
            fprintf(stderr, "key %d\n", ev.code);
            for (i = 0; i < NBTN; i++)
               if (btn[i].code == ev.code)
                  btn[i].seen = known = 1;
            if (!known)
               snprintf(other, sizeof(other), "other code: %d", ev.code);
         }
      if (input_held(BTN_START) && input_held(BTN_EAST))
         break;

      gfx_clear(c, 0xff1c1830);

      /* corner labels */
      gfx_draw_text(c, small, "TOP-LEFT", 6, 4, 0xffffff00, 0);
      gfx_draw_text(c, small, "TOP-RIGHT", c->w - 6 - font_text_width(small, "TOP-RIGHT"), 4, 0xffffff00, 0);
      gfx_draw_text(c, small, "BOTTOM-LEFT", 6, c->h - 62 - font_height(small), 0xffffff00, 0);
      gfx_draw_text(c, small, "BOTTOM-RIGHT", c->w - 6 - font_text_width(small, "BOTTOM-RIGHT"),
            c->h - 62 - font_height(small), 0xffffff00, 0);

      /* labelled colour bars */
      {
         static const struct { const char *n; uint32_t col, txt; } bar[4] = {
            { "RED", 0xffff0000, 0xffffffff }, { "GREEN", 0xff00ff00, 0xff000000 },
            { "BLUE", 0xff0000ff, 0xffffffff }, { "WHITE", 0xffffffff, 0xff000000 } };
         for (i = 0; i < 4; i++)
         {
            int bx = i * c->w / 4;
            gfx_fill_rect(c, bx, c->h - 50, c->w / 4, 50, bar[i].col);
            gfx_draw_text(c, small, bar[i].n, bx + 12, c->h - 40, bar[i].txt, 0);
         }
      }

      gfx_draw_text(c, big, "G36 button check", x0, 28, 0xffffffff, 0);
      snprintf(buf, sizeof(buf), "%d fps  -  %.0f s left  -  hold Start + B to exit", fps, 90 - (now_s() - start));
      gfx_draw_text(c, small, buf, x0, 70, 0xffb0a8d0, 0);
      gfx_fill_rect(c, (int)((now_s() - start) * 200) % c->w, 102, 40, 5, 0xffffc000);

      /* button checklist, 4 columns */
      for (i = 0; i < NBTN; i++)
      {
         int bx = 40 + (i % 4) * 150, by = y0 + 30 + (i / 4) * 52;
         gfx_fill_rect(c, bx, by, 136, 42, btn[i].seen ? 0xff2e9e4f : 0xff3a3550);
         gfx_draw_text(c, small, btn[i].name, bx + 12, by + 8, 0xffffffff, 0);
      }
      gfx_draw_text(c, small, other, 40, y0 + 30 + 4 * 52 + 4, 0xffff8080, 0);

      if (img)
         gfx_draw_image_scaled(c, img, c->w - 200, 125, 170, 170 * img->h / img->w);

      gfx_present();
      frames++;
      if (now_s() - last_fps >= 1)
      {
         fps = frames;
         frames = 0;
         last_fps = now_s();
      }
   }

   for (i = 0; i < NBTN; i++)
      fprintf(stderr, "%-6s %s\n", btn[i].name, btn[i].seen ? "seen" : "NOT seen");
   font_free(big);
   font_free(small);
   image_free(img);
   input_close();
   gfx_shutdown();
   return 0;
}
