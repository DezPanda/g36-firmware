/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* Button input for the G36 menu. See input.h. */
#include "input.h"

#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* keypad (all face/shoulder/d-pad buttons), power button, volume keys */
static const char *names[] = { "Chbp", "gpio-keys", "adc-keys" };
#define NDEV (sizeof(names) / sizeof(names[0]))

static int fds[NDEV] = { -1, -1, -1 };
static unsigned char held[KEY_MAX + 1];
static short xlate[KEY_MAX + 1];   /* real button -> the one the menu is told about */
static int xlate_on, raw_mode;

static const int fn_codes[9] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, KEY_MENU };

void input_set_map(const int *real)
{
   int i, k, seen = 0;
   xlate_on = 0;
   if (!real)
      return;
   for (i = 0; i <= KEY_MAX; i++)
      xlate[i] = (short)i;
   for (k = 0; k < 9; k++)
   {
      int j, ok = 0;
      for (j = 0; j < 9; j++)
         if (real[k] == fn_codes[j] && !(seen >> j & 1))
         {
            seen |= 1 << j;
            ok = 1;
         }
      if (!ok)
         return;                                      /* not a good map: leave the buttons as they are */
      xlate[real[k]] = (short)fn_codes[k];
      if (real[k] != fn_codes[k])
         xlate_on = 1;
   }
}

void input_set_raw(int raw)
{
   raw_mode = raw;
}

static int open_by_name(const char *want)
{
   char path[64], name[128];
   int i;
   for (i = 0; i < 16; i++)
   {
      int fd;
      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      if ((fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC)) < 0)
         continue;
      memset(name, 0, sizeof(name));
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && !strcmp(name, want))
         return fd;
      close(fd);
   }
   return -1;
}

int input_open(void)
{
   size_t i;
   int ok = 0;
   for (i = 0; i < NDEV; i++)
      if ((fds[i] = open_by_name(names[i])) >= 0)
         ok++;
   return ok ? 0 : -1;
}

int input_next(key_event_t *ev, int timeout_ms)
{
   struct pollfd p[NDEV];
   size_t i, n = 0;

   for (;;)
   {
      /* drain anything already queued first */
      for (i = 0; i < NDEV; i++)
      {
         struct input_event e;
         if (fds[i] < 0)
            continue;
         while (read(fds[i], &e, sizeof(e)) == sizeof(e))
         {
            if (e.type != EV_KEY || e.code > KEY_MAX)
               continue;
            held[e.code] = e.value != 0;
            ev->code  = xlate_on && !raw_mode ? xlate[e.code] : e.code;
            ev->value = e.value;
            return 1;
         }
      }
      if (timeout_ms == 0)
         return 0;

      n = 0;
      for (i = 0; i < NDEV; i++)
         if (fds[i] >= 0)
         {
            p[n].fd = fds[i];
            p[n].events = POLLIN;
            n++;
         }
      if (poll(p, n, timeout_ms) <= 0)
         return 0;
      timeout_ms = 0;
   }
}

int input_held(int code)
{
   return code >= 0 && code <= KEY_MAX && held[code];
}

void input_close(void)
{
   size_t i;
   for (i = 0; i < NDEV; i++)
      if (fds[i] >= 0)
      {
         close(fds[i]);
         fds[i] = -1;
      }
}
