/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/*
 * g36d - small background helper for the G36 handheld.
 *
 *  1. Holding the power button for 3 s while a game (RetroArch) is running
 *     requests the same safe shutdown the game menu uses (/tmp/halt).
 *     In the menu itself, gamebox already shows its own power-off prompt.
 *  2. (The menu keeps its screen and power timers off on a charger or computer itself.)
 *  3. Low battery: off the charger, at battery_off_percent (g36/settings.cfg,
 *     default 10) for 30 s, requests the safe shutdown, in a game too (RetroArch
 *     closes normally and writes its saves). Every change in the level is logged
 *     to g36/logs/battery.log, to see where the battery really runs out.
 *  4. Clock: there's no clock chip, so the time is saved to /etc/g36-clock every
 *     10 minutes and when g36d is stopped (shutdown); S04g36clock restores it.
 *  5. Volume: a press of a volume button steps the volume once, holding it keeps
 *     stepping, in the menu and in games, and the new level is saved to gamebox.json
 *     when the button is let go. (The factory helper vol_adj used to do the first step;
 *     it isn't on the handheld any more.)
 */
#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "board.h"

#define POWER_HOLD_MS   3000
#define EXTCON_STATE    BOARD_EXTCON_STATE
#define HALT_FLAG       "/tmp/halt"
#define BATTERY_DIR     BOARD_BATTERY_DIR
#define SETTINGS        BOARD_GAMES_MOUNT "/g36/settings.cfg"
#define BATTERY_LOG     BOARD_GAMES_MOUNT "/g36/logs/battery.log"
#define BATTERY_MS      10000
#define BATTERY_LOW_N   3          /* readings in a row at or below the limit */
#define CLOCK_FILE      "/etc/g36-clock"
#define CLOCK_MS        600000
#define VOL_DELAY_MS    400        /* hold this long before repeating */
#define VOL_REPEAT_MS   120
#define GAMEBOX_JSON    "/oem/release/config/gamebox.json"
#define SLEEPING_FLAG   "/tmp/sleeping"          /* g36-sleep is putting the handheld to sleep */
#define HOLD_HINT_MS    1000                     /* held this long in a game: say what holding does */
#define SLEEP_DOUBLE_MS 2000                     /* in a game: tap Power twice within this to sleep */

static long long now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Open the /dev/input/eventN whose device name matches. */
static int open_input(const char *want, int flags)
{
   char path[64], name[128];
   int i;

   for (i = 0; i < 16; i++)
   {
      int fd;
      snprintf(path, sizeof(path), "/dev/input/event%d", i);
      fd = open(path, flags);
      if (fd < 0)
         continue;
      memset(name, 0, sizeof(name));
      if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) >= 0 && strcmp(name, want) == 0)
         return fd;
      close(fd);
   }
   return -1;
}

/* Is a process with this exact command name running? */
static int process_running(const char *comm)
{
   DIR *d = opendir("/proc");
   struct dirent *e;
   int found = 0;

   if (!d)
      return 0;
   while (!found && (e = readdir(d)))
   {
      char path[300], buf[64];
      FILE *f;
      if (e->d_name[0] < '0' || e->d_name[0] > '9')
         continue;
      snprintf(path, sizeof(path), "/proc/%s/comm", e->d_name);
      if (!(f = fopen(path, "r")))
         continue;
      if (fgets(buf, sizeof(buf), f))
      {
         buf[strcspn(buf, "\n")] = 0;
         found = strcmp(buf, comm) == 0;
      }
      fclose(f);
   }
   closedir(d);
   return found;
}

/* extcon reports the USB cable/charger type; any "=1" except host mode means external power. */
static int external_power(void)
{
   char line[64];
   int on = 0;
   FILE *f = fopen(EXTCON_STATE, "r");

   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
   {
      if (strncmp(line, "USB-HOST", 8) == 0 || strncmp(line, "USB_VBUS_EN", 11) == 0)
         continue;
      if (strstr(line, "=1"))
         on = 1;
   }
   fclose(f);
   return on;
}

static int read_int(const char *path, int def)
{
   FILE *f = fopen(path, "r");
   int v;
   if (!f)
      return def;
   if (fscanf(f, "%d", &v) != 1)
      v = def;
   fclose(f);
   return v;
}

/* "key = number" from g36/settings.cfg */
static int setting_int(const char *key, int def)
{
   char line[256];
   size_t n = strlen(key);
   int v = def;
   FILE *f = fopen(SETTINGS, "r");

   if (!f)
      return def;
   while (fgets(line, sizeof(line), f))
   {
      char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (strncmp(p, key, n) || (p[n] != ' ' && p[n] != '\t' && p[n] != '='))
         continue;
      p = strchr(p + n, '=');
      if (p)
         v = atoi(p + 1);
   }
   fclose(f);
   return v;
}

/* what the power button did, in RAM (/tmp/g36d.log; g36-logs doesn't keep it) */
static void dlog(const char *fmt, ...)
{
   FILE *f = fopen("/tmp/g36d.log", "a");
   struct timespec ts;
   va_list ap;
   if (!f)
      return;
   clock_gettime(CLOCK_BOOTTIME, &ts);
   fprintf(f, "%8.2f s  ", ts.tv_sec + ts.tv_nsec / 1e9);
   va_start(ap, fmt);
   vfprintf(f, fmt, ap);
   va_end(ap);
   fputc('\n', f);
   fclose(f);
}

static void battery_log(const char *fmt, int a, int b)
{
   FILE *f = fopen(BATTERY_LOG, "a");
   struct timespec ts;
   if (!f)
      return;
   clock_gettime(CLOCK_BOOTTIME, &ts);
   fprintf(f, "%6ld s  ", (long)ts.tv_sec);
   fprintf(f, fmt, a, b);
   fputc('\n', f);
   fclose(f);
}

/* the time is on the disk before it replaces the old one: a power cut can't leave an empty clock file */
static void save_clock(void)
{
   FILE *f = fopen(CLOCK_FILE ".tmp", "w");
   int ok;
   if (!f)
      return;
   fprintf(f, "%ld\n", (long)time(NULL));
   ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
   if (fclose(f) == 0 && ok)
      rename(CLOCK_FILE ".tmp", CLOCK_FILE);
   else
      unlink(CLOCK_FILE ".tmp");
}

static volatile sig_atomic_t stopping;
static void on_term(int sig)
{
   (void)sig;
   stopping = 1;
}

/* ---- volume, through the sound system's Master control */
static snd_mixer_t *mixer;
static snd_mixer_elem_t *master;

static int mixer_ready(void)
{
   snd_mixer_selem_id_t *sid;
   if (master)
      return 1;
   if (mixer)
      snd_mixer_close(mixer);
   mixer = NULL;
   if (snd_mixer_open(&mixer, 0) < 0)
      return 0;
   if (snd_mixer_attach(mixer, BOARD_MIXER_DEVICE) < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
         snd_mixer_load(mixer) < 0)
   {
      snd_mixer_close(mixer);
      mixer = NULL;
      return 0;
   }
   snd_mixer_selem_id_alloca(&sid);
   snd_mixer_selem_id_set_name(sid, BOARD_MIXER_ELEMENT);
   master = snd_mixer_find_selem(mixer, sid);
   return master != NULL;
}

/* step by 10% like vol_adj; returns the new level in percent, or -1 */
static int volume_step(int dir)
{
   long v, lo, hi, step;
   if (!mixer_ready())
      return -1;
   snd_mixer_handle_events(mixer);
   snd_mixer_selem_get_playback_volume_range(master, &lo, &hi);
   if (hi <= lo || snd_mixer_selem_get_playback_volume(master, SND_MIXER_SCHN_FRONT_LEFT, &v) < 0)
      return -1;
   step = (hi - lo) / 10;
   v += dir * (step ? step : 1);
   v = v < lo ? lo : v > hi ? hi : v;
   snd_mixer_selem_set_playback_volume_all(master, v);
   return (int)((v - lo) * 100 / (hi - lo));
}

/* save the level where vol_adj keeps it ("volume" : N in gamebox.json) */
static void volume_save(int pct)
{
   char buf[2048], out[2100], *p, *q;
   FILE *f = fopen(GAMEBOX_JSON, "r");
   size_t n;
   if (!f || pct < 0)
   {
      if (f)
         fclose(f);
      return;
   }
   n = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[n] = 0;
   if (!(p = strstr(buf, "\"volume\"")) || !(q = strchr(p, ':')))
      return;
   q++;
   while (*q == ' ')
      q++;
   snprintf(out, sizeof(out), "%.*s%d", (int)(q - buf), buf, pct);
   while (*q >= '0' && *q <= '9')
      q++;
   strncat(out, q, sizeof(out) - strlen(out) - 1);
   if ((f = fopen(GAMEBOX_JSON ".tmp", "w")))
   {
      fputs(out, f);
      if (fclose(f) == 0)
         rename(GAMEBOX_JSON ".tmp", GAMEBOX_JSON);
   }
}

static void start_sleep(void)
{
   /* g36-sleep does the rest (save the game's state, close it, flush the card, suspend, wake) */
   if (fork() == 0)
   {
      setsid();
      execl("/usr/sbin/g36-sleep", "g36-sleep", (char *)NULL);
      _exit(1);
   }
}

/* shows a message on top of the game: RetroArch (our build) reads /tmp/g36-osd, "frames:text" */
static void osd(int frames, const char *text)
{
   FILE *f = fopen("/tmp/g36-osd.tmp", "w");
   if (!f)
      return;
   fprintf(f, "%d:%s\n", frames, text);
   fclose(f);
   rename("/tmp/g36-osd.tmp", "/tmp/g36-osd");
}

static void request_halt(void)
{
   FILE *f = fopen(HALT_FLAG, "w");
   if (f)
      fclose(f);
}

int main(void)
{
   int power_fd  = open_input("gpio-keys", O_RDONLY | O_NONBLOCK);
   int vol_fd    = open_input("adc-keys", O_RDONLY | O_NONBLOCK);
   int vol_dir = 0, vol_last = -1;
   long long vol_next = 0;
   long long power_down_at = 0, last_battery = 0, last_clock = now_ms();
   int halted = 0, low_n = 0, last_level = -1;
   long long sleep_confirm_until = 0;
   int hold_msg = 0;

   signal(SIGCHLD, SIG_IGN);

   if (power_fd < 0)
      fprintf(stderr, "g36d: power button device not found\n");

   signal(SIGTERM, on_term);
   signal(SIGINT, on_term);
   save_clock();

   while (!stopping)
   {
      struct pollfd pfd[2] = { { power_fd, POLLIN, 0 }, { vol_fd, POLLIN, 0 } };
      long long t;

      /* wake often only while a volume button is held */
      poll(pfd, 2, vol_dir ? 20 : 250);

      if (power_fd >= 0 && (pfd[0].revents & POLLIN))
      {
         struct input_event ev;
         while (read(power_fd, &ev, sizeof(ev)) == sizeof(ev))
            if (ev.type == EV_KEY && ev.code == KEY_POWER)
            {
               if (ev.value)
               {
                  power_down_at = now_ms();
                  hold_msg = 0;
                  dlog("power down (game running: %s)", process_running("retroarch-1.22") ? "yes" : "no");
               }
               else
               {
                  /* tapped (not held) in a game: two taps in a row put the handheld to sleep.
                   * (In the menu, a tap is the menu's own: screen off and on.) */
                  long long t0 = now_ms();
                  dlog("power up after %lld ms (halted %d, sleeping %d)", power_down_at ? t0 - power_down_at : -1, halted,
                        access(SLEEPING_FLAG, F_OK) == 0);
                  if (power_down_at && !halted && t0 - power_down_at < POWER_HOLD_MS && access(SLEEPING_FLAG, F_OK) != 0
                        && process_running("retroarch-1.22"))
                  {
                     if (t0 < sleep_confirm_until)
                     {
                        sleep_confirm_until = 0;
                        dlog("second tap: sleep");
                        osd(90, "Going to sleep...");
                        start_sleep();
                     }
                     else
                     {
                        sleep_confirm_until = t0 + SLEEP_DOUBLE_MS;
                        dlog("first tap: waiting %d ms for a second", SLEEP_DOUBLE_MS);
                        osd(110, "Tap Power again to sleep");
                     }
                  }
                  power_down_at = 0;
               }
            }
      }

      t = now_ms();

      if (vol_fd >= 0 && (pfd[1].revents & POLLIN))
      {
         struct input_event ev;
         while (read(vol_fd, &ev, sizeof(ev)) == sizeof(ev))
         {
            if (ev.type != EV_KEY || (ev.code != KEY_VOLUMEUP && ev.code != KEY_VOLUMEDOWN))
               continue;
            if (ev.value == 1)
            {
               /* the first step now; repeats start after a moment */
               vol_dir = ev.code == KEY_VOLUMEUP ? 1 : -1;
               vol_last = volume_step(vol_dir);
               vol_next = t + VOL_DELAY_MS;
            }
            else if (ev.value == 0 && vol_dir)
            {
               if (vol_last >= 0)
                  volume_save(vol_last);
               vol_dir = 0;
               vol_last = -1;
            }
         }
      }
      if (vol_dir && t >= vol_next)
      {
         vol_last = volume_step(vol_dir);
         vol_next = t + VOL_REPEAT_MS;
      }

      if (power_down_at && !halted && !hold_msg && t - power_down_at >= HOLD_HINT_MS
            && process_running("retroarch-1.22"))
      {
         osd(120, "Keep holding to save and power off");
         hold_msg = 1;
      }
      if (power_down_at && !halted && t - power_down_at >= POWER_HOLD_MS
            && process_running("retroarch-1.22"))
      {
         dlog("power held %d ms in a game: safe shutdown", POWER_HOLD_MS);
         osd(300, "Saving and powering off...");
         request_halt();
         halted = 1;
      }

      if (t - last_battery >= BATTERY_MS)
      {
         int level = read_int(BATTERY_DIR "/capacity", -1);
         last_battery = t;
         if (external_power() || level < 0)
         {
            low_n = 0;
            last_level = -1;
         }
         else
         {
            int off = setting_int("battery_off_percent", 10);
            if (level != last_level)
               battery_log("%3d%%  voltage %d", level, read_int(BATTERY_DIR "/voltage_now", 0));
            last_level = level;
            low_n = level <= off ? low_n + 1 : 0;
            if (low_n >= BATTERY_LOW_N && !halted)
            {
               battery_log("battery at %d%% (limit %d%%): safe shutdown", level, off);
               request_halt();
               halted = 1;
            }
         }
      }

      if (t - last_clock >= CLOCK_MS)
      {
         last_clock = t;
         save_clock();
      }
   }
   /* stopped at shutdown, just before the system drive goes read-only */
   save_clock();
   return 0;
}
