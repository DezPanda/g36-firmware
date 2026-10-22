/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/*
 * g36-key <button>... - press buttons on the G36 keypad from a script (testing over ADB).
 * Buttons: a b x y l r start select up down left right, or a Linux key code.
 * "wait" pauses 0.4 s; "select+a" holds the first while pressing the second;
 * "hold:start" / "release:start" press and let go separately.
 */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int open_device(const char *want)
{
   char path[64], name[128];
   int i;
   for (i = 0; i < 16; i++)
   {
      int fd;
      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      if ((fd = open(path, O_RDWR)) < 0)
         continue;
      memset(name, 0, sizeof(name));
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && !strcmp(name, want))
         return fd;
      close(fd);
   }
   return -1;
}

static void emit(int fd, int type, int code, int value)
{
   struct input_event ev;
   memset(&ev, 0, sizeof(ev));
   ev.type = type;
   ev.code = code;
   ev.value = value;
   if (write(fd, &ev, sizeof(ev)) < 0)
      perror("g36-key");
}

int main(int argc, char **argv)
{
   static const struct { const char *name; int code; } keys[] = {
      { "a", BTN_SOUTH }, { "b", BTN_EAST }, { "x", BTN_NORTH }, { "y", BTN_WEST },
      { "l", BTN_TL }, { "r", BTN_TR }, { "start", BTN_START }, { "select", BTN_SELECT },
      { "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT }, { "right", KEY_RIGHT },
      { "volup", KEY_VOLUMEUP }, { "voldown", KEY_VOLUMEDOWN }, { "menu", KEY_MENU },
   };
   /* the volume keys are on their own device; the kernel only takes a key from a
    * device that has it */
   int fd = open_device("Chbp"), vfd = open_device("adc-keys"), i;
   size_t k;

   if (fd < 0)
   {
      fprintf(stderr, "g36-key: keypad not found\n");
      return 1;
   }
   for (i = 1; i < argc; i++)
   {
      char arg[64], *plus;
      int code = 0, hold = 0;
      if (!strcmp(argv[i], "wait"))
      {
         usleep(400000);
         continue;
      }
      snprintf(arg, sizeof(arg), "%s", argv[i]);
      if (!strncmp(arg, "hold:", 5) || !strncmp(arg, "release:", 8))
      {
         int down = arg[0] == 'h';
         const char *b = strchr(arg, ':') + 1;
         for (k = 0; k < sizeof(keys) / sizeof(*keys); k++)
            if (!strcmp(b, keys[k].name))
            {
               int c = keys[k].code;
               int d = (c == KEY_VOLUMEUP || c == KEY_VOLUMEDOWN) && vfd >= 0 ? vfd : fd;
               emit(d, EV_KEY, c, down);
               emit(d, EV_SYN, SYN_REPORT, 0);
            }
         usleep(100000);
         continue;
      }
      if ((plus = strchr(arg, '+')))
      {
         *plus++ = 0;
         for (k = 0; k < sizeof(keys) / sizeof(*keys); k++)
            if (!strcmp(arg, keys[k].name))
               hold = keys[k].code;
         memmove(arg, plus, strlen(plus) + 1);
      }
      code = atoi(arg);
      for (k = 0; k < sizeof(keys) / sizeof(*keys); k++)
         if (!strcmp(arg, keys[k].name))
            code = keys[k].code;
      if (!code)
      {
         fprintf(stderr, "g36-key: unknown button %s\n", argv[i]);
         continue;
      }
      if (hold)
      {
         emit(fd, EV_KEY, hold, 1);
         emit(fd, EV_SYN, SYN_REPORT, 0);
         usleep(100000);
      }
      {
         int d = (code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN) && vfd >= 0 ? vfd : fd;
         emit(d, EV_KEY, code, 1);
         emit(d, EV_SYN, SYN_REPORT, 0);
         usleep(60000);
         emit(d, EV_KEY, code, 0);
         emit(d, EV_SYN, SYN_REPORT, 0);
      }
      if (hold)
      {
         usleep(60000);
         emit(fd, EV_KEY, hold, 0);
         emit(fd, EV_SYN, SYN_REPORT, 0);
      }
      usleep(150000);
   }
   close(fd);
   return 0;
}
