/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/*
 * g36menu - ES-DE style game menu for the G36 handheld.
 *
 * Systems carousel -> game list (folders shown as folders). Games are read
 * straight from roms/<system>/ on the G36-GAMES partition and re-checked every
 * couple of seconds, so the
 * lists always match the folders. Launching writes /tmp/game.cfg and exits; the
 * startup loop runs the game launcher (retroarch_r) and restarts the menu after.
 */
#define _GNU_SOURCE   /* strcasestr */
#include <alsa/asoundlib.h>
#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <errno.h>
#include <linux/input.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gfx.h"
#include "input.h"
#include "player.h"

#include "../board.h"
#include "cartridge_icon.h"
#define SDCARD     BOARD_GAMES_MOUNT
#define ROMS       SDCARD "/roms"
#define SYSCFG     SDCARD "/g36/systems.cfg"
#define GAMECORES  SDCARD "/g36/game-cores.cfg"
#define STATEFILE  SDCARD "/g36/menu-state"
#define BOOT_SEEN  "/tmp/g36menu-started"   /* made by the first menu start after the handheld was switched on */
#define CORE_DIR   BOARD_EMU_MOUNT "/cores"
#define LIBCACHE   SDCARD "/g36/library.cache"
#define MAX_DEPTH  4                           /* subfolder levels scanned inside a console */
#define ARCADE_NAMES "/usr/share/g36/arcade-names.txt"   /* "shortname<TAB>title", sorted */
#define GAMECFG    "/tmp/game.cfg"
#define HALT_FLAG  "/tmp/halt"
#define REBOOT_FLAG "/tmp/reboot"
#define MENU_LOG   "/tmp/g36menu.log"         /* copied to g36/logs/menu.log at shutdown */
#define ALIVE_FLAG "/tmp/g36-menu-alive"      /* there while the menu runs; it removes it when it quits on purpose.
                                                 * Still there when the start-up loop looks = it crashed */
#define RECOVERED_FLAG "/tmp/g36-recovered"   /* the start-up loop set the saved view, library list and theme aside */
#define SETTINGS   SDCARD "/g36/settings.cfg"
#define BATTERY    BOARD_BATTERY_CAPACITY
#define BAT_WARNED "/tmp/g36-battery-warned"   /* low-battery warning shown (cleared on charger) */
#define FT_TOOL    "/usr/sbin/g36-filetransfer"
#define FT_FLAG    "/tmp/g36-filetransfer"     /* file transfer is on */
#define USB_ASKED  "/tmp/g36-usb-asked"        /* already asked for this plug-in */
#define EXTCON     BOARD_EXTCON_STATE
#define UDC_DIR    BOARD_UDC_DIR
#define FONT       BOARD_UI_FONT

#define MAX_SYSTEMS 64
#define MAX_CORES   8

/* colours (0xAARRGGBB) */
/* ------------------------------------------------------------- theme
 * Every colour that can be themed: a key (as written in g36/theme.cfg), the name the
 * editor shows, its group and its default. The menu reads them through TH() and the
 * C_ names. */
enum {
   TH_BG_TOP, TH_BG_BOT, TH_BG_LIGHT,
   TH_CARD_TOP, TH_CARD_BOT, TH_CARD_LINE,
   TH_LET1, TH_LET2, TH_LET3, TH_LET4, TH_LET5, TH_LET_EDGE,
   TH_ACCENT, TH_TEXT, TH_DIM, TH_FAINT, TH_SEL, TH_PANEL, TH_PICBOX, TH_DOTS,
   TH_BAR, TH_BAR_BOX, TH_BAR_LINE, TH_KEY, TH_KEY_LINE, TH_SUGGEST,
   TH_CARDL_TOP, TH_CARDL_BOT,   /* the light cards' colours: shown with the CARDS rows, worked out from them in a preset */
   TH_COUNT
};
typedef struct { const char *key, *label, *group; uint32_t def, v; } themecol_t;
static themecol_t th[TH_COUNT] = {
   { "background_top",    "Top",             "BACKGROUND",          0xff24152b, 0xff24152b },
   { "background_bottom", "Bottom",          NULL,                  0xff130b17, 0xff130b17 },
   { "background_light",  "Light",           NULL,                  0xff562876, 0xff562876 },
   { "card_top",          "Dark top",        "CARDS",               0xff3f264b, 0xff3f264b },
   { "card_bottom",       "Dark bottom",     NULL,                  0xff22132a, 0xff22132a },
   { "card_outline",      "Outline",         NULL,                  0xff4f3360, 0xff4f3360 },
   { "lettering_1",       "Top",             "LETTERING",           0xffc07dff, 0xffc07dff },
   { "lettering_2",       "Upper middle",    NULL,                  0xffff7fe6, 0xffff7fe6 },
   { "lettering_3",       "Middle",          NULL,                  0xffff5a8f, 0xffff5a8f },
   { "lettering_4",       "Lower middle",    NULL,                  0xffff944c, 0xffff944c },
   { "lettering_5",       "Bottom",          NULL,                  0xffffd816, 0xffffd816 },
   { "lettering_edge",    "3D edge",         NULL,                  0xff5a1ec6, 0xff5a1ec6 },
   { "highlight",         "Highlight",       "TEXT AND HIGHLIGHTS", 0xffcd5cff, 0xffcd5cff },
   { "text",              "Text",            NULL,                  0xffffffff, 0xffffffff },
   { "text_dim",          "Dim text",        NULL,                  0xffab93b5, 0xffab93b5 },
   { "text_faint",        "Faint text",      NULL,                  0xff6e5677, 0xff6e5677 },
   { "selected_row",      "Selected row",    NULL,                  0xff592f6b, 0xff592f6b },
   { "panel",             "Panels",          NULL,                  0xff291830, 0xff291830 },
   { "picture_box",       "Picture box",     NULL,                  0xff392442, 0xff392442 },
   { "dots",              "Position dots",   NULL,                  0xff4c2f5a, 0xff4c2f5a },
   { "bar",               "Bottom bar",      "BARS AND KEYS",       0xff130b16, 0xff130b16 },
   { "bar_box",           "Bar boxes",       NULL,                  0xff2a1832, 0xff2a1832 },
   { "bar_box_outline",   "Bar box outline", NULL,                  0xff4b3158, 0xff4b3158 },
   { "key",               "Keys",            NULL,                  0xff4a2f58, 0xff4a2f58 },
   { "key_outline",       "Key outline",     NULL,                  0xff7a5a8c, 0xff7a5a8c },
   { "suggestion",        "Suggestions",     NULL,                  0xff5a3a6c, 0xff5a3a6c },
   { "card_light_top",    "Light top",       "CARDS",               0xff8a6a98, 0xff8a6a98 },   /* (worked out at start from the card colour and accent) */
   { "card_light_bottom", "Light bottom",    NULL,                  0xff6a4a78, 0xff6a4a78 },
};
#define TH(i) (th[i].v)
#define C_PANEL     TH(TH_PANEL)
#define C_ACCENT    TH(TH_ACCENT)
#define C_TEXT      TH(TH_TEXT)
#define C_DIM       TH(TH_DIM)
#define C_FAINT     TH(TH_FAINT)
#define C_SEL       TH(TH_SEL)
#define C_GOLD      0xffffc94a
#define C_LOW       0xffff5a5a
#define C_CHARGE    0xff5ad16a
#define C_THUMB     0xff574560   /* the square of a game with no thumbnail */

/* ------------------------------------------------------------- utilities */

/* one line in the menu log, stamped with seconds since boot */
static void mlog(const char *fmt, ...)
{
   struct timespec ts;
   va_list ap;
   clock_gettime(CLOCK_BOOTTIME, &ts);
   fprintf(stderr, "%6ld s  ", (long)ts.tv_sec);
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}

static double now_s(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static char *trim(char *s)
{
   char *e;
   while (isspace((unsigned char)*s))
      s++;
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1]))
      *--e = 0;
   return s;
}

/* Case-insensitive natural order ("Game 2" before "Game 10"). */
static int natcmp(const char *a, const char *b)
{
   while (*a && *b)
   {
      if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b))
      {
         unsigned long long na = strtoull(a, (char **)&a, 10);
         unsigned long long nb = strtoull(b, (char **)&b, 10);
         if (na != nb)
            return na < nb ? -1 : 1;
         continue;
      }
      if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
         return tolower((unsigned char)*a) - tolower((unsigned char)*b);
      a++;
      b++;
   }
   return (unsigned char)*a - (unsigned char)*b;
}

/* ------------------------------------------------------------- systems */

/* one game in the library: path inside roms/<console>/, and the name the menu shows */
typedef struct
{
   char *path, *label;
} game_t;

/* a scanned folder's fingerprint: entry count (-1 = missing) and a checksum of the names */
typedef struct
{
   char *rel;
   int n;
   unsigned long long hash;
   long long mtime;   /* the folder's modification time (ns) when it was last looked at in full; 0 = not known */
   time_t full;       /* and when */
} dirsig_t;

typedef struct
{
   char id[32], name[64], exts[160];
   char cores[MAX_CORES][40];
   int ncores;
   int arcade;          /* zips named by short name (kof97.zip): look up the real title */
   int count;           /* games found (recursive) */
   game_t *games;       /* the library for this console (from the cache or a scan) */
   int ngames, gcap;
   dirsig_t *dirs;      /* every folder scanned, to spot changes */
   int ndirs, dcap;
   int known;           /* library loaded from the cache or scanned this session */
} system_t;

static system_t systems[MAX_SYSTEMS];
static int nsystems;

static void load_systems(void)
{
   FILE *f = fopen(SYSCFG, "r");
   char line[512];

   nsystems = 0;
   if (!f)
      return;
   while (fgets(line, sizeof(line), f) && nsystems < MAX_SYSTEMS)
   {
      char *field[4], *p = trim(line), *core;
      int n = 0;
      system_t *s = &systems[nsystems];

      if (!*p || *p == '#')
         continue;
      field[n++] = p;
      while (n < 4 && (p = strchr(p, '|')))
      {
         *p++ = 0;
         field[n++] = p;
      }
      if (n < 4)
         continue;
      memset(s, 0, sizeof(*s));
      snprintf(s->id, sizeof(s->id), "%s", trim(field[0]));
      snprintf(s->name, sizeof(s->name), "%s", trim(field[1]));
      snprintf(s->exts, sizeof(s->exts), ",%s,", trim(field[2]));
      for (core = strtok(trim(field[3]), " \t"); core && s->ncores < MAX_CORES; core = strtok(NULL, " \t"))
         snprintf(s->cores[s->ncores++], sizeof(s->cores[0]), "%s", core);
      s->arcade = s->ncores && (!strncmp(s->cores[0], "fbneo", 5) || !strncmp(s->cores[0], "mame", 4)
            || !strncmp(s->cores[0], "fbalpha", 7));
      nsystems++;
   }
   fclose(f);
}

static int skip_name(const char *n)
{
   return n[0] == '.' || !strcmp(n, "images") || !strcmp(n, "media") ||
          !strcmp(n, "videos") || !strcmp(n, "manuals") || !strcmp(n, "downloaded_media");
}

static int ext_match(const system_t *s, const char *name)
{
   const char *dot = strrchr(name, '.');
   char key[24];
   size_t i;

   if (!dot || strlen(dot + 1) >= sizeof(key) - 2)
      return 0;
   key[0] = ',';
   for (i = 0; dot[1 + i]; i++)
      key[1 + i] = tolower((unsigned char)dot[1 + i]);
   key[1 + i] = ',';
   key[2 + i] = 0;
   return strstr(s->exts, key) != NULL;
}

/* Disc files listed in a folder's .m3u playlists: the playlist is shown, its discs aren't. */
typedef struct
{
   char **name;
   int n, cap;
} nameset_t;

static void nameset_free(nameset_t *ns)
{
   int i;
   for (i = 0; i < ns->n; i++)
      free(ns->name[i]);
   free(ns->name);
   memset(ns, 0, sizeof(*ns));
}

static int nameset_has(const nameset_t *ns, const char *name)
{
   int i;
   for (i = 0; i < ns->n; i++)
      if (!strcasecmp(ns->name[i], name))
         return 1;
   return 0;
}

static void m3u_discs(const char *dir, nameset_t *ns)
{
   DIR *d = opendir(dir);
   struct dirent *e;

   memset(ns, 0, sizeof(*ns));
   if (!d)
      return;
   while ((e = readdir(d)))
   {
      const char *dot = strrchr(e->d_name, '.');
      char path[1024], line[512];
      FILE *f;
      if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".m3u"))
         continue;
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      if (!(f = fopen(path, "r")))
         continue;
      while (fgets(line, sizeof(line), f))
      {
         char *l = trim(line), *slash;
         if (!*l || *l == '#')
            continue;
         /* only discs in this folder matter here; they're listed by name or ./name */
         slash = strrchr(l, '/');
         if (slash && strncmp(l, "./", 2))
            continue;
         if (ns->n == ns->cap)
         {
            ns->cap = ns->cap ? ns->cap * 2 : 8;
            ns->name = realloc(ns->name, ns->cap * sizeof(char *));
         }
         ns->name[ns->n++] = strdup(slash ? slash + 1 : l);
      }
      fclose(f);
   }
   closedir(d);
}

/* Indices of systems that currently have games, in config order. */
static int visible[MAX_SYSTEMS], nvisible;

static int refresh_counts(void);
static void thumbs_clear(void);
static void large_clear(void);
static void gcard_clear(void);

/* ------------------------------------------------------------- game list */

typedef struct
{
   char *name;       /* file or folder name */
   char *label;      /* what the list shows */
   int is_dir;
   int sys;          /* index into systems[] */
} entry_t;

typedef struct
{
   entry_t *e;
   int n, cap;
} list_t;

static void list_free(list_t *l)
{
   int i;
   for (i = 0; i < l->n; i++)
   {
      free(l->e[i].name);
      free(l->e[i].label);
   }
   free(l->e);
   memset(l, 0, sizeof(*l));
}

static int entry_cmp(const void *a, const void *b)
{
   const entry_t *x = a, *y = b;
   int c;
   if (x->is_dir != y->is_dir)
      return y->is_dir - x->is_dir;
   c = natcmp(x->label, y->label);
   return c ? c : natcmp(x->name, y->name);
}

/* ---- clean names: "Legend of Zelda, The - Link's Awakening DX (USA) (Rev 2).zip"
 *      shows as "The Legend of Zelda - Link's Awakening DX" */

static void clean_name(const char *in, char *out, size_t sz)
{
   static const char *const articles[] = { "The", "A", "An" };
   char buf[512], *o = buf, *dash;
   const char *orig = in;
   int depth = 0;
   size_t i;

   /* drop (...) and [...] tags, squeeze spaces */
   for (; *in && o < buf + sizeof(buf) - 1; in++)
   {
      if (*in == '(' || *in == '[')
         depth++;
      else if ((*in == ')' || *in == ']') && depth)
         depth--;
      else if (!depth && !(*in == ' ' && (o == buf || o[-1] == ' ')))
         *o++ = *in;
   }
   *o = 0;
   while (o > buf && (o[-1] == ' ' || o[-1] == '-' || o[-1] == ','))
      *--o = 0;

   /* move a trailing article to the front: "Legend of Zelda, The - DX" */
   dash = strstr(buf, " - ");
   for (i = 0; i < sizeof(articles) / sizeof(*articles); i++)
   {
      char suffix[8];
      size_t sl, end = dash ? (size_t)(dash - buf) : strlen(buf);
      snprintf(suffix, sizeof(suffix), ", %s", articles[i]);
      sl = strlen(suffix);
      if (end > sl && !strncmp(buf + end - sl, suffix, sl))
      {
         char tmp[512];
         snprintf(tmp, sizeof(tmp), "%s %.*s%s", articles[i], (int)(end - sl), buf, buf + end);
         snprintf(buf, sizeof(buf), "%s", tmp);
         break;
      }
   }
   snprintf(out, sz, "%s", buf[0] ? buf : orig);
}

/* arcade titles, loaded the first time an arcade list opens */
static char *arcade_data;
static char **arcade_key;     /* pairs: key, title */
static int arcade_n, arcade_tried;

static void arcade_load(void)
{
   FILE *f;
   long len;
   char *p;
   int cap = 0;

   arcade_tried = 1;
   if (!(f = fopen(ARCADE_NAMES, "r")))
      return;
   fseek(f, 0, SEEK_END);
   len = ftell(f);
   fseek(f, 0, SEEK_SET);
   arcade_data = malloc(len + 1);
   if (!arcade_data || fread(arcade_data, 1, len, f) != (size_t)len)
   {
      fclose(f);
      free(arcade_data);
      arcade_data = NULL;
      return;
   }
   fclose(f);
   arcade_data[len] = 0;
   for (p = arcade_data; *p; )
   {
      char *nl = strchr(p, '\n'), *tab = strchr(p, '\t');
      if (nl)
         *nl = 0;
      if (tab && (!nl || tab < nl))
      {
         *tab = 0;
         if (arcade_n == cap)
         {
            cap = cap ? cap * 2 : 8192;
            arcade_key = realloc(arcade_key, cap * 2 * sizeof(char *));
         }
         arcade_key[arcade_n * 2] = p;
         arcade_key[arcade_n * 2 + 1] = tab + 1;
         arcade_n++;
      }
      if (!nl)
         break;
      p = nl + 1;
   }
   mlog("arcade names: %d", arcade_n);
}

static const char *arcade_title(const char *shortname)
{
   int lo = 0, hi;
   if (!arcade_tried)
      arcade_load();
   hi = arcade_n - 1;
   while (lo <= hi)
   {
      int mid = (lo + hi) / 2, c = strcmp(arcade_key[mid * 2], shortname);
      if (!c)
         return arcade_key[mid * 2 + 1];
      if (c < 0)
         lo = mid + 1;
      else
         hi = mid - 1;
   }
   return NULL;
}

/* the name with its tags, used when two games would otherwise look the same */
static void full_label(const system_t *s, const entry_t *e, char *out, size_t sz)
{
   char base[512];
   const char *dot, *t;
   snprintf(base, sizeof(base), "%s", e->name);
   if (!e->is_dir && (dot = strrchr(base, '.')))
      base[dot - base] = 0;
   if (!e->is_dir && s->arcade && (t = arcade_title(base)))
      snprintf(out, sz, "%s", t);
   else
      snprintf(out, sz, "%s", base);
}

static char *make_label(const system_t *s, const entry_t *e)
{
   char full[512], clean[512];
   if (e->is_dir)
      return strdup(e->name);
   full_label(s, e, full, sizeof(full));
   clean_name(full, clean, sizeof(clean));
   return strdup(clean);
}

/* ------------------------------------------------------------- library cache
 * Each console's games are kept in g36/library.cache between boots. A folder counts
 * as changed when its entry count or the checksum of its names differs; timestamps
 * can't be used, because without a clock every file the handheld writes gets the
 * same date (exFAT's earliest, 1980). Only consoles with a changed folder are rescanned.
 */

static unsigned long long name_hash(const char *n)
{
   unsigned long long h = 1469598103934665603ULL;
   for (; *n; n++)
   {
      h ^= (unsigned char)*n;
      h *= 1099511628211ULL;
   }
   return h;
}

static void dir_sig(const char *dir, int *n, unsigned long long *hash)
{
   DIR *d = opendir(dir);
   struct dirent *e;

   *n = -1;
   *hash = 0;
   if (!d)
      return;
   *n = 0;
   while ((e = readdir(d)))
   {
      const char *dot = strrchr(e->d_name, '.');
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
         continue;
      (*n)++;
      *hash += name_hash(e->d_name);
      /* a playlist can change without its name changing */
      if (dot && !strcasecmp(dot, ".m3u"))
      {
         char path[1024];
         struct stat st;
         snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
         if (stat(path, &st) == 0)
            *hash += (unsigned long long)st.st_size * 2654435761ULL;
      }
   }
   closedir(d);
}

/* a folder's modification time in nanoseconds (0 if it can't be read); it changes when a name is added, removed or renamed in it */
static long long dir_mtime(const char *dir)
{
   struct stat st;
   if (stat(dir, &st) != 0)
      return 0;
   return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
}

static void sys_dir(const system_t *s, const char *rel, char *out, size_t sz)
{
   snprintf(out, sz, "%s/%s%s%s", ROMS, s->id, rel[0] ? "/" : "", rel);
}

static void lib_clear(system_t *s)
{
   int i;
   for (i = 0; i < s->ngames; i++)
   {
      free(s->games[i].path);
      free(s->games[i].label);
   }
   for (i = 0; i < s->ndirs; i++)
      free(s->dirs[i].rel);
   free(s->games);
   free(s->dirs);
   s->games = NULL;
   s->dirs = NULL;
   s->ngames = s->gcap = s->ndirs = s->dcap = 0;
}

static void lib_add_dir(system_t *s, const char *rel, int n, unsigned long long hash, long long mtime)
{
   if (s->ndirs == s->dcap)
   {
      s->dcap = s->dcap ? s->dcap * 2 : 8;
      s->dirs = realloc(s->dirs, s->dcap * sizeof(dirsig_t));
   }
   s->dirs[s->ndirs].rel = strdup(rel);
   s->dirs[s->ndirs].n = n;
   s->dirs[s->ndirs].hash = hash;
   s->dirs[s->ndirs].mtime = mtime;
   s->dirs[s->ndirs].full = time(NULL);
   s->ndirs++;
}

/* takes ownership of label */
static void lib_add_game(system_t *s, const char *path, char *label)
{
   if (s->ngames == s->gcap)
   {
      s->gcap = s->gcap ? s->gcap * 2 : 64;
      s->games = realloc(s->games, s->gcap * sizeof(game_t));
   }
   s->games[s->ngames].path = strdup(path);
   s->games[s->ngames].label = label;
   s->ngames++;
}

static void scan_dir(system_t *s, const char *rel, int depth)
{
   char dir[1024];
   unsigned long long hash;
   nameset_t discs;
   struct dirent *e;
   DIR *d;
   int n;
   long long mt;

   sys_dir(s, rel, dir, sizeof(dir));
   mt = dir_mtime(dir);   /* (before the folder is read, so a change in between shows up next time) */
   dir_sig(dir, &n, &hash);
   lib_add_dir(s, rel, n, hash, mt);
   if (n <= 0 || !(d = opendir(dir)))
      return;
   m3u_discs(dir, &discs);
   while ((e = readdir(d)))
   {
      char child[1024];
      int is_dir = e->d_type == DT_DIR;
      if (skip_name(e->d_name) || nameset_has(&discs, e->d_name))
         continue;
      if (e->d_type == DT_UNKNOWN)
      {
         char path[1300];
         struct stat st;
         snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
         is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
      }
      snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
      if (is_dir)
      {
         if (depth < MAX_DEPTH)
            scan_dir(s, child, depth + 1);
      }
      else if (ext_match(s, e->d_name))
      {
         entry_t tmp = { e->d_name, NULL, 0, (int)(s - systems) };
         lib_add_game(s, child, make_label(s, &tmp));
      }
   }
   closedir(d);
   nameset_free(&discs);
}

/* The folders are checked for changes on a thread of their own (a big folder takes ~150 ms to
 * read, which the menu would feel as a stall); it only reads a console's folder list. This
 * lock is held while that list is made or thrown away, and while the thread reads it. */
static pthread_mutex_t lib_m = PTHREAD_MUTEX_INITIALIZER;

static void lib_scan(system_t *s)
{
   pthread_mutex_lock(&lib_m);
   lib_clear(s);
   scan_dir(s, "", 0);
   s->count = s->ngames;
   s->known = 1;
   pthread_mutex_unlock(&lib_m);
}

/* Has anything changed in a console's folders? A folder whose modification time hasn't moved is not read again (reading every
 * folder every couple of seconds was a few percent of a core on a still menu); every 30 seconds each is read in full anyway,
 * which also catches a playlist's contents changing under the same name. */
static int lib_changed(system_t *s)
{
   int i;
   time_t now = time(NULL);
   if (!s->known)
      return 1;
   for (i = 0; i < s->ndirs; i++)
   {
      char dir[1024];
      unsigned long long hash;
      int n;
      long long mt;
      sys_dir(s, s->dirs[i].rel, dir, sizeof(dir));
      mt = dir_mtime(dir);
      if (mt && mt == s->dirs[i].mtime && now - s->dirs[i].full < 30)
         continue;
      dir_sig(dir, &n, &hash);
      if (n != s->dirs[i].n || hash != s->dirs[i].hash)
         return 1;
      s->dirs[i].mtime = mt;
      s->dirs[i].full = now;
   }
   return 0;
}

/* a cache made with a different systems.cfg or arcade name list is thrown away */
static unsigned long long config_sig(void)
{
   unsigned long long h = 0;
   char line[512];
   struct stat st;
   FILE *f = fopen(SYSCFG, "r");
   if (f)
   {
      while (fgets(line, sizeof(line), f))
      {
         /* only id | name | extensions decide which games exist; the emulator
          * column can change (the emulator picker) without a rescan */
         char *p = line;
         int bars = 0;
         for (; *p && bars < 3; p++)
            if (*p == '|')
               bars++;
         if (bars == 3)
            p[-1] = 0;
         h = h * 31 + name_hash(line);
      }
      fclose(f);
   }
   if (stat(ARCADE_NAMES, &st) == 0)
      h += (unsigned long long)st.st_size;
   return h;
}

static void lib_save(void)
{
   char tmp[300];
   FILE *f;
   int i, j;

   snprintf(tmp, sizeof(tmp), "%s.tmp", LIBCACHE);
   if (!(f = fopen(tmp, "w")))
   {
      mlog("library: can't write the cache (%s)", strerror(errno));
      return;
   }
   fprintf(f, "G36LIB 1 %llu\n", config_sig());
   for (i = 0; i < nsystems; i++)
   {
      system_t *s = &systems[i];
      if (!s->known)
         continue;
      fprintf(f, "S %s\n", s->id);
      for (j = 0; j < s->ndirs; j++)
         fprintf(f, "D %d %llu %s\n", s->dirs[j].n, s->dirs[j].hash, s->dirs[j].rel);
      for (j = 0; j < s->ngames; j++)
         fprintf(f, "G %s\t%s\n", s->games[j].path, s->games[j].label);
   }
   if (fclose(f) == 0)
      rename(tmp, LIBCACHE);
}

static void lib_load(void)
{
   FILE *f = fopen(LIBCACHE, "r");
   char line[1500];
   unsigned long long sig;
   system_t *s = NULL;
   int i, ok = 0;

   if (!f)
      return;
   if (fgets(line, sizeof(line), f) && sscanf(line, "G36LIB 1 %llu", &sig) == 1 && sig == config_sig())
      ok = 1;
   while (ok && fgets(line, sizeof(line), f))
   {
      line[strcspn(line, "\n")] = 0;
      if (!strncmp(line, "S ", 2))
      {
         s = NULL;
         for (i = 0; i < nsystems; i++)
            if (!strcmp(systems[i].id, line + 2))
               s = &systems[i];
         if (s)
         {
            lib_clear(s);
            s->known = 1;
         }
      }
      else if (s && !strncmp(line, "D ", 2))
      {
         int n, off = 0;
         unsigned long long hash;
         if (sscanf(line + 2, "%d %llu %n", &n, &hash, &off) >= 2 && off)
            lib_add_dir(s, line + 2 + off, n, hash, 0);
      }
      else if (s && !strncmp(line, "G ", 2))
      {
         char *tab = strchr(line + 2, '\t');
         if (tab)
         {
            *tab = 0;
            lib_add_game(s, line + 2, strdup(tab + 1));
         }
      }
   }
   fclose(f);
   for (i = 0; i < nsystems; i++)
      systems[i].count = systems[i].ngames;
   if (!ok)
      mlog("library: cache out of date, rescanning everything");
}

static int sys_keep(void);
static void sys_restore(int keep);

/* check every console, rescan the changed ones, and rebuild the visible list; mask (from the
 * folder-watching thread) says which consoles changed, NULL = look now */
static int refresh_counts_x(const char *mask)
{
   int i, changed = 0, keep = sys_keep();
   nvisible = 0;
   for (i = 0; i < nsystems; i++)
   {
      if (mask ? mask[i] : lib_changed(&systems[i]))
      {
         lib_scan(&systems[i]);
         changed = 1;
         mlog("library: scanned %s (%d games)", systems[i].id, systems[i].ngames);
      }
      if (systems[i].count > 0)
         visible[nvisible++] = i;
   }
   if (changed)
      lib_save();
   sys_restore(keep);
   return changed;
}

static int refresh_counts(void)
{
   return refresh_counts_x(NULL);
}

/* ---- the folder watcher: every console's folders checked on a thread (see lib_m) */
static pthread_t watch_th;
static pthread_mutex_t watch_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t watch_c = PTHREAD_COND_INITIALIZER;
static int watch_started, watch_req;
static int watch_state;                 /* 0 idle, 1 checking, 2 results in watch_changed */
static char watch_changed[MAX_SYSTEMS];

static void *watch_main(void *arg)
{
   (void)arg;
   for (;;)
   {
      int i;
      pthread_mutex_lock(&watch_m);
      while (!watch_req)
         pthread_cond_wait(&watch_c, &watch_m);
      watch_req = 0;
      pthread_mutex_unlock(&watch_m);
      for (i = 0; i < nsystems; i++)
      {
         pthread_mutex_lock(&lib_m);
         watch_changed[i] = (char)lib_changed(&systems[i]);
         pthread_mutex_unlock(&lib_m);
      }
      __atomic_store_n(&watch_state, 2, __ATOMIC_RELEASE);
   }
   return NULL;
}

/* ask for a check (the results come in watch_state == 2); 0 if one is already going */
static int watch_start(void)
{
   if (__atomic_load_n(&watch_state, __ATOMIC_ACQUIRE) != 0)
      return 0;
   if (!watch_started)
   {
      if (pthread_create(&watch_th, NULL, watch_main, NULL) != 0)
         return 0;
      pthread_detach(watch_th);
      watch_started = 1;
   }
   __atomic_store_n(&watch_state, 1, __ATOMIC_RELEASE);
   pthread_mutex_lock(&watch_m);
   watch_req = 1;
   pthread_cond_signal(&watch_c);
   pthread_mutex_unlock(&watch_m);
   return 1;
}

/* games of one console whose clean names collide (regions, revisions) keep their tags */
static void list_finish(list_t *l)
{
   int i;
   qsort(l->e, l->n, sizeof(entry_t), entry_cmp);
   for (i = 0; i < l->n; )
   {
      int j = i + 1, k;
      while (j < l->n && !l->e[j].is_dir && !l->e[i].is_dir && l->e[j].sys == l->e[i].sys
            && !strcasecmp(l->e[i].label, l->e[j].label))
         j++;
      if (j - i > 1)
         for (k = i; k < j; k++)
         {
            char full[512];
            const char *slash = strrchr(l->e[k].name, '/');
            entry_t base = l->e[k];
            if (slash)
               base.name = (char *)slash + 1;   /* All Games names hold the path */
            full_label(&systems[l->e[k].sys], &base, full, sizeof(full));
            free(l->e[k].label);
            l->e[k].label = strdup(full);
         }
      i = j;
   }
   qsort(l->e, l->n, sizeof(entry_t), entry_cmp);
}

static void list_add(list_t *l, const char *name, const char *label, int is_dir, int sys)
{
   if (l->n == l->cap)
   {
      l->cap = l->cap ? l->cap * 2 : 64;
      l->e = realloc(l->e, l->cap * sizeof(entry_t));
   }
   l->e[l->n].name = strdup(name);
   l->e[l->n].label = strdup(label);
   l->e[l->n].is_dir = is_dir;
   l->e[l->n].sys = sys;
   l->n++;
}

/* the games and game-holding subfolders directly inside folder ("" = the console's top) */
static void list_from_library(list_t *l, const system_t *s, const char *folder)
{
   size_t plen = strlen(folder);
   int i, k;

   list_free(l);
   if (!s)
      return;
   for (i = 0; i < s->ngames; i++)
   {
      const char *rest = s->games[i].path, *slash;
      if (plen)
      {
         if (strncmp(rest, folder, plen) || rest[plen] != '/')
            continue;
         rest += plen + 1;
      }
      if (!(slash = strchr(rest, '/')))
         list_add(l, rest, s->games[i].label, 0, (int)(s - systems));
      else
      {
         char dname[256];
         int seen = 0;
         snprintf(dname, sizeof(dname), "%.*s", (int)(slash - rest), rest);
         for (k = 0; k < l->n && !seen; k++)
            seen = l->e[k].is_dir && !strcmp(l->e[k].name, dname);
         if (!seen)
            list_add(l, dname, dname, 1, (int)(s - systems));
      }
   }
   list_finish(l);
}

/* every game on every console, flat; names are paths inside roms/<console>/ */
static void list_all_games(list_t *l)
{
   int i, j;
   list_free(l);
   for (i = 0; i < nvisible; i++)
   {
      system_t *s = &systems[visible[i]];
      for (j = 0; j < s->ngames; j++)
         list_add(l, s->games[j].path, s->games[j].label, 0, visible[i]);
   }
   list_finish(l);
}

static int total_games(void)
{
   int i, n = 0;
   for (i = 0; i < nvisible; i++)
      n += systems[visible[i]].count;
   return n;
}

/* ------------------------------------------------------------- collections
 * All Games, Recently Played, Favorites, then one per text file in collections/.
 * Recently Played, Favorites and custom collections are plain text: one game per
 * line, as its path inside roms/ ("gba/Pokemon - Emerald Version (USA, Europe).gba").
 */
#define RECENT_FILE SDCARD "/g36/recent.txt"
#define FAVS_FILE   SDCARD "/g36/favorites.txt"
#define COLL_DIR    SDCARD "/collections"
#define RECENT_MAX  30
#define MAX_COLLS   32

#define COLL_ALL    0
#define COLL_RECENT 1
#define COLL_SEARCH 2      /* not a list of its own: the search screen, then its results */
#define COLL_FAVS   3
#define COLL_BUILTIN 4

typedef struct
{
   char name[64];
   char file[300];   /* "" for All Games */
   int count;
} coll_t;

static coll_t colls[MAX_COLLS];
static int ncolls;

/* "gba/Some Game.gba" -> console and game in the library */
static int game_find(const char *key, int *sys, int *gi)
{
   const char *slash = strchr(key, '/');
   int i, j;
   if (!slash)
      return 0;
   for (i = 0; i < nsystems; i++)
   {
      if (strncmp(systems[i].id, key, slash - key) || systems[i].id[slash - key])
         continue;
      for (j = 0; j < systems[i].ngames; j++)
         if (!strcmp(systems[i].games[j].path, slash + 1))
         {
            *sys = i;
            *gi = j;
            return 1;
         }
   }
   return 0;
}

/* read a collection file's lines (keys), skipping blanks and # comments */
static int read_keys(const char *file, char ***keys)
{
   FILE *f = fopen(file, "r");
   char line[1200];
   int n = 0, cap = 0;
   *keys = NULL;
   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
   {
      char *k = trim(line);
      if (!*k || *k == '#')
         continue;
      if (n == cap)
      {
         cap = cap ? cap * 2 : 32;
         *keys = realloc(*keys, cap * sizeof(char *));
      }
      (*keys)[n++] = strdup(k);
   }
   fclose(f);
   return n;
}

static void free_keys(char **keys, int n)
{
   int i;
   for (i = 0; i < n; i++)
      free(keys[i]);
   free(keys);
}

/* Finish a file that is about to replace another with rename(): its data has to be on the card first, or a
 * power cut can leave the new name on an empty or half-written file (the card's exFAT, unlike ext4, doesn't
 * guard against that). 0 = written out completely. Not for files that change many times a second. */
static int commit_close(FILE *f)
{
   int ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
   return fclose(f) == 0 && ok ? 0 : -1;
}

static void write_keys(const char *file, char **keys, int n)
{
   char tmp[320];
   FILE *f;
   int i;
   snprintf(tmp, sizeof(tmp), "%s.tmp", file);
   if (!(f = fopen(tmp, "w")))
      return;
   for (i = 0; i < n; i++)
      fprintf(f, "%s\n", keys[i]);
   if (commit_close(f) == 0)
      rename(tmp, file);
   else
      unlink(tmp);
}

/* the games of a collection file that are on the card; keep_order for Recently Played */
static void list_from_file(list_t *l, const char *file, int keep_order)
{
   char **keys;
   int n = read_keys(file, &keys), i, sys, gi;
   list_free(l);
   for (i = 0; i < n; i++)
      if (game_find(keys[i], &sys, &gi))
         list_add(l, systems[sys].games[gi].path, systems[sys].games[gi].label, 0, sys);
   free_keys(keys, n);
   if (!keep_order)
      list_finish(l);
}

/* ---- search: what's been typed, and the games that match it */
static char search_q[48];

/* lower case letters and digits, everything else one space; accented letters lose
 * their accents ("Pokémon" -> "pokemon") */
static void search_norm(const char *in, char *out, size_t sz)
{
   static const char from[] = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";   /* U+00E0.. */
   size_t o = 0;
   const unsigned char *p = (const unsigned char *)in;
   int space = 1;
   while (*p && o + 1 < sz)
   {
      int c = *p++;
      if (c == 0xc3 && *p >= 0x80 && *p <= 0xbf)
      {
         /* a Latin-1 letter in UTF-8: its plain letter */
         int k = (*p++ & 0x3f) | 0x40;   /* 0x40..0x7f: U+00C0..U+00FF */
         c = from[k & 0x1f];
         if (c == '/')
            c = ' ';
      }
      else if (c >= 0x80)
         continue;
      c = tolower(c);
      if (isalnum(c))
      {
         out[o++] = (char)c;
         space = 0;
      }
      else if (!space)
      {
         out[o++] = ' ';
         space = 1;
      }
   }
   while (o && out[o - 1] == ' ')
      o--;
   out[o] = 0;
}

/* every word typed starts a word of the name, in any order ("em" finds Emerald, not
 * the "em" inside Pokemon) */
static int word_start(const char *name, const char *word)
{
   const char *p = name;
   size_t n = strlen(word);
   while ((p = strstr(p, word)))
   {
      if (p == name || p[-1] == ' ')
         return 1;
      p++;
   }
   (void)n;
   return 0;
}

static int search_match(const char *qnorm, const char *label)
{
   char name[600], word[64];
   const char *q = qnorm;
   search_norm(label, name, sizeof(name));
   while (*q)
   {
      size_t n = strcspn(q, " ");
      if (n >= sizeof(word))
         n = sizeof(word) - 1;
      memcpy(word, q, n);
      word[n] = 0;
      if (!word_start(name, word))
         return 0;
      q += n;
      while (*q == ' ')
         q++;
   }
   return 1;
}

static void list_search(list_t *l)
{
   char q[sizeof(search_q) * 2];
   int i, j;
   list_free(l);
   search_norm(search_q, q, sizeof(q));
   if (!q[0])
      return;
   for (i = 0; i < nvisible; i++)
   {
      system_t *s = &systems[visible[i]];
      for (j = 0; j < s->ngames; j++)
         if (search_match(q, s->games[j].label))
            list_add(l, s->games[j].path, s->games[j].label, 0, visible[i]);
   }
   list_finish(l);
   {
      /* names that start with what's typed come first ("M": Mad Max before Alex Kidd
       * in Miracle World), each group still in name order */
      entry_t *sorted = malloc((size_t)(l->n ? l->n : 1) * sizeof(entry_t));
      int pass, n = 0;
      size_t ql = strlen(q);
      if (sorted)
      {
         for (pass = 0; pass < 2; pass++)
            for (i = 0; i < l->n; i++)
            {
               char name[600];
               search_norm(l->e[i].label, name, sizeof(name));
               if ((strncmp(name, q, ql) == 0) == (pass == 0))
                  sorted[n++] = l->e[i];
            }
         memcpy(l->e, sorted, (size_t)l->n * sizeof(entry_t));
         free(sorted);
      }
   }
}

static void list_collection(list_t *l, int ci)
{
   if (ci == COLL_ALL)
      list_all_games(l);
   else if (ci == COLL_SEARCH)
      list_search(l);
   else
      list_from_file(l, colls[ci].file, ci == COLL_RECENT);
}

static int coll_cmp(const void *a, const void *b)
{
   return natcmp(((const coll_t *)a)->name, ((const coll_t *)b)->name);
}

/* the collections row: the four built in, then the .txt files in collections/, by name */
static void load_collections(void)
{
   DIR *d;
   struct dirent *e;
   list_t tmp = { 0 };
   int i;

   memset(colls, 0, sizeof(colls));
   snprintf(colls[COLL_ALL].name, sizeof(colls[0].name), "All Games");
   snprintf(colls[COLL_SEARCH].name, sizeof(colls[0].name), "Search");
   snprintf(colls[COLL_RECENT].name, sizeof(colls[0].name), "Recently Played");
   snprintf(colls[COLL_RECENT].file, sizeof(colls[0].file), "%s", RECENT_FILE);
   snprintf(colls[COLL_FAVS].name, sizeof(colls[0].name), "Favorites");
   snprintf(colls[COLL_FAVS].file, sizeof(colls[0].file), "%s", FAVS_FILE);
   ncolls = COLL_BUILTIN;
   if ((d = opendir(COLL_DIR)))
   {
      while ((e = readdir(d)) && ncolls < MAX_COLLS)
      {
         const char *dot = strrchr(e->d_name, '.');
         if (e->d_name[0] == '.' || !dot || strcasecmp(dot, ".txt") || dot == e->d_name)
            continue;
         snprintf(colls[ncolls].name, sizeof(colls[ncolls].name), "%.*s", (int)(dot - e->d_name), e->d_name);
         snprintf(colls[ncolls].file, sizeof(colls[ncolls].file), "%s/%s", COLL_DIR, e->d_name);
         ncolls++;
      }
      closedir(d);
      qsort(colls + COLL_BUILTIN, ncolls - COLL_BUILTIN, sizeof(coll_t), coll_cmp);
   }
   colls[COLL_ALL].count = total_games();
   for (i = COLL_RECENT; i < ncolls; i++)
   {
      if (i == COLL_SEARCH)
         continue;
      list_from_file(&tmp, colls[i].file, 1);
      colls[i].count = tmp.n;
   }
   list_free(&tmp);
}

/* ---- favorites, kept in memory for the stars in the lists */
static char **favs;
static int nfavs;

static void load_favs(void)
{
   free_keys(favs, nfavs);
   nfavs = read_keys(FAVS_FILE, &favs);
}

static int is_fav(const char *key)
{
   int i;
   for (i = 0; i < nfavs; i++)
      if (!strcmp(favs[i], key))
         return 1;
   return 0;
}

/* returns 1 if it's now a favorite */
static int toggle_fav(const char *key)
{
   int i;
   for (i = 0; i < nfavs; i++)
      if (!strcmp(favs[i], key))
      {
         free(favs[i]);
         memmove(favs + i, favs + i + 1, (nfavs - i - 1) * sizeof(char *));
         nfavs--;
         write_keys(FAVS_FILE, favs, nfavs);
         return 0;
      }
   favs = realloc(favs, (nfavs + 1) * sizeof(char *));
   favs[nfavs++] = strdup(key);
   write_keys(FAVS_FILE, favs, nfavs);
   return 1;
}

/* a launch goes to the top of Recently Played */
static void add_recent(const char *key)
{
   char **keys;
   int n = read_keys(RECENT_FILE, &keys), i, j = 0;
   char **out = malloc((n + 1) * sizeof(char *));
   if (!out)
   {
      free_keys(keys, n);
      return;
   }
   out[j++] = (char *)key;
   for (i = 0; i < n && j < RECENT_MAX; i++)
      if (strcmp(keys[i], key))
         out[j++] = keys[i];
   write_keys(RECENT_FILE, out, j);
   free(out);
   free_keys(keys, n);
}

/* take a game out of Recently Played (it is only the list that changes; the game stays on the card) */
static void remove_recent(const char *key)
{
   char **keys;
   int n = read_keys(RECENT_FILE, &keys), i, j = 0;
   for (i = 0; i < n; i++)
      if (strcmp(keys[i], key))
         keys[j++] = keys[i];
      else
         free(keys[i]);
   if (j != n)
      write_keys(RECENT_FILE, keys, j);
   free_keys(keys, j);
}

static void display_name(const entry_t *e, char *out, size_t sz)
{
   snprintf(out, sz, "%s", e->label);
}

/* ------------------------------------------------------------- state */

#define ROW_COLL   0
#define ROW_CONS   1
#define ROW_TOOLS  2
#define NROWS      3

#define TOOL_VIDEO     0      /* tools row: Video Player, Music Player, Pictures, Settings, Controls, Theme, RetroArch, Chatbot, Image Generator */
#define TOOL_MUSIC     1
#define TOOL_PICTURES  2
#define TOOL_SETTINGS  3
#define TOOL_CONTROLS  4
#define TOOL_THEME     5
#define TOOL_RETROARCH 6
#define TOOL_CHAT      7
#define TOOL_IMAGES    8

static const char *const row_titles[NROWS] = { "COLLECTIONS", "CONSOLES", "TOOLS" };
static const char *const tool_names[] = { "Video Player", "Music Player", "Pictures", "Settings", "Controls", "Theme", "RetroArch", "Chatbot", "Image Generator" };
#define NTOOLS ((int)(sizeof(tool_names) / sizeof(tool_names[0])))
#define ROW_MEDIA  NROWS      /* the Video / Music Player's folders: its own carousel, after the home rows */

/* the folders inside videos/ or music/, one card each; files lying loose at the top
 * get an "Other Videos" / "Other Music" card at the end */
static struct
{
   int video;                 /* 1 = videos, 0 = music */
   char name[MAX_SYSTEMS][256];
   int count[MAX_SYSTEMS];
   int n;                     /* folder cards */
   int loose;                 /* files at the top */
   unsigned sig;
   double check;
} mf;

static struct
{
   int view;                 /* 0 = home rows, 1 = game list */
   int row_pos;              /* focused row, unbounded (the rows loop): row = row_pos mod 3 */
   double row_anim;
   int sel_[NROWS + 1];      /* selection in each row; consoles: index into visible[] */
   int pos_[NROWS + 1];      /* the same, unbounded (carousels loop): sel = pos mod count */
   double anim_[NROWS + 1];  /* animated pos (the last is the players' folder row) */
   int list_all;             /* the open list is a collection (games from several consoles) */
   int coll;                 /* which one: index into colls[] */
   char toast[64];
   double toast_until;
   char folder[512];         /* folder inside the system, "" = top */
   list_t list;
   int sel, top;
   double list_check;
   char want_entry[256];     /* entry to reselect after a reload */
} ui;

#define sys_sel  sel_[ROW_CONS]
#define sys_anim anim_[ROW_CONS]

static int wrap(int a, int n)
{
   return n ? ((a % n) + n) % n : 0;
}

static int cur_row(void)
{
   return wrap(ui.row_pos, NROWS);
}

static int row_count(int r)
{
   return r == ROW_CONS ? (nvisible ? nvisible : 1) : r == ROW_COLL ? ncolls : r == ROW_MEDIA ? mf.n + (mf.loose > 0) : NTOOLS;
}

static system_t *cur_system(void)
{
   return nvisible ? &systems[visible[ui.sys_sel]] : NULL;
}

static int sys_keep(void)
{
   system_t *s = cur_system();
   return s ? (int)(s - systems) : -1;
}

/* set a row's selection directly (no slide) */
static void row_set(int r, int sel)
{
   ui.sel_[r] = sel;
   ui.pos_[r] = sel;
   ui.anim_[r] = sel;
}

/* move along a row; the carousel slides one step even when it wraps */
static void row_move(int r, int delta)
{
   int n = row_count(r);
   if (!n)
      return;
   ui.pos_[r] += delta;
   ui.sel_[r] = wrap(ui.pos_[r], n);
}

static void sys_restore(int keep)
{
   int i, sel = ui.sys_sel;
   if (sel >= nvisible)
      sel = 0;
   for (i = 0; i < nvisible; i++)
      if (visible[i] == keep)
         sel = i;
   if (sel != ui.sys_sel || wrap(ui.pos_[ROW_CONS], nvisible ? nvisible : 1) != sel)
      row_set(ROW_CONS, sel);
}

static void toast(const char *msg, double now)
{
   snprintf(ui.toast, sizeof(ui.toast), "%s", msg);
   ui.toast_until = now + (strlen(msg) <= 2 ? 0.6 : 1.5);
}

static void current_dir(char *out, size_t sz)
{
   system_t *s = cur_system();
   if (ui.folder[0])
      snprintf(out, sz, "%s/%s/%s", ROMS, s->id, ui.folder);
   else
      snprintf(out, sz, "%s/%s", ROMS, s->id);
}

/* ---- the games carousel (Theme card: Games Carousel). A game list drawn like the home screen: each folder is a row (the
 * console's loose games are the first), each game a square card with its title under the chosen one. The list in ui.list then
 * holds every game of the console, folder after folder, with the folder in the name ("Ape Escape/Ape Escape.cue"); a row is one
 * run of it. */
static int theme_games_carousel = 1;   /* on unless theme.cfg says "games_carousel = off" */

typedef struct
{
   char title[160];
   int first, n;
} grow_t;

static struct
{
   int on;                 /* the open list is drawn as a carousel */
   grow_t *row;
   int nrow, cap;
   int *pos;               /* per row: the chosen card, unbounded (a row loops) */
   double *anim;           /* per row: the same, as it slides */
   int row_pos;            /* the focused row, unbounded (the rows loop too) */
   double row_anim;
   int glint_sel;          /* the card the glint went to last, and when it began and ends */
   double glint_t0, glint_until;
} gc;
static int gc_moving;       /* a row or card is still sliding: the main loop draws every refresh */

/* a folder's games after the loose ones; each in name order */
static int entry_cmp_flat(const void *a, const void *b)
{
   const entry_t *x = a, *y = b;
   const char *sx = strrchr(x->name, '/'), *sy = strrchr(y->name, '/');
   int c;
   if (!sx != !sy)
      return sx ? 1 : -1;
   if (sx)
   {
      char dx[512], dy[512];
      snprintf(dx, sizeof(dx), "%.*s", (int)(sx - x->name), x->name);
      snprintf(dy, sizeof(dy), "%.*s", (int)(sy - y->name), y->name);
      c = natcmp(dx, dy);
      if (c)
         return c;
   }
   c = natcmp(x->label, y->label);
   return c ? c : natcmp(x->name, y->name);
}

/* every game of a console, folders and all, in the carousel's order (names are paths inside roms/<console>/) */
static void list_flat_library(list_t *l, const system_t *s)
{
   int i;
   list_free(l);
   if (!s)
      return;
   for (i = 0; i < s->ngames; i++)
      list_add(l, s->games[i].path, s->games[i].label, 0, (int)(s - systems));
   list_finish(l);   /* (clean names that collide keep their tags) */
   qsort(l->e, l->n, sizeof(entry_t), entry_cmp_flat);
}

/* the rows, from ui.list: a console's list splits at the folders; a collection is one row */
static void gc_build(void)
{
   int i, r = -1, n = ui.list.n;
   char prev[512] = "";

   free(gc.row);
   free(gc.pos);
   free(gc.anim);
   gc.row = NULL;
   gc.pos = NULL;
   gc.anim = NULL;
   gc.nrow = gc.cap = 0;
   gc.on = theme_games_carousel && n > 0;
   if (!gc.on)
      return;
   for (i = 0; i < n; i++)
   {
      const char *name = ui.list.e[i].name, *sl = strrchr(name, '/');
      char dir[512];
      snprintf(dir, sizeof(dir), "%.*s", !ui.list_all && sl ? (int)(sl - name) : 0, name);
      if (r < 0 || strcmp(dir, prev))
      {
         grow_t *g;
         if (gc.nrow == gc.cap)
         {
            gc.cap = gc.cap ? gc.cap * 2 : 16;
            gc.row = realloc(gc.row, gc.cap * sizeof(grow_t));
            if (!gc.row)
            {
               gc.on = 0;
               gc.nrow = gc.cap = 0;
               return;
            }
         }
         r = gc.nrow++;
         g = &gc.row[r];
         g->first = i;
         g->n = 0;
         if (ui.list_all)
            snprintf(g->title, sizeof(g->title), ui.coll == COLL_SEARCH ? "Search: %s" : "%s",
                  ui.coll == COLL_SEARCH ? search_q : colls[ui.coll < ncolls ? ui.coll : 0].name);
         else if (!dir[0])
            snprintf(g->title, sizeof(g->title), "%s", cur_system() ? cur_system()->name : "");
         else
         {
            char *o = g->title;
            const char *q;
            for (q = dir; *q && o < g->title + sizeof(g->title) - 4; q++)
               if (*q == '/')
               {
                  *o++ = ' ';
                  *o++ = '/';
                  *o++ = ' ';
               }
               else
                  *o++ = *q;
            *o = 0;
         }
         snprintf(prev, sizeof(prev), "%s", dir);
      }
      gc.row[r].n++;
   }
   gc.pos = calloc(gc.nrow, sizeof(int));
   gc.anim = calloc(gc.nrow, sizeof(double));
   if (!gc.pos || !gc.anim)
      gc.on = 0;
   gc.glint_sel = -1;
}

/* the focused row and card follow ui.sel (no slide) */
static void gc_focus(void)
{
   int r;
   if (!gc.on)
      return;
   for (r = 0; r < gc.nrow; r++)
      if (ui.sel >= gc.row[r].first && ui.sel < gc.row[r].first + gc.row[r].n)
         break;
   if (r >= gc.nrow)
      r = 0;
   gc.row_pos = r;
   gc.row_anim = r;
   gc.pos[r] = ui.sel - gc.row[r].first;
   gc.anim[r] = gc.pos[r];
}

static void reload_list(void)
{
   char keep[256] = "";
   int i;

   thumbs_clear();   /* pictures added to ready/ show up the next time a list opens */

   if (ui.want_entry[0])
      snprintf(keep, sizeof(keep), "%s", ui.want_entry);
   else if (ui.list.n && ui.sel < ui.list.n)
      snprintf(keep, sizeof(keep), "%s", ui.list.e[ui.sel].name);
   ui.want_entry[0] = 0;

   if (theme_games_carousel && !ui.list_all && ui.folder[0])
   {
      /* a folder's list from before: the carousel has them all, with the folder in each name */
      if (keep[0])
      {
         char full[512];
         snprintf(full, sizeof(full), "%s/%s", ui.folder, keep);
         snprintf(keep, sizeof(keep), "%.255s", full);
      }
      ui.folder[0] = 0;
   }
   if (ui.list_all)
      list_collection(&ui.list, ui.coll < ncolls ? ui.coll : COLL_ALL);
   else if (theme_games_carousel)
      list_flat_library(&ui.list, cur_system());
   else
      list_from_library(&ui.list, cur_system(), ui.folder);
   ui.sel = 0;
   for (i = 0; keep[0] && i < ui.list.n; i++)
      if (!strcmp(ui.list.e[i].name, keep))
         ui.sel = i;
   gc_build();
   gc_focus();
}

static const char *coll_empty_msg(int ci)
{
   return ci == COLL_ALL ? "No games yet" : ci == COLL_RECENT ? "Nothing played yet"
        : ci == COLL_FAVS ? "No favorites yet (Y adds one)" : "No games from this list on the card";
}

/* open collection ci; the home screen behind follows, so B lands on its card */
static int open_collection(int ci, double now)
{
   if (!colls[ci].count)
   {
      toast(coll_empty_msg(ci), now);
      return 0;
   }
   ui.row_pos += ROW_COLL - cur_row();
   ui.row_anim = ui.row_pos;
   row_set(ROW_COLL, ci);
   ui.view = 1;
   ui.list_all = 1;
   ui.coll = ci;
   ui.folder[0] = 0;
   ui.want_entry[0] = 0;
   list_free(&ui.list);
   ui.top = ui.sel = 0;
   reload_list();
   ui.list_check = now;
   return 1;
}

/* Select, from anywhere in the menu: straight to All Games */
static void open_all_games(double now)
{
   if (ui.view == 1 && ui.list_all && ui.coll == COLL_ALL)
      return;
   if (!total_games())
   {
      toast("No games yet", now);
      return;
   }
   /* the home screen behind it moves to All Games too, so B lands there */
   ui.row_pos += ROW_COLL - cur_row();
   ui.row_anim = ui.row_pos;
   row_set(ROW_COLL, COLL_ALL);
   row_set(ROW_CONS, 0);     /* a fresh start: the other rows back to their first item */
   row_set(ROW_TOOLS, 0);
   ui.view = 1;
   ui.list_all = 1;
   ui.coll = COLL_ALL;
   ui.folder[0] = 0;
   ui.want_entry[0] = 0;
   list_free(&ui.list);
   ui.top = ui.sel = 0;
   reload_list();
   ui.list_check = now;
}

/* a game's key ("gba/Some Game.gba"): the same in every list */
static void entry_key(const entry_t *e, char *out, size_t sz)
{
   if (ui.list_all || !ui.folder[0])
      snprintf(out, sz, "%s/%s", systems[e->sys].id, e->name);
   else
      snprintf(out, sz, "%s/%s/%s", systems[e->sys].id, ui.folder, e->name);
}

/* the letter a list entry files under: A-Z, or # for digits and symbols */
static char entry_letter(const entry_t *e)
{
   const char *p = e->label;
   while (*p && !isalnum((unsigned char)*p))
      p++;
   return isalpha((unsigned char)*p) ? (char)toupper((unsigned char)*p) : '#';
}

/* L/R in a list: to the first game of the next (or previous) letter, wrapping */
static void letter_jump(int dir, double now)
{
   int n = ui.list.n, i = ui.sel;
   char cur, msg[4];
   if (n < 2)
      return;
   cur = entry_letter(&ui.list.e[i]);
   if (dir > 0)
   {
      do
         i = (i + 1) % n;
      while (i != ui.sel && entry_letter(&ui.list.e[i]) == cur);
   }
   else
   {
      char prev;
      /* the start of the current letter... */
      while (i > 0 && entry_letter(&ui.list.e[i - 1]) == cur)
         i--;
      /* ...then back into the previous letter, and to its start */
      i = (i + n - 1) % n;
      prev = entry_letter(&ui.list.e[i]);
      while (i > 0 && entry_letter(&ui.list.e[i - 1]) == prev)
         i--;
   }
   ui.sel = i;
   snprintf(msg, sizeof(msg), "%c", entry_letter(&ui.list.e[i]));
   toast(msg, now);
}

/* the carousel's L / R: to the first game of the next (or previous) letter along the row, wrapping */
static void gc_letter(int dir, double now)
{
   grow_t *rw = &gc.row[wrap(gc.row_pos, gc.nrow)];
   int n = rw->n, lo = rw->first, start = ui.sel - lo, i = start;
   char cur, msg[4];
   if (n < 2)
      return;
   cur = entry_letter(&ui.list.e[lo + i]);
   if (dir > 0)
   {
      do
         i = (i + 1) % n;
      while (i != start && entry_letter(&ui.list.e[lo + i]) == cur);
   }
   else
   {
      char prev;
      while (i > 0 && entry_letter(&ui.list.e[lo + i - 1]) == cur)
         i--;
      i = (i + n - 1) % n;
      prev = entry_letter(&ui.list.e[lo + i]);
      while (i > 0 && entry_letter(&ui.list.e[lo + i - 1]) == prev)
         i--;
   }
   gc.pos[wrap(gc.row_pos, gc.nrow)] = i;   /* (a long way: no slide) */
   gc.anim[wrap(gc.row_pos, gc.nrow)] = i;
   ui.sel = lo + i;
   snprintf(msg, sizeof(msg), "%c", entry_letter(&ui.list.e[ui.sel]));
   toast(msg, now);
}

/* the carousel's buttons: the d-pad along the row and between the rows, L / R by letter; 1 = it was one of those */
static int gc_nav(int code, double now)
{
   int r, d;
   if (!gc.on || !gc.nrow)
      return 0;
   r = wrap(gc.row_pos, gc.nrow);
   if (code == KEY_LEFT || code == KEY_RIGHT)
   {
      d = code == KEY_LEFT ? -1 : 1;
      gc.pos[r] += d;
      ui.sel = gc.row[r].first + wrap(gc.pos[r], gc.row[r].n);
   }
   else if (code == KEY_UP || code == KEY_DOWN)
   {
      if (gc.nrow > 1)
      {
         gc.row_pos += code == KEY_UP ? -1 : 1;
         r = wrap(gc.row_pos, gc.nrow);
         ui.sel = gc.row[r].first + wrap(gc.pos[r], gc.row[r].n);
      }
   }
   else if (code == BTN_TL || code == BTN_TR)
      gc_letter(code == BTN_TL ? -1 : 1, now);
   else
      return 0;
   return 1;
}

/* every frame: the rows and cards ease towards where they should be (like the home screen's); 1 while any is still moving */
static int gc_ease(void)
{
   int r, moving = 0;
   if (!gc.on)
      return 0;
   gc.row_anim += (gc.row_pos - gc.row_anim) * 0.3;
   if (fabs(gc.row_pos - gc.row_anim) < 0.01)
      gc.row_anim = gc.row_pos;
   else
      moving = 1;
   for (r = 0; r < gc.nrow; r++)
      if (gc.anim[r] != gc.pos[r])
      {
         gc.anim[r] += (gc.pos[r] - gc.anim[r]) * 0.3;
         if (fabs(gc.pos[r] - gc.anim[r]) < 0.01)
            gc.anim[r] = gc.pos[r];
         else
            moving = 1;
      }
   return moving;
}

static void save_state(void)
{
   FILE *f = fopen(STATEFILE, "w");
   system_t *s = cur_system();
   if (!f)
      return;
   fprintf(f, "view=%d\nrow=%d\ncollection=%d\ntool=%d\nopen_collection=%s\nsystem=%s\nfolder=%s\nentry=%s\n",
         ui.view, cur_row(), ui.sel_[ROW_COLL], ui.sel_[ROW_TOOLS],
         ui.list_all ? colls[ui.coll < ncolls ? ui.coll : 0].name : "",
         s ? s->id : "", ui.folder, ui.list.n ? ui.list.e[ui.sel].name : "");
   fclose(f);
}

static void load_state(void)
{
   FILE *f = fopen(STATEFILE, "r");
   char line[1024], sys[64] = "";
   int i;

   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *v = strchr(line, '=');
      if (!v)
         continue;
      *v++ = 0;
      v[strcspn(v, "\n")] = 0;
      if (!strcmp(line, "view"))
         ui.view = atoi(v);
      else if (!strcmp(line, "row"))
         ui.row_pos = wrap(atoi(v), NROWS);
      else if (!strcmp(line, "collection"))
         ui.sel_[ROW_COLL] = wrap(atoi(v), ncolls);
      else if (!strcmp(line, "tool"))
         ui.sel_[ROW_TOOLS] = wrap(atoi(v), NTOOLS);
      else if (!strcmp(line, "open_collection") && *v)
      {
         int k;
         for (k = 0; k < ncolls; k++)
            if (!strcmp(colls[k].name, v))
            {
               ui.list_all = 1;
               ui.coll = k;
            }
      }
      else if (!strcmp(line, "system"))
         snprintf(sys, sizeof(sys), "%s", v);
      else if (!strcmp(line, "folder"))
         snprintf(ui.folder, sizeof(ui.folder), "%s", v);
      else if (!strcmp(line, "entry"))
         snprintf(ui.want_entry, sizeof(ui.want_entry), "%s", v);
   }
   fclose(f);
   for (i = 0; i < nvisible; i++)
      if (!strcmp(systems[visible[i]].id, sys))
         ui.sys_sel = i;
   if (ui.list_all)
      ui.folder[0] = 0;
   else if (!sys[0] || !nvisible || strcmp(cur_system()->id, sys))
   {
      ui.view = 0;
      ui.folder[0] = 0;
   }
   if (ui.list_all && ui.coll == COLL_SEARCH)
   {
      ui.view = 0;   /* a search isn't kept */
      ui.list_all = 0;
   }
   if (ui.view >= 2)
      ui.view = 0;   /* settings and the media player start fresh */
   if (ui.view == 0)
      ui.list_all = 0;
}

/* ------------------------------------------------------------- launching */

static void pick_core(const system_t *s, const char *rom, char *out, size_t sz)
{
   FILE *f = fopen(GAMECORES, "r");
   char line[1200], key[1100];

   snprintf(out, sz, "%s", s->ncores ? s->cores[0] : "");
   if (!f)
      return;
   snprintf(key, sizeof(key), "%s = ", rom);
   while (fgets(line, sizeof(line), f))
      if (!strncmp(line, key, strlen(key)))
      {
         snprintf(out, sz, "%s", trim(line + strlen(key)));
      }
   fclose(f);
}

static int launch_core(const system_t *s, const char *rom, const char *core);
static void bezel_write_rom(const char *rom);
static int game_find(const char *key, int *sys, int *gi);
static char err_title[160], err_line1[160], err_line2[160];
static int core_installed(const char *core);
static const char *core_title(const char *core);

static int launch(const system_t *s, const char *rom)
{
   char core[64];
   pick_core(s, rom, core, sizeof(core));
   return launch_core(s, rom, core);
}

/* ---- BIOS check, from g36/bios.cfg: "<console> | a / b , c" = (a or b) and c */
#define BIOS_CFG  SDCARD "/g36/bios.cfg"
#define BIOS_DIR  SDCARD "/bios"

static struct
{
   int sys;
   char rom[1300], core[64];
   int sleep;   /* it is waiting on "load the sleep state, or start normally", not on "Play anyway" */
   int psx;     /* it is the PlayStation sticks note: "Play" or "Play, don't show this again" */
} pending;   /* the launch waiting on "Play anyway" */
static int prompt_toggle;   /* the switch on a prompt that has one ("Don't show again") is on */

/* ---- the sleep marker: g36-sleep writes it (on the card) when it puts a game to sleep, and g36-logs removes
 * it once the game that was started from it has ended cleanly. Still there when that game is launched from
 * here = the wake-up start crashed, hung or never happened: offer to load the state. */
#define SLEEP_MARKER SDCARD "/g36/sleep-state.cfg"
#define SLEEP_WAKE   "/tmp/sleep-wake"   /* retroarch_r starts the next game with its state loaded */
typedef struct { char rom[1300], core[300], state[1300]; } sleepmark_t;

static int sleep_marker_read(sleepmark_t *m)
{
   FILE *f = fopen(SLEEP_MARKER, "r");
   char line[1700];
   memset(m, 0, sizeof(*m));
   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
   {
      char *eq = strstr(line, " = \""), *end;
      if (!eq)
         continue;
      *eq = 0;
      eq += 4;
      if (!(end = strrchr(eq, '"')))
         continue;
      *end = 0;
      if (!strcmp(line, "rom_path"))
         snprintf(m->rom, sizeof(m->rom), "%s", eq);
      else if (!strcmp(line, "core_path"))
         snprintf(m->core, sizeof(m->core), "%s", eq);
      else if (!strcmp(line, "state_path"))
         snprintf(m->state, sizeof(m->state), "%s", eq);
   }
   fclose(f);
   return m->rom[0] && m->core[0] && m->state[0];
}

/* the marker, if it is for this game and its state and emulator are still there */
static int sleep_marker_for(const char *rom, sleepmark_t *m)
{
   return sleep_marker_read(m) && !strcmp(m->rom, rom) && access(m->state, F_OK) == 0 && access(m->core, F_OK) == 0;
}

/* start the game with its sleep state loaded, the way g36-sleep does on waking; 0 = launching */
static int sleep_load(const sleepmark_t *m)
{
   FILE *f;
   bezel_write_rom(m->rom);
   f = fopen(GAMECFG, "w");
   if (!f)
      return -1;
   fprintf(f, "core_path = \"%s\"\nrom_path = \"%s\"\ng36_menu = \"1\"\n", m->core, m->rom);
   fclose(f);
   if ((f = fopen(SLEEP_WAKE, "w")))
      fclose(f);
   if ((f = fopen(SLEEP_MARKER, "w")))   /* a start from it again: ends cleanly, or the offer stays */
   {
      fprintf(f, "rom_path = \"%s\"\ncore_path = \"%s\"\nstate_path = \"%s\"\nstatus = resumed\n", m->rom, m->core, m->state);
      fclose(f);
   }
   mlog("launch: loading the sleep state: %s  %s", m->core, m->rom);
   if (!strncmp(m->rom, ROMS "/", sizeof(ROMS)))
      add_recent(m->rom + sizeof(ROMS));
   save_state();
   return 0;
}

/* is name in bios/ (or, by mistake, in a folder inside it)? */
static int bios_present(const char *name, char *misplaced, size_t msz)
{
   char path[600];
   DIR *d;
   struct dirent *e;
   snprintf(path, sizeof(path), "%s/%s", BIOS_DIR, name);
   if (access(path, F_OK) == 0)
      return 1;
   if (strchr(name, '/') || !(d = opendir(BIOS_DIR)))
      return 0;
   while ((e = readdir(d)))
   {
      if (e->d_name[0] == '.')
         continue;
      snprintf(path, sizeof(path), "%s/%s/%s", BIOS_DIR, e->d_name, name);
      if (access(path, F_OK) == 0)
      {
         snprintf(misplaced, msz, "%s", e->d_name);
         break;
      }
   }
   closedir(d);
   return 0;
}

/* 1 = everything there; 0 = something missing (err_* says what) */
static int bios_check(const system_t *s)
{
   FILE *f = fopen(BIOS_CFG, "r");
   char line[1024];
   int ok = 1;

   if (!f)
      return 1;
   while (ok && fgets(line, sizeof(line), f))
   {
      char *p = trim(line), *bar, *entry, *save1;
      if (!*p || *p == '#' || !(bar = strchr(p, '|')))
         continue;
      *bar = 0;
      if (strcmp(trim(p), s->id))
         continue;
      for (entry = strtok_r(bar + 1, ",", &save1); entry && ok; entry = strtok_r(NULL, ",", &save1))
      {
         char alts[512], *alt, *save2, names[300] = "", misplaced[64] = "";
         int found = 0, n = 0;
         snprintf(alts, sizeof(alts), "%s", entry);
         for (alt = strtok_r(alts, "/", &save2); alt; alt = strtok_r(NULL, "/", &save2))
         {
            /* "neocd/x.rom" splits on the slash too: glue folder and file back */
            char name[200];
            alt = trim(alt);
            if (!*alt)
               continue;
            snprintf(name, sizeof(name), "%s", alt);
            if (!strchr(alt, '.'))
            {
               char *next = strtok_r(NULL, "/", &save2);
               if (!next)
                  break;
               snprintf(name, sizeof(name), "%s/%s", alt, trim(next));
            }
            if (bios_present(name, misplaced, sizeof(misplaced)))
               found = 1;
            if (n++ < 3)
            {
               size_t len = strlen(names);
               snprintf(names + len, sizeof(names) - len, "%s%s", n > 1 ? " or " : "", name);
            }
         }
         if (!found)
         {
            ok = 0;
            snprintf(err_title, sizeof(err_title), "Missing BIOS for %s", s->name);
            snprintf(err_line1, sizeof(err_line1), "It needs %s%s in bios/.", names, n > 3 ? " (or another)" : "");
            if (misplaced[0])
               snprintf(err_line2, sizeof(err_line2), "It's in bios/%s/: move it up into bios/.", misplaced);
            else
               snprintf(err_line2, sizeof(err_line2), "The game probably won't start without it.");
         }
      }
   }
   fclose(f);
   return ok;
}

/* ---- zipped games are unpacked into /tmp (RAM) by RetroArch; arcade emulators read
 * zips directly. The sizes come from the zip's central directory, at its end. */
#define ZIP_MARGIN (16LL << 20)   /* room left for everything else in /tmp */

static long long zip_unpacked_size(const char *path)
{
   FILE *f = fopen(path, "rb");
   unsigned char buf[65557 + 22];
   long size, start, i, n;
   long long total = 0;

   if (!f)
      return -1;
   fseek(f, 0, SEEK_END);
   size = ftell(f);
   start = size > (long)sizeof(buf) ? size - (long)sizeof(buf) : 0;
   fseek(f, start, SEEK_SET);
   n = (long)fread(buf, 1, sizeof(buf), f);
   /* end-of-central-directory record: PK\5\6 */
   for (i = n - 22; i >= 0; i--)
      if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 5 && buf[i + 3] == 6)
         break;
   if (i >= 0)
   {
      unsigned entries = buf[i + 10] | buf[i + 11] << 8;
      unsigned long cd_off = buf[i + 16] | buf[i + 17] << 8 | buf[i + 18] << 16 | (unsigned long)buf[i + 19] << 24;
      unsigned k;
      fseek(f, (long)cd_off, SEEK_SET);
      for (k = 0; k < entries; k++)
      {
         unsigned char h[46];
         unsigned long usize;
         unsigned nl, el, cl;
         if (fread(h, 1, 46, f) != 46 || h[0] != 'P' || h[1] != 'K' || h[2] != 1 || h[3] != 2)
            break;
         usize = h[24] | h[25] << 8 | h[26] << 16 | (unsigned long)h[27] << 24;
         nl = h[28] | h[29] << 8;
         el = h[30] | h[31] << 8;
         cl = h[32] | h[33] << 8;
         total += usize == 0xffffffffUL ? (4LL << 30) : (long long)usize;   /* ZIP64: huge */
         fseek(f, nl + el + cl, SEEK_CUR);
      }
   }
   fclose(f);
   return i >= 0 ? total : -1;
}

/* 1 = fits (or doesn't need unpacking); 0 = too big (err_* says so) */
static int zip_fits(const system_t *s, const char *rom)
{
   const char *dot = strrchr(rom, '.');
   struct statvfs v;
   long long need, room;
   if (s->arcade || !dot || strcasecmp(dot, ".zip") || statvfs("/tmp", &v) < 0)
      return 1;
   need = zip_unpacked_size(rom);
   room = (long long)v.f_bavail * v.f_frsize - ZIP_MARGIN;
   if (need < 0 || need <= room)
      return 1;
   snprintf(err_title, sizeof(err_title), "Too big to unzip here");
   snprintf(err_line1, sizeof(err_line1), "Unzipped it's %lld MB; there's room for about %lld MB.",
         need >> 20, room > 0 ? room >> 20 : 0);
   snprintf(err_line2, sizeof(err_line2), "Unzip it on a computer (CD games: use .chd).");
   return 0;
}

/* returns 0 = launching, -2 = can't (err_* says why), -3 = a warning (ask first) */
static int launch_core_checked(const system_t *s, const char *rom, const char *core, int force);
static int setting_int(const char *key, int def);

static int launch_core(const system_t *s, const char *rom, const char *core)
{
   return launch_core_checked(s, rom, core, 0);
}

static int launch_core_checked(const system_t *s, const char *rom, const char *core, int force)
{
   FILE *f;
   if (!core_installed(core))
   {
      const char *slash = strrchr(rom, '/'), *name = slash ? slash + 1 : rom;
      int sys, gi;
      if (!strncmp(rom, ROMS "/", sizeof(ROMS)) && game_find(rom + sizeof(ROMS), &sys, &gi))
         name = systems[sys].games[gi].label;
      snprintf(err_title, sizeof(err_title), "Couldn't start %s", name);
      snprintf(err_line1, sizeof(err_line1), "The emulator %s isn't on the card.", core_title(core));
      snprintf(err_line2, sizeof(err_line2), "It goes in cores/ on G36-EMU, as %s_libretro.so", core);
      mlog("launch failed: %s: no emulator %s", rom, core);
      return -2;
   }
   if (!force)
   {
      sleepmark_t m;
      if (sleep_marker_for(rom, &m))
      {
         pending.sys = (int)(s - systems);
         snprintf(pending.rom, sizeof(pending.rom), "%s", rom);
         snprintf(pending.core, sizeof(pending.core), "%s", core);
         pending.sleep = 1;
         pending.psx = 0;
         snprintf(err_title, sizeof(err_title), "Load the sleep save state?");
         snprintf(err_line1, sizeof(err_line1), "This game's last sleep didn't finish: it may have crashed.");
         snprintf(err_line2, sizeof(err_line2), "Load the state saved when you slept, or start normally?");
         mlog("launch: %s has a sleep state from a wake-up that didn't finish: asking", rom);
         return -3;
      }
   }
   if (!force && (!bios_check(s) || !zip_fits(s, rom)))
   {
      pending.sleep = 0;
      pending.psx = 0;
      pending.sys = (int)(s - systems);
      snprintf(pending.rom, sizeof(pending.rom), "%s", rom);
      snprintf(pending.core, sizeof(pending.core), "%s", core);
      mlog("launch warning for %s: %s %s", rom, err_title, err_line1);
      return -3;
   }
   if (!force && !strcmp(s->id, "psx") && !setting_int("psx_sticks_warned", 0))
   {
      /* the sticks are wired to the D-pad's and the face buttons' inputs: say so before a PlayStation game starts */
      pending.sleep = 0;
      pending.psx = 1;
      prompt_toggle = 0;
      pending.sys = (int)(s - systems);
      snprintf(pending.rom, sizeof(pending.rom), "%s", rom);
      snprintf(pending.core, sizeof(pending.core), "%s", core);
      snprintf(err_title, sizeof(err_title), "The sticks are a lie");
      snprintf(err_line1, sizeof(err_line1), "They send the same inputs as the D-pad and the A, B, X, Y buttons,");
      snprintf(err_line2, sizeof(err_line2), "so games that need two analog sticks can't be played. We know of no way around it.");
      mlog("launch: PlayStation sticks note for %s", rom);
      return -3;
   }
   mlog("launch: %s  %s", core, rom);
   if (!strncmp(rom, ROMS "/", sizeof(ROMS)))
      add_recent(rom + sizeof(ROMS));
   bezel_write_rom(rom);
   if (!(f = fopen(GAMECFG, "w")))
   {
      mlog("launch failed: can't write " GAMECFG " (%s)", strerror(errno));
      return -1;
   }
   fprintf(f, "core_path = \"%s/%s_libretro.so\"\nrom_path = \"%s\"\ng36_menu = \"1\"\n", CORE_DIR, core, rom);
   fclose(f);
   save_state();
   return 0;
}

/* ------------------------------------------------------------- emulator picker */

static const char *core_title(const char *core)
{
   static const char *const names[][2] = {
      { "a5200", "a5200" }, { "atari800", "Atari800" }, { "fba", "FB Alpha" },
      { "fbalpha2012", "FB Alpha 2012" }, { "fbneo", "FinalBurn Neo" }, { "fceumm", "FCEUmm" },
      { "fceunext", "FCEUNext" }, { "gambatte", "Gambatte" }, { "gearsystem", "Gearsystem" },
      { "genesis_plus_gx", "Genesis Plus GX" }, { "gpsp", "gpSP" }, { "handy", "Handy" },
      { "mame2003", "MAME 2003" }, { "mame2003_plus", "MAME 2003-Plus" }, { "mame2010", "MAME 2010" },
      { "mame2016", "MAME 2016" }, { "mednafen_ngp", "Beetle NeoPop" },
      { "mednafen_pce_fast", "Beetle PCE Fast" }, { "mednafen_pce", "Beetle PCE" }, { "mgba", "mGBA" },
      { "nekop2", "Neko Project II" }, { "neocd", "NeoCD" }, { "pcsx_rearmed", "PCSX ReARMed" },
      { "prosystem", "ProSystem" }, { "snes9x2010", "Snes9x 2010" }, { "snes9x", "Snes9x" },
      { "stella2014", "Stella 2014" }, { "stella", "Stella" },
   };
   size_t i;
   for (i = 0; i < sizeof(names) / sizeof(*names); i++)
      if (!strcmp(names[i][0], core))
         return names[i][1];
   return core;
}

static int core_installed(const char *core)
{
   char path[200];
   snprintf(path, sizeof(path), "%s/%s_libretro.so", CORE_DIR, core);
   return access(path, F_OK) == 0;
}

/* the per-game choice for rom, or "" */
static void game_core(const char *rom, char *out, size_t sz)
{
   FILE *f = fopen(GAMECORES, "r");
   char line[1200], key[1100];
   out[0] = 0;
   if (!f)
      return;
   snprintf(key, sizeof(key), "%s = ", rom);
   while (fgets(line, sizeof(line), f))
      if (!strncmp(line, key, strlen(key)))
         snprintf(out, sz, "%s", trim(line + strlen(key)));
   fclose(f);
}

/* set (core) or clear (NULL) the per-game choice for rom */
static void set_game_core(const char *rom, const char *core)
{
   char **keep = NULL, line[1200], key[1100], tmp[300];
   int n = 0, cap = 0, i;
   FILE *f = fopen(GAMECORES, "r");

   snprintf(key, sizeof(key), "%s = ", rom);
   if (f)
   {
      while (fgets(line, sizeof(line), f))
      {
         if (!strncmp(line, key, strlen(key)))
            continue;
         if (n == cap)
         {
            cap = cap ? cap * 2 : 16;
            keep = realloc(keep, cap * sizeof(char *));
         }
         keep[n++] = strdup(line);
      }
      fclose(f);
   }
   snprintf(tmp, sizeof(tmp), "%s.tmp", GAMECORES);
   if ((f = fopen(tmp, "w")))
   {
      for (i = 0; i < n; i++)
         fputs(keep[i], f);
      if (core)
         fprintf(f, "%s = %s\n", rom, core);
      if (commit_close(f) == 0)
         rename(tmp, GAMECORES);
      else
         unlink(tmp);
   }
   for (i = 0; i < n; i++)
      free(keep[i]);
   free(keep);
}

/* make core the console's default: first in its line of systems.cfg */
static void set_console_core(system_t *s, const char *core)
{
   char **lines = NULL, line[512], tmp[300];
   int n = 0, cap = 0, i, j, k;
   FILE *f = fopen(SYSCFG, "r");

   /* in memory first */
   for (k = 0; k < s->ncores && strcmp(s->cores[k], core); k++)
      ;
   if (k == s->ncores)
      return;
   for (j = k; j > 0; j--)
      memcpy(s->cores[j], s->cores[j - 1], sizeof(s->cores[0]));
   snprintf(s->cores[0], sizeof(s->cores[0]), "%s", core);

   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *p = line, *id;
      int bars = 0;
      while (*p == ' ' || *p == '\t')
         p++;
      id = p;
      if (*id != '#' && !strncmp(id, s->id, strlen(s->id)) &&
            (id[strlen(s->id)] == ' ' || id[strlen(s->id)] == '\t' || id[strlen(s->id)] == '|'))
      {
         /* keep id | name | extensions as written, rewrite the emulators */
         char out[512];
         for (p = line; *p && bars < 3; p++)
            if (*p == '|')
               bars++;
         if (bars == 3)
         {
            size_t len;
            snprintf(out, sizeof(out), "%.*s ", (int)(p - line), line);
            for (j = 0; j < s->ncores; j++)
            {
               len = strlen(out);
               snprintf(out + len, sizeof(out) - len, "%s%s", j ? " " : "", s->cores[j]);
            }
            len = strlen(out);
            snprintf(out + len, sizeof(out) - len, "\n");
            snprintf(line, sizeof(line), "%s", out);
         }
      }
      if (n == cap)
      {
         cap = cap ? cap * 2 : 64;
         lines = realloc(lines, cap * sizeof(char *));
      }
      lines[n++] = strdup(line);
   }
   fclose(f);
   snprintf(tmp, sizeof(tmp), "%s.tmp", SYSCFG);
   if ((f = fopen(tmp, "w")))
   {
      for (i = 0; i < n; i++)
         fputs(lines[i], f);
      if (commit_close(f) == 0)
         rename(tmp, SYSCFG);
      else
         unlink(tmp);
   }
   for (i = 0; i < n; i++)
      free(lines[i]);
   free(lines);
}

static struct
{
   int sys;                    /* systems[] index */
   char rom[1300];             /* full path */
   char label[256];            /* the game's name, for the title */
   char cores[MAX_CORES][40];  /* the console's emulators that are on the card */
   int n, sel;
   char game[40];              /* this game's own choice, "" = none */
} pick;

/* RetroArch on its own (its config doesn't save on exit, so the G36 settings stay) */
static int launch_retroarch(void)
{
   unlink("/tmp/g36-bezel.raw");
   FILE *f;
   mlog("launch: RetroArch without a game");
   if (!(f = fopen(GAMECFG, "w")))
      return -1;
   fprintf(f, "core_path = \"\"\nrom_path = \"\"\ng36_menu = \"1\"\n");
   fclose(f);
   save_state();
   return 0;
}

/* ------------------------------------------------------------- USB */

static int read_file(const char *path, char *out, size_t sz)
{
   FILE *f = fopen(path, "r");
   size_t n;
   if (!f)
      return -1;
   n = fread(out, 1, sz - 1, f);
   out[n] = 0;
   fclose(f);
   return 0;
}

static int file_exists(const char *path)
{
   return access(path, F_OK) == 0;
}

static void touch(const char *path)
{
   FILE *f = fopen(path, "w");
   if (f)
      fclose(f);
}

/* a USB cable is plugged in (computer or charger) */
static int cable_in(void)
{
   char buf[256];
   if (read_file(EXTCON, buf, sizeof(buf)) < 0)
      return 0;
   return !strncmp(buf, "USB=1", 5) || strstr(buf, "\nUSB=1");
}

/* outside power of any kind: computer, charger (extcon type =1, except host mode) */
static int external_power(void)
{
   char buf[256], *line;
   if (read_file(EXTCON, buf, sizeof(buf)) < 0)
      return 0;
   for (line = strtok(buf, "\n"); line; line = strtok(NULL, "\n"))
      if (strncmp(line, "USB-HOST", 8) && strncmp(line, "USB_VBUS_EN", 11) && strstr(line, "=1"))
         return 1;
   return 0;
}

/* a cable to a computer, not a charger: the port reports a standard or charging data port */
static int data_cable(void)
{
   char buf[256];
   if (read_file(EXTCON, buf, sizeof(buf)) < 0)
      return 0;
   return (strstr(buf, "SDP=1") || strstr(buf, "CDP=1")) && !strstr(buf, "DCP=1");
}

static int setting_int(const char *key, int def);

/* The games drive didn't mount (a card that can't be read): /mnt/sdcard is then just a folder
 * on the system partition, on the same device as /mnt. */
static int games_drive_missing(void)
{
   struct stat a, b;
   return stat(SDCARD, &a) == 0 && stat("/mnt", &b) == 0 && a.st_dev == b.st_dev;
}

/* the USB controller's state ("configured" once a computer has set it up), "" if there is none */
static void udc_state(char *out, size_t sz)
{
   char path[300], buf[64];
   struct dirent *d;
   DIR *dir;
   out[0] = 0;
   if (!(dir = opendir(UDC_DIR)))
      return;
   while ((d = readdir(dir)))
   {
      if (d->d_name[0] == '.')
         continue;
      snprintf(path, sizeof(path), UDC_DIR "/%s/state", d->d_name);
      if (read_file(path, buf, sizeof(buf)) == 0)
      {
         buf[strcspn(buf, "\r\n")] = 0;
         snprintf(out, sz, "%s", buf);
      }
   }
   closedir(dir);
}

/* a computer has set up the USB connection (a wall charger never does) */
static int computer_connected(void)
{
   char path[300], buf[64];
   struct dirent *d;
   DIR *dir;
   int on = 0;

   if (!cable_in())
      return 0;
   /* with USB debugging off there's nothing on USB to be "configured" until file transfer is
    * allowed, so a data cable itself is the sign of a computer */
   if (!setting_int("usb_debugging", 0))
      return data_cable();
   if (!(dir = opendir(UDC_DIR)))
      return 0;
   while ((d = readdir(dir)))
   {
      if (d->d_name[0] == '.')
         continue;
      snprintf(path, sizeof(path), UDC_DIR "/%s/state", d->d_name);
      if (read_file(path, buf, sizeof(buf)) == 0 && !strncmp(buf, "configured", 10))
         on = 1;
   }
   closedir(dir);
   return on;
}

/* switch file transfer on or off in the background (it takes a couple of seconds) */
static void filetransfer(const char *arg)
{
   pid_t pid;
   mlog("file transfer %s", arg);
   pid = fork();
   if (pid == 0)
   {
      /* the menu ignores SIGCHLD; the scripts need it back to collect their own children */
      signal(SIGCHLD, SIG_DFL);
      setsid();
      execl(FT_TOOL, FT_TOOL, arg, (char *)NULL);
      _exit(127);
   }
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
      char *p = trim(line);
      if (strncmp(p, key, n) || (p[n] != ' ' && p[n] != '\t' && p[n] != '='))
         continue;
      if ((p = strchr(p + n, '=')))
         v = atoi(p + 1);
   }
   fclose(f);
   return v;
}

static int battery_level(void)
{
   char buf[16];
   return read_file(BATTERY, buf, sizeof(buf)) == 0 ? atoi(buf) : -1;
}

/* ------------------------------------------------------------- thumbnails
 * A small square left of each game: thumbnails/ready/ (used as is) first, then
 * thumbnails/resized/ (made by g36-thumbs from the to-resize/ inbox), matched by the
 * ROM's file name, in a <console>/ subfolder or directly in the folder. */
#define THUMB_DIR   SDCARD "/thumbnails"
#define THUMB_TOOL  "/usr/bin/g36-thumbs"
#define THUMB_PX    24
#define THUMB_CACHE 256

typedef struct
{
   char *key;
   image_t *img;   /* NULL = no thumbnail for this game */
} thumb_t;

static thumb_t thumbs[THUMB_CACHE];
static int nthumbs;
static pid_t thumb_pid;
static unsigned long long thumb_inbox_done;   /* inbox fingerprint after the last run */

static void thumbs_clear(void)
{
   int i;
   large_clear();   /* the big pictures too, so new ones show */
   for (i = 0; i < nthumbs; i++)
   {
      free(thumbs[i].key);
      if (thumbs[i].img)
         image_free(thumbs[i].img);
   }
   nthumbs = 0;
}

static image_t *thumb_load(const char *sys, const char *base)
{
   static const char *const dirs[] = { "ready", "resized" };
   static const char *const exts[] = { "png", "jpg" };
   char path[1400];
   size_t d, x;
   int sub;
   for (d = 0; d < 2; d++)
      for (sub = 1; sub >= 0; sub--)
         for (x = 0; x < (d ? 1 : 2); x++)
         {
            image_t *img, *small;
            if (sub)
               snprintf(path, sizeof(path), "%s/%s/%s/%s.%s", THUMB_DIR, dirs[d], sys, base, exts[x]);
            else
               snprintf(path, sizeof(path), "%s/%s/%s.%s", THUMB_DIR, dirs[d], base, exts[x]);
            if (access(path, F_OK) || !(img = image_load(path)))
               continue;
            /* square it (centre crop) and shrink it once; the list just copies it */
            if (img->w != img->h)
            {
               int side = img->w < img->h ? img->w : img->h, ox = (img->w - side) / 2, oy = (img->h - side) / 2, j;
               image_t *sq = malloc(sizeof(image_t));
               if (sq && (sq->px = malloc((size_t)side * side * 4)))
               {
                  sq->w = sq->h = side;
                  for (j = 0; j < side; j++)
                     memcpy(sq->px + (size_t)j * side, img->px + (size_t)(oy + j) * img->w + ox, (size_t)side * 4);
                  image_free(img);
                  img = sq;
               }
               else
                  free(sq);
            }
            small = image_scaled(img, THUMB_PX, THUMB_PX);
            image_free(img);
            return small;
         }
   return NULL;
}

static const image_t *thumb_for(const entry_t *e)
{
   char key[1300], base[1024];
   const char *slash = strrchr(e->name, '/'), *dot;
   int i;
   snprintf(key, sizeof(key), "%s/%s", systems[e->sys].id, e->name);
   for (i = 0; i < nthumbs; i++)
      if (!strcmp(thumbs[i].key, key))
         return thumbs[i].img;
   if (nthumbs == THUMB_CACHE)
      thumbs_clear();
   snprintf(base, sizeof(base), "%s", slash ? slash + 1 : e->name);
   if ((dot = strrchr(base, '.')))
      base[dot - base] = 0;
   thumbs[nthumbs].key = strdup(key);
   thumbs[nthumbs].img = thumb_load(systems[e->sys].id, base);
   return thumbs[nthumbs++].img;
}

/* the inbox: start g36-thumbs when it has something new; returns 1 when a run has
 * just finished (so the list can pick up the new pictures) */
static int thumbs_check(void)
{
   char dir[300];
   unsigned long long sig = 0, h;
   int n, total = 0, finished = 0;
   DIR *d;
   struct dirent *e;

   if (thumb_pid && kill(thumb_pid, 0) != 0)
   {
      thumb_pid = 0;
      finished = 1;
   }
   if (thumb_pid)
      return 0;
   /* fingerprint of the inbox and its console subfolders */
   snprintf(dir, sizeof(dir), "%s/to-resize", THUMB_DIR);
   dir_sig(dir, &n, &h);
   if (n <= 0)
   {
      thumb_inbox_done = 0;
      return finished;
   }
   sig = h;
   if ((d = opendir(dir)))
   {
      while ((e = readdir(d)))
      {
         char sub[600];
         int sn;
         if (e->d_name[0] == '.')
            continue;
         snprintf(sub, sizeof(sub), "%s/%s", dir, e->d_name);
         dir_sig(sub, &sn, &h);
         if (sn < 0)
            total++;          /* a file */
         else
         {
            total += sn;
            sig += h * 31;
         }
      }
      closedir(d);
   }
   if (total > 0 && sig != thumb_inbox_done)
   {
      pid_t pid = fork();
      if (pid == 0)
      {
         signal(SIGCHLD, SIG_DFL);
         setsid();
         execl(THUMB_TOOL, THUMB_TOOL, (char *)NULL);
         _exit(127);
      }
      if (pid > 0)
      {
         thumb_pid = pid;
         thumb_inbox_done = sig;
         mlog("thumbnails: resizing what's in the inbox");
      }
   }
   return finished;
}

/* ------------------------------------------------------------- drawing */

static font_t *f_huge, *f_big, *f_mid, *f_small, *f_title, *f_tiny, *f_label;

/* the favorite marker: the font has no star, so it's drawn from this 13x12 shape */
static void draw_star(canvas_t *c, int x, int y, uint32_t col)
{
   static const char *const shape[] = {
      "......#......",
      ".....###.....",
      ".....###.....",
      "....#####....",
      "#############",
      ".###########.",
      "..#########..",
      "...#######...",
      "...#######...",
      "..####.####..",
      "..###...###..",
      ".##.......##.",
   };
   int i, j;
   for (j = 0; j < 12; j++)
      for (i = 0; shape[j][i]; i++)
         if (shape[j][i] == '#')
            gfx_fill_rect(c, x + i, y + j, 1, 1, col);
}

static void draw_centered(canvas_t *c, font_t *f, const char *t, int cx, int y, uint32_t col)
{
   gfx_draw_text(c, f, t, cx - font_text_width(f, t) / 2, y, col, 0);
}

/* a larger favorite star with smooth edges, 24 pixels across, centred on (cx, cy): its coverage is worked out once */
#define STAR_BIG 24
static void fill_px(canvas_t *c, int x, int y, uint32_t col, float cov);
static void draw_star_big(canvas_t *c, int cx, int cy, uint32_t col)
{
   static unsigned char mask[STAR_BIG * STAR_BIG];
   static int made;
   int i, j;
   if (!made)
   {
      float vx[10], vy[10];
      int k, sx, sy;
      for (k = 0; k < 10; k++)
      {
         float a = -3.14159265f / 2 + k * 3.14159265f / 5, rad = (STAR_BIG / 2.0f - 0.5f) * (k & 1 ? 0.42f : 1.0f);
         vx[k] = STAR_BIG / 2.0f + rad * cosf(a);
         vy[k] = STAR_BIG / 2.0f + 0.6f + rad * sinf(a);
      }
      for (j = 0; j < STAR_BIG; j++)
         for (i = 0; i < STAR_BIG; i++)
         {
            int hit = 0;
            for (sy = 0; sy < 4; sy++)
               for (sx = 0; sx < 4; sx++)
               {
                  float px = i + (sx + 0.5f) / 4, py = j + (sy + 0.5f) / 4;
                  int inside = 0, a2, b2;
                  for (a2 = 0, b2 = 9; a2 < 10; b2 = a2++)
                     if ((vy[a2] > py) != (vy[b2] > py) && px < (vx[b2] - vx[a2]) * (py - vy[a2]) / (vy[b2] - vy[a2]) + vx[a2])
                        inside = !inside;
                  hit += inside;
               }
            mask[j * STAR_BIG + i] = (unsigned char)(hit * 255 / 16);
         }
      made = 1;
   }
   for (j = 0; j < STAR_BIG; j++)
      for (i = 0; i < STAR_BIG; i++)
         if (mask[j * STAR_BIG + i])
            fill_px(c, cx - STAR_BIG / 2 + i, cy - STAR_BIG / 2 + j, col, mask[j * STAR_BIG + i] / 255.0f);
}

static int ft_on;   /* file transfer is on (shown in the hint bar) */

static void fill_round_rect(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col);
static void stroke_round_rect(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col);
static void fill_px(canvas_t *c, int x, int y, uint32_t col, float cov);
static void draw_centered(canvas_t *c, font_t *f, const char *t, int cx, int y, uint32_t col);
/* ---- button pictures for the hint bar: A B X Y round, L R like shoulder buttons,
 * Select and Start as pills, the d-pad's directions as arrows on small squares */
#define BTN_INK  0xffffffff      /* what's printed on them */
#define BTN_SHOULDER 0xff8a52b0  /* L and R */
#define BTN_SYSTEM   0xff6a4f7e  /* Select, Start, the d-pad */
static int glyph_raw;            /* draw the letters as given (the Controls screen) */
/* the buttons that can be moved on the Controls screen; the first eight are the ones a game or the menu sees as its A B X Y L R
 * Select Start, the d-pad directions can only be shortcut buttons in games */
enum { PB_A, PB_B, PB_X, PB_Y, PB_L, PB_R, PB_SELECT, PB_START, PB_MENU, PB_UP, PB_DOWN, PB_LEFT, PB_RIGHT, PB_N };
static const char *const pb_name[PB_N]  = { "a", "b", "x", "y", "l", "r", "select", "start", "menu", "up", "down", "left", "right" };
static const char *const pb_glyph[PB_N] = { "A", "B", "X", "Y", "L", "R", "Select", "Start", "Menu", "Up", "Down", "<", ">" };
static const int pb_code[PB_N] = { BTN_SOUTH, BTN_EAST, BTN_NORTH, BTN_WEST, BTN_TL, BTN_TR, BTN_SELECT, BTN_START,
   KEY_MENU, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
static const int pb_ra[PB_N] = { 4, 5, 6, 7, 8, 9, 10, 11, -1, 0, 3, 1, 2 };   /* RetroArch's numbers for them: the arrows are 0..3 (up 0, left 1, right 2, down 3), then A B X Y L R Select Start; retroarch_r has the same table */
static int menu_remapped;        /* the menu's buttons are on other ones: pictures show the ones to press */
static unsigned char menu_m[9] = { 0, 1, 2, 3, 4, 5, 6, 7, 8 };   /* the menu's A B X Y L R Select Start Menu: which button acts as each */

/* the face buttons in the classic colours: A red, B amber, X blue, Y green */
static uint32_t face_colour(char b)
{
   return b == 'A' ? 0xffd9434c : b == 'B' ? 0xffd08a16 : b == 'X' ? 0xff3a74d6 : 0xff2f9e5e;
}

static void glyph_triangle(canvas_t *c, int cx, int cy, int dir, uint32_t col)
{
   /* dir: 0 left, 1 right, 2 up, 3 down; about 9 px */
   int i;
   for (i = 0; i < 5; i++)
   {
      int len = 9 - 2 * i;
      /* the wide end first, narrowing to the point the arrow shows */
      if (dir == 0)
         gfx_fill_rect(c, cx + 2 - i, cy - len / 2, 1, len, col);
      else if (dir == 1)
         gfx_fill_rect(c, cx - 2 + i, cy - len / 2, 1, len, col);
      else if (dir == 2)
         gfx_fill_rect(c, cx - len / 2, cy + 2 - i, len, 1, col);
      else
         gfx_fill_rect(c, cx - len / 2, cy - 2 + i, len, 1, col);
   }
}

/* text centred on (cx, cy) by its ink, not its line height */
static void draw_ink_centered(canvas_t *c, font_t *f, const char *t, int cx, int cy, uint32_t col)
{
   int x0, y0, x1, y1;
   font_ink_box(f, t, &x0, &y0, &x1, &y1);
   gfx_draw_text(c, f, t, cx - (x0 + x1) / 2, cy - (y0 + y1) / 2, col, 0);
}

/* one button; returns its width (draw = 0: just measure) */
static int glyph_one(canvas_t *c, const char *t, int x, int cy, int draw)
{
   font_t *bf = f_title ? f_title : f_small, *tf = f_tiny ? f_tiny : f_small;
   int w;
   if (menu_remapped && !glyph_raw)
   {
      int k;
      for (k = 0; k < 9; k++)
         if (!strcmp(t, pb_glyph[k]))
         {
            t = pb_glyph[menu_m[k]];
            break;
         }
   }
   if (strlen(t) == 1 && strchr("ABXY", t[0]))
   {
      /* a face button */
      if (draw)
      {
         fill_round_rect(c, x, cy - 10, 20, 20, 10, face_colour(t[0]));
         draw_ink_centered(c, bf, t, x + 10, cy, BTN_INK);
      }
      return 20;
   }
   if (strlen(t) == 1 && strchr("LR", t[0]))
   {
      /* a shoulder button */
      if (draw)
      {
         fill_round_rect(c, x, cy - 9, 24, 18, 6, BTN_SHOULDER);
         draw_ink_centered(c, bf, t, x + 12, cy, BTN_INK);
      }
      return 24;
   }
   if (!strncmp(t, "Dpad", 4))
   {
      /* the d-pad: a bold white plus (22 px, arms 8 wide). "Dpad:UD", "Dpad:LR" and so on add
       * tiny black arrows in the arms that do this; even widths keep them centred */
      if (draw)
      {
         const char *d = t[4] == ':' ? t + 5 : "";
         int cx = x + 11, k;
         uint32_t ink = 0xff000000;
         fill_round_rect(c, x + 7, cy - 11, 8, 22, 2, 0xffffffff);
         fill_round_rect(c, x, cy - 4, 22, 8, 2, 0xffffffff);
         for (k = 0; k < 3; k++)
         {
            if (strchr(d, 'U'))
               gfx_fill_rect(c, cx - 1 - k, cy - 9 + k, 2 * k + 2, 1, ink);
            if (strchr(d, 'D'))
               gfx_fill_rect(c, cx - 1 - k, cy + 8 - k, 2 * k + 2, 1, ink);
            if (strchr(d, 'L'))
               gfx_fill_rect(c, x + 2 + k, cy - 1 - k, 1, 2 * k + 2, ink);
            if (strchr(d, 'R'))
               gfx_fill_rect(c, x + 19 - k, cy - 1 - k, 1, 2 * k + 2, ink);
         }
      }
      return 22;
   }
   if (!strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, "Up") || !strcmp(t, "Down"))
   {
      /* a d-pad direction */
      if (draw)
      {
         int dir = t[0] == '<' ? 0 : t[0] == '>' ? 1 : t[0] == 'U' ? 2 : 3;
         fill_round_rect(c, x, cy - 9, 18, 18, 5, BTN_SYSTEM);
         glyph_triangle(c, x + 9, cy, dir, BTN_INK);
      }
      return 18;
   }
   /* Select, Start, anything else: a pill with the word in small letters */
   w = font_text_width(tf, t) + 14;
   if (draw)
   {
      fill_round_rect(c, x, cy - 9, w, 18, 9, BTN_SYSTEM);
      draw_ink_centered(c, tf, t, x + w / 2, cy, BTN_INK);
   }
   return w;
}

/* a button or a combo ("Select + A", "L / R", "< >"); returns its width */
static int draw_glyphs(canvas_t *c, const char *key, int x, int cy, int draw)
{
   char tok[32];
   const char *p = key;
   int x0 = x, first = 1;
   while (*p)
   {
      size_t n = strcspn(p, " ");
      if (n >= sizeof(tok))
         n = sizeof(tok) - 1;
      memcpy(tok, p, n);
      tok[n] = 0;
      p += n;
      while (*p == ' ')
         p++;
      if (!tok[0])
         continue;
      if (!strcmp(tok, "+") || !strcmp(tok, "/"))
      {
         /* between buttons */
         int sw = font_text_width(f_small, tok);
         if (draw)
            gfx_draw_text(c, f_small, tok, x + 3, cy - font_height(f_small) / 2, C_DIM, 0);
         x += sw + 6;
         first = 1;
         continue;
      }
      if (!first)
         x += 3;
      x += glyph_one(c, tok, x, cy, draw);
      first = 0;
   }
   return x - x0;
}


/* The bar along the bottom: "A  Open     B  Back ..." (entries five spaces apart, the
 * button and what it does two apart). Each entry sits in a small box of its own; the
 * button is bright, what it does dimmer. Entries that don't fit are left off. */
static void draw_hint_boxes(canvas_t *c, const char *hints, int x, int by, int limit);
#define HINT_BAR_A 179   /* the hint bar's background: 70% opaque */
static int theme_glass_bar = 1;    /* Theme card: Translucent bar On/Off (Off: a solid bar) */
static int theme_glass_pill = 1;   /* Theme card: Translucent pill On/Off (the position dots' pill) */
static uint32_t mix(uint32_t dst, uint32_t src, unsigned a);
static void draw_hints_now(canvas_t *c, const char *hints);

/* The bar along the bottom changes only when its words, its colours or the "File transfer on"
 * badge do, but it's wanted on every frame: it's drawn once into a strip of its own and
 * copied (drawing its boxes and text cost several milliseconds a frame). */
static void draw_hints(canvas_t *c, const char *hints)
{
   static uint32_t *strip;
   static unsigned key;
   static int strip_w;
   unsigned k = 2166136261u;
   const char *p;
   size_t y;
   for (p = hints; *p; p++)
      k = (k ^ (unsigned char)*p) * 16777619u;
   k = (k ^ (unsigned)ft_on) * 16777619u;
   k = (k ^ TH(TH_BAR)) * 16777619u;
   k = (k ^ TH(TH_BAR_BOX)) * 16777619u;
   k = (k ^ TH(TH_BAR_LINE)) * 16777619u;
   k = (k ^ C_DIM) * 16777619u;
   k = (k ^ C_ACCENT) * 16777619u;
   k = (k ^ (unsigned)c->w) * 16777619u;
   k = (k ^ (unsigned)theme_glass_bar) * 16777619u;
   if (menu_remapped && !glyph_raw)
      for (y = 0; y < 9; y++)
         k = (k ^ menu_m[y]) * 16777619u;
   k = (k ^ (unsigned)glyph_raw) * 16777619u;
   if (c->h < 40 || c->w <= 0)
      return;
   if (!strip || strip_w != c->w)
   {
      free(strip);
      strip = malloc((size_t)c->w * 40 * 4);
      strip_w = c->w;
      key = ~k;
   }
   if (!strip)
   {
      draw_hints_now(c, hints);
      return;
   }
   if (k != key)
   {
      canvas_t sub = { c->w, 40, c->w, strip };   /* (40 high: the bar sits at its top) */
      draw_hints_now(&sub, hints);
      key = k;
   }
   /* the bar's own colour is see-through (HINT_BAR_A of 255) so what is behind it shows; the buttons and the text on them
    * are solid. The strip holds that in its alpha (made in draw_hints_now), and each frame it is blended over the bottom rows. */
   for (y = 0; y < 40; y++)
   {
      uint32_t *dst = c->px + (size_t)(c->h - 40 + y) * c->stride, *src = strip + y * c->w;
      int x;
      for (x = 0; x < c->w; x++)
      {
         unsigned a = src[x] >> 24;
         dst[x] = a == 255 ? src[x] : mix(dst[x], src[x] | 0xff000000, a);
      }
   }
}

static void draw_hints_now(canvas_t *c, const char *hints)
{
   const char *badge = "File transfer on";
   int bw = ft_on ? font_text_width(f_small, badge) + 30 : 0;
   int by = c->h - 40 + (40 - 26) / 2;
   int x, y;
   uint32_t bar = TH(TH_BAR) | 0xff000000;
   gfx_fill_rect(c, 0, c->h - 40, c->w, 40, TH(TH_BAR));
   draw_hint_boxes(c, hints, 12, by, c->w - 12 - bw);
   if (ft_on)
   {
      /* the badge is on a solid pill like the buttons, so the cards behind the see-through bar can't cut across its words */
      fill_round_rect(c, c->w - 20 - font_text_width(f_small, badge) - 10, by, font_text_width(f_small, badge) + 20, 26, 13, TH(TH_BAR_BOX));
      gfx_draw_text(c, f_small, badge, c->w - 20 - font_text_width(f_small, badge),
            by + (26 - font_height(f_small)) / 2, C_ACCENT, 0);
   }
   /* every pixel still exactly the bar's colour is bar background: make it see-through; all else stays solid */
   for (y = c->h - 40; y < c->h; y++)
      for (x = 0; x < c->w; x++)
      {
         uint32_t *px = &c->px[(size_t)y * c->stride + x];
         if ((*px | 0xff000000) == bar)
            *px = (bar & 0xffffff) | ((uint32_t)(theme_glass_bar ? HINT_BAR_A : 255) << 24);
         else
            *px |= 0xff000000;
      }
}

/* the boxed entries alone, in a row from x with their tops at by */
static void draw_hint_boxes(canvas_t *c, const char *hints, int x, int by, int limit)
{
   int fh = font_height(f_small), bh = 26, ty = by + (bh - fh) / 2;
   const char *p = hints;
   while (*p)
   {
      char entry[96], key[48];
      const char *end = strstr(p, "     "), *sep;
      size_t n = end ? (size_t)(end - p) : strlen(p);
      int kw, w;
      if (n >= sizeof(entry))
         n = sizeof(entry) - 1;
      memcpy(entry, p, n);
      entry[n] = 0;
      p = end ? end + 5 : p + n;
      while (*p == ' ')
         p++;
      if (!entry[0])
         continue;
      /* the button, and what it does */
      sep = strstr(entry, "  ");
      snprintf(key, sizeof(key), "%.*s", sep ? (int)(sep - entry) : (int)strlen(entry), entry);
      kw = draw_glyphs(c, key, 0, 0, 0);
      w = kw + (sep ? 8 + font_text_width(f_small, sep + 2) : 0) + 14;
      if (x + w > limit)
         break;
      fill_round_rect(c, x - 1, by - 1, w + 2, bh + 2, 9, TH(TH_BAR_LINE));
      fill_round_rect(c, x, by, w, bh, 8, TH(TH_BAR_BOX));
      draw_glyphs(c, key, x + 4, by + bh / 2, 1);
      if (sep)
         gfx_draw_text(c, f_small, sep + 2, x + 4 + kw + 8, ty, C_DIM, 0);
      x += w + 8;
   }
}

/* 0..1, an ordered pattern added before rounding so gentle gradients don't band */
static float dither(int x, int y)
{
   static const unsigned char bayer[4][4] = {
      { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
   return (bayer[y & 3][x & 3] + 0.5f) / 16;
}

/* a top-to-bottom gradient from colour a to colour b, dithered */
static void fill_vgradient(canvas_t *c, int x0, int y0, int w, int h, uint32_t a, uint32_t b)
{
   int x, y, k;
   for (y = 0; y < h; y++)
   {
      float t = h > 1 ? (float)y / (h - 1) : 0;
      if (y0 + y < 0 || y0 + y >= c->h)
         continue;
      for (x = 0; x < w; x++)
      {
         uint32_t out = 0xff000000;
         float d = dither(x0 + x, y0 + y);
         if (x0 + x < 0 || x0 + x >= c->w)
            continue;
         for (k = 16; k >= 0; k -= 8)
         {
            float ca = a >> k & 0xff, cb = b >> k & 0xff, v = ca + (cb - ca) * t + d;
            out |= (uint32_t)(v > 255 ? 255 : v) << k;
         }
         c->px[(size_t)(y0 + y) * c->stride + x0 + x] = out;
      }
   }
}

/* The background is made once, BG_M pixels bigger than the screen on every side, so it can be shifted (the parallax) and still
 * cover the screen. There are two: the plain one for every screen, and the "stage" for the home screen, which adds a perspective
 * floor with a horizon behind the cards. bg_dx/bg_dy are the shift in effect (content moves left/up for positive values),
 * and round_corners() reads the same buffer and shift, so the cards' corners match what is behind them. */
#define BG_M 28
static uint32_t *bg, *bg_stage;
static int theme_skyline = 1;    /* the stage background's skyline (Theme card: Skyline On/Off) */
static int theme_sky_style = 1;  /* 0 city, 1 mountains, 2 forest (Theme card: Skyline style); mountains unless theme.cfg says otherwise */
static uint32_t *ctl_bgbuf;   /* the Controls screen's backdrop (the stage background, dimmed), made once */
static uint32_t ctl_bgkey;
static const uint32_t *bg_cur;
static int bg_w, bg_dx, bg_dy, bg_stage_on;
#define BG_POOL_W    0.50f   /* the light pool behind the focused card: radii as a share of the screen's width and height */
#define BG_POOL_H    0.62f
#define BG_POOL      0.42f   /* its strength (a mix towards the light colour's core) */
#define BG_VIGNETTE  0.72f   /* how much the far corners are darkened */
#define FLOOR_HORIZON 0.47f  /* the stage's horizon, as a share of the screen's height (it's behind the focused card) */
#define FLOOR_GRID    0.80f  /* the grid lines' strength (a mix towards the accent colour) */

/* a pixel moved part of the way (a of 0..1) towards a colour */
static inline uint32_t bg_toward(uint32_t d, const float col[3], float a)
{
   float r = (d >> 16 & 255) + (col[0] - (d >> 16 & 255)) * a, g = (d >> 8 & 255) + (col[1] - (d >> 8 & 255)) * a,
         b = (d & 255) + (col[2] - (d & 255)) * a;
   return 0xff000000 | (uint32_t)(r + 0.5f) << 16 | (uint32_t)(g + 0.5f) << 8 | (uint32_t)(b + 0.5f);
}

/* value noise along a line (0..1), the same every time */
static float sky_lat(int i, unsigned seed)
{
   unsigned h = (unsigned)i * 374761393u + seed * 668265263u;
   h = (h ^ (h >> 13)) * 1274126177u;
   return ((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

static float sky_noise(float x, unsigned seed)
{
   int i = (int)floorf(x);
   float f = x - i;
   f = f * f * (3 - 2 * f);
   return sky_lat(i, seed) * (1 - f) + sky_lat(i + 1, seed) * f;
}

/* The mountains style: three ranges, one behind the other, with sharp peaks (ridged noise). The far one is faint, the near one
 * darkest; a thin rim of the accent colour runs along each ridge, and the slopes that face left are lit a little. */
static void bg_sky_mountains(uint32_t *buf, int bw, int bh, int base, uint32_t cb, uint32_t accent)
{
   const float acc[3] = { accent >> 16 & 255, accent >> 8 & 255, accent & 255 };
   /* each range is the sky behind it made darker (so it shows against the sky's glow whatever the theme) with a little accent */
   static const float amp[3] = { 140, 112, 80 }, period[3] = { 300, 215, 150 }, dark[3] = { 0.80f, 0.68f, 0.55f };
   (void)cb;
   int layer, xx, yy, o;
   int *hgt = malloc((size_t)bw * sizeof(int));
   if (!hgt)
      return;
   for (layer = 0; layer < 3; layer++)
   {
      for (xx = 0; xx < bw; xx++)
      {
         float x = (float)(xx - BG_M) / period[layer], r = 0, a = 1;
         for (o = 0; o < 4; o++, a *= 0.5f, x *= 2)
            r += a * (1 - fabsf(2 * sky_noise(x, 11u + layer * 7u + o) - 1));
         r /= 1.875f;
         hgt[xx] = (int)(amp[layer] * powf(r, 1.7f) * 1.5f);
         if (hgt[xx] < 6)
            hgt[xx] = 6;
      }
      for (xx = 0; xx < bw; xx++)
      {
         int top = base - hgt[xx];
         float slope = xx >= 4 ? (hgt[xx] - hgt[xx - 4]) / 8.0f : 0;   /* (rising to the right: a slope facing left, lit) */
         float lit = slope < 0 ? 0 : slope > 1 ? 1 : slope;
         for (yy = top; yy <= base; yy++)
         {
            uint32_t *px;
            float f = dark[layer], r, g, b, fade = yy - top < 18 ? 1.0f - (yy - top) / 18.0f : 0;
            if (yy < 0 || yy >= bh)
               continue;
            px = &buf[(size_t)yy * bw + xx];
            r = (*px >> 16 & 255) * f + acc[0] * 0.05f;
            g = (*px >> 8 & 255) * f + acc[1] * 0.05f;
            b = (*px & 255) * f + acc[2] * 0.05f;
            r += (acc[0] - r) * 0.10f * lit * fade;
            g += (acc[1] - g) * 0.10f * lit * fade;
            b += (acc[2] - b) * 0.10f * lit * fade;
            *px = 0xff000000 | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
         }
         if (top >= 0 && top < bh)
            buf[(size_t)top * bw + xx] = bg_toward(buf[(size_t)top * bw + xx], acc, 0.22f);
      }
   }
   free(hgt);
}

/* ---- shapes for the stage's skyline. A shape is a mask: row 0 is its bottom (where it stands), columns left to right; 0 empty,
 * 1 body, 2 lit edge (light from the upper left), 3 shaded, 4 highlighted, 5 a lit window. */
typedef struct { unsigned char *m; int w, h; } fmask_t;

static int fm_make(fmask_t *fm, int w, int h)
{
   fm->w = w;
   fm->h = h;
   fm->m = calloc((size_t)w * h, 1);
   return fm->m != NULL;
}

static inline void fm_put(fmask_t *fm, int r, int c, unsigned char v)
{
   if (r >= 0 && r < fm->h && c >= 0 && c < fm->w)
      fm->m[(size_t)r * fm->w + c] = v;
}

static inline unsigned char fm_get(const fmask_t *fm, int r, int c)
{
   return r >= 0 && r < fm->h && c >= 0 && c < fm->w ? fm->m[(size_t)r * fm->w + c] : 0;
}

/* a shape put on the background: the sky or floor behind it made darker (so it shows against the glow whatever the theme), with
 * its edges and windows in the accent colour; x0 is the column of its left side, contact the line it stands on */
static void front_body(uint32_t *buf, int bw, int bh, const fmask_t *fm, int x0, int contact, float dark, const float acc[3],
      const uint32_t *orig, int ref_max)
{
   const float lit[3] = { acc[0] * 0.85f + 38, acc[1] * 0.85f + 38, acc[2] * 0.85f + 38 };
   int r, c;
   for (r = 0; r < fm->h; r++)
   {
      int y = contact - r;
      if (y < 0 || y >= bh)
         continue;
      for (c = 0; c < fm->w; c++)
      {
         unsigned char v = fm->m[(size_t)r * fm->w + c];
         int xx = x0 + c;
         uint32_t *px, src;
         float f, cr, cg, cb;
         if (!v || xx < 0 || xx >= bw)
            continue;
         px = &buf[(size_t)y * bw + xx];
         if (orig)
            src = orig[(size_t)(y < ref_max ? y : ref_max) * bw + xx];   /* (solid: the colour comes from the untouched sky, the floor from the horizon) */
         else
            src = *px;
         f = v == 3 ? dark * 0.80f : dark;
         cr = (src >> 16 & 255) * f + acc[0] * 0.05f;
         cg = (src >> 8 & 255) * f + acc[1] * 0.05f;
         cb = (src & 255) * f + acc[2] * 0.05f;
         if (v == 2 || v == 4 || v == 5)
         {
            const float *to = v == 5 ? lit : acc;
            float a = v == 2 ? 0.16f : v == 4 ? 0.10f : 0.45f;
            cr += (to[0] - cr) * a;
            cg += (to[1] - cg) * a;
            cb += (to[2] - cb) * a;
         }
         *px = 0xff000000 | (uint32_t)(cr > 255 ? 255 : cr) << 16 | (uint32_t)(cg > 255 ? 255 : cg) << 8 | (uint32_t)(cb > 255 ? 255 : cb);
      }
   }
}

/* a shape that stands on the floor: the shadow it throws (to the right and flattened onto the floor, softer and fainter the
 * higher the part that throws it) and its reflection (upside down, squashed, fading), both under the line it stands on */
static void front_floor(uint32_t *buf, int bw, int bh, const fmask_t *fm, int x0, int contact, const float acc[3])
{
   const float lit[3] = { acc[0] * 0.85f + 38, acc[1] * 0.85f + 38, acc[2] * 0.85f + 38 };
   int sw = fm->w + fm->h / 2 + 8, shh = fm->h * 16 / 100 + 4, r, c, i, j;
   unsigned char *sh = calloc((size_t)sw * shh, 1), *bl = calloc((size_t)sw * shh, 1);
   if (sh && bl)
   {
      for (r = 0; r < fm->h; r++)
      {
         int dy = r * 16 / 100 + 1, dx = r / 2;
         unsigned char a = (unsigned char)(255 - r * 170 / fm->h);
         for (c = 0; c < fm->w; c++)
            if (fm->m[(size_t)r * fm->w + c] && dy < shh && sh[(size_t)dy * sw + c + dx] < a)
               sh[(size_t)dy * sw + c + dx] = a;
      }
      for (j = 0; j < shh; j++)   /* softened: a box 7 wide along each row */
         for (i = 0; i < sw; i++)
         {
            int k, sum = 0;
            for (k = -3; k <= 3; k++)
               if (i + k >= 0 && i + k < sw)
                  sum += sh[(size_t)j * sw + i + k];
            bl[(size_t)j * sw + i] = (unsigned char)(sum / 7);
         }
      for (j = 0; j < shh; j++)
      {
         int y = contact + j;
         if (y < 0 || y >= bh)
            continue;
         for (i = 0; i < sw; i++)
         {
            unsigned char a = bl[(size_t)j * sw + i];
            int xx = x0 + i;
            uint32_t *px;
            float f;
            if (!a || xx < 0 || xx >= bw)
               continue;
            px = &buf[(size_t)y * bw + xx];
            f = 1.0f - 0.50f * a / 255.0f;
            *px = 0xff000000 | (uint32_t)((*px >> 16 & 255) * f) << 16 | (uint32_t)((*px >> 8 & 255) * f) << 8 | (uint32_t)((*px & 255) * f);
         }
      }
   }
   free(sh);
   free(bl);
   for (j = 1; j <= 40; j++)   /* the reflection */
   {
      int y = contact + j, src = j * 2;
      float a = 0.32f * (1.0f - j / 41.0f);
      if (y >= bh || src >= fm->h)
         break;
      for (c = 0; c < fm->w; c++)
      {
         unsigned char v = fm->m[(size_t)src * fm->w + c];
         int xx = x0 + c;
         uint32_t *px;
         float cr, cg, cb;
         if (!v || xx < 0 || xx >= bw)
            continue;
         px = &buf[(size_t)y * bw + xx];
         cr = (*px >> 16 & 255) * (1 - a);
         cg = (*px >> 8 & 255) * (1 - a);
         cb = (*px & 255) * (1 - a);
         if (v == 5)
         {
            cr += (lit[0] - cr) * a * 0.9f;
            cg += (lit[1] - cg) * a * 0.9f;
            cb += (lit[2] - cb) * a * 0.9f;
         }
         *px = 0xff000000 | (uint32_t)cr << 16 | (uint32_t)cg << 8 | (uint32_t)cb;
      }
   }
}

/* ---- the trees: seven stylized silhouettes (Open Clipart, public domain; tools/make_treeshapes.py made treeshapes.h from them) */
#include "treeshapes.h"

/* tree shapes, in treeshapes.h's order */
enum { TS_ROUND, TS_PAGODA, TS_SPRUCE, TS_CYPRESS, TS_MAPLE, TS_WIDEOAK, TS_FIR };

/* each shape's running totals (how many lit source pixels lie above and to the left of each point), made once: the lit pixels inside any
 * box of the source are then four reads, so bringing a shape to a size costs only the pixels of the result */
static unsigned short *ts_sat[TS_N];

static const unsigned short *tree_shape_sat(int shape)
{
   int sw = ts_shape[shape].w, stride = (sw + 7) / 8, x, y;
   if (!ts_sat[shape])
   {
      unsigned short *t = calloc((size_t)(sw + 1) * (TS_H + 1), sizeof(unsigned short));
      if (!t)
         return NULL;
      for (y = 0; y < TS_H; y++)
      {
         int row = 0;
         for (x = 0; x < sw; x++)
         {
            row += ts_shape[shape].bits[(size_t)y * stride + x / 8] >> (7 - x % 8) & 1;
            t[(size_t)(y + 1) * (sw + 1) + x + 1] = (unsigned short)(t[(size_t)y * (sw + 1) + x + 1] + row);
         }
      }
      ts_sat[shape] = t;
   }
   return ts_sat[shape];
}

static int tree_shape_width(int shape, int h)
{
   int w = ts_shape[shape].w * h / TS_H;
   return w < 3 ? 3 : w;
}

/* a tree of a given shape, h pixels tall and standing with its middle at column cx, put in a mask (flipped if flip): the shape is
 * brought to size by averaging (so a small tree keeps its overall shape), the left edge of each row and the top of each column are
 * lit, and the right side of the tree is in shade */
static void tree_shape_draw(fmask_t *fm, int cx, int h, int shape, int flip)
{
   int tw = tree_shape_width(shape, h), sw = ts_shape[shape].w, tr, tc, x0 = cx - tw / 2;
   const unsigned short *sat = tree_shape_sat(shape);
   unsigned char *tmp = sat ? calloc((size_t)tw * h, 1) : NULL;
   if (!tmp)
      return;
   for (tr = 0; tr < h; tr++)
   {
      int sy0 = tr * TS_H / h, sy1 = (tr + 1) * TS_H / h;
      if (sy1 <= sy0)
         sy1 = sy0 + 1;
      for (tc = 0; tc < tw; tc++)
      {
         int sx0 = tc * sw / tw, sx1 = (tc + 1) * sw / tw, on, area;
         if (sx1 <= sx0)
            sx1 = sx0 + 1;
         area = (sy1 - sy0) * (sx1 - sx0);
         on = sat[(size_t)sy1 * (sw + 1) + sx1] - sat[(size_t)sy0 * (sw + 1) + sx1] - sat[(size_t)sy1 * (sw + 1) + sx0] +
              sat[(size_t)sy0 * (sw + 1) + sx0];
         if (on * 2 >= area)
            tmp[(size_t)tr * tw + (flip ? tw - 1 - tc : tc)] = 1;
      }
   }
   for (tr = 0; tr < h; tr++)   /* shade the right side, light the left edge of each row */
   {
      int first = -1;
      for (tc = 0; tc < tw; tc++)
         if (tmp[(size_t)tr * tw + tc])
         {
            if (first < 0)
            {
               first = tc;
               tmp[(size_t)tr * tw + tc] = 2;
            }
            else if (h > 40 && tc > tw * 62 / 100)
               tmp[(size_t)tr * tw + tc] = 3;
         }
   }
   for (tc = 0; tc < tw; tc++)   /* and the top of each column */
      for (tr = 0; tr < h; tr++)
         if (tmp[(size_t)tr * tw + tc])
         {
            tmp[(size_t)tr * tw + tc] = 2;
            break;
         }
   for (tr = 0; tr < h; tr++)
      for (tc = 0; tc < tw; tc++)
         if (tmp[(size_t)tr * tw + tc])
            fm_put(fm, h - 1 - tr, x0 + tc, tmp[(size_t)tr * tw + tc]);
   free(tmp);
}

/* The forest style: three rows of trees, small and pale far away, bigger and darker nearer, taller and shorter in clumps, with
 * clearings now and then. Each row is drawn as a shape first (so overlapping trees aren't darkened twice). The same every time.
 * (The big trees in front are the skyline's front shapes, bg_sky_front.) */
static void bg_sky_forest(uint32_t *buf, int bw, int bh, int base, uint32_t accent, const uint32_t *orig)
{
   const float acc[3] = { accent >> 16 & 255, accent >> 8 & 255, accent & 255 };
   static const float dark[3] = { 0.86f, 0.72f, 0.57f };
   static const int hmin[3] = { 30, 46, 64 }, hmax[3] = { 66, 98, 134 }, step[3] = { 6, 10, 16 };
   /* how often each shape is chosen, in per cent, in the order of TS_ROUND ... TS_FIR: far rows are mostly conifers */
   static const int weight[3][7] = { { 8, 5, 32, 20, 10, 0, 25 }, { 14, 10, 18, 8, 22, 8, 20 }, { 14, 14, 14, 4, 20, 16, 18 } };
   unsigned seed = 0x51ed270bu;
   fmask_t fm;
   int layer, xx, r;
#define F_RND() (seed = seed * 1664525u + 1013904223u, seed >> 12)
   if (!fm_make(&fm, bw, 230))
      return;
   for (layer = 0; layer < 3; layer++)
   {
      int x = -8;
      memset(fm.m, 0, (size_t)fm.w * fm.h);
      while (x < bw + 8)
      {
         int h, roll = (int)(F_RND() % 100), shape, acc_w = 0, gap, flip = (int)(F_RND() & 1);
         float clump = 0.72f + 0.56f * sky_noise((x - BG_M) / 170.0f + layer * 3, 90u + layer);
         h = (int)((hmin[layer] + (int)(F_RND() % (unsigned)(hmax[layer] - hmin[layer]))) * clump);
         if (h > 228)
            h = 228;
         for (shape = 0; shape < 6; shape++)
         {
            acc_w += weight[layer][shape];
            if (roll < acc_w)
               break;
         }
         tree_shape_draw(&fm, x, h, shape, flip);
         gap = tree_shape_width(shape, h) * 35 / 100 + step[layer] / 2 + (int)(F_RND() % (unsigned)step[layer]);
         if (layer > 0 && F_RND() % 100 < 12)
            gap += 40 + (int)(F_RND() % 60);   /* a clearing */
         x += gap;
      }
      if (layer == 2)
         for (xx = 0; xx < bw; xx++)   /* the third row stands on a low rolling ground */
         {
            int hh = 7 + (int)(4 * sinf((xx - BG_M) * 0.021f + 0.7f) + 2 * sinf((xx - BG_M) * 0.063f));
            for (r = 0; r < hh; r++)
               fm_put(&fm, r, xx, 1);
         }
      front_body(buf, bw, bh, &fm, 0, base, dark[layer], acc, orig, base);
   }
#undef F_RND
   free(fm.m);
}

/* The shapes that stand close, in front of everything and down on the floor beyond the horizon line: big trees (the forest),
 * a few tall buildings (the city) or rounded hills (the mountains). Each stands on its own line a little below the horizon,
 * is darker than anything behind it, and throws a shadow and a reflection on the floor. The same every time. */
static void bg_sky_front(uint32_t *buf, int bw, int bh, int base, uint32_t accent, int style, const uint32_t *orig)
{
   const float acc[3] = { accent >> 16 & 255, accent >> 8 & 255, accent & 255 };
   /* where each stands: across the screen (in 1000ths), how wide, how tall, and how far below the horizon line */
   static const struct { int at, w, h, below; } city[] = { { 20, 62, 104, 16 }, { 150, 40, 68, 8 }, { 800, 50, 84, 12 }, { 955, 68, 120, 20 } };
   static const struct { int at, w, h, below; } hills[] = { { 40, 340, 74, 26 }, { 400, 380, 50, 14 }, { 640, 300, 62, 20 }, { 930, 330, 92, 34 } };
   static const struct { int at, shape, h, below; } trees[] = { { 30, TS_SPRUCE, 150, 22 }, { 210, TS_ROUND, 112, 12 }, { 520, TS_FIR, 128, 16 }, { 790, TS_MAPLE, 140, 20 }, { 965, TS_WIDEOAK, 150, 26 } };
   int canvas_w = bw - 2 * BG_M, n = style == 2 ? 5 : 4, i;
   unsigned seed = 0x7e57ab1eu;
   for (i = 0; i < n; i++)
   {
      fmask_t fm;
      int at = style == 2 ? trees[i].at : style == 1 ? hills[i].at : city[i].at, cx, x0, w, below;
      if (style == 2)
      {
         w = tree_shape_width(trees[i].shape, trees[i].h) + 2;
         below = trees[i].below;
         if (!fm_make(&fm, w, trees[i].h + 4))
            return;
         tree_shape_draw(&fm, w / 2, trees[i].h, trees[i].shape, i & 1);
      }
      else if (style == 1)
      {
         int c, r, h = hills[i].h;
         w = hills[i].w;
         below = hills[i].below;
         if (!fm_make(&fm, w, h + 4))
            return;
         for (c = 0; c < w; c++)   /* a rounded hill with a bumpy top */
         {
            float u = (float)c / (w - 1);
            int top = (int)(h * powf(sinf(u * 3.14159265f), 0.7f) * (0.88f + 0.12f * sky_noise(c / 14.0f + i * 5, 40u + i)));
            int prev = c > 0 ? (int)(h * powf(sinf((u - 1.0f / w) * 3.14159265f), 0.7f)) - 1 : -1;
            for (r = 0; r < top; r++)
               fm_put(&fm, r, c, r == top - 1 || r == prev ? 2 : r < top / 3 ? 3 : 1);
         }
      }
      else
      {
         int c, r, h = city[i].h, wx, wy;
         w = city[i].w;
         below = city[i].below;
         if (!fm_make(&fm, w, h + 24))
            return;
         for (r = 0; r < h; r++)
            for (c = 0; c < w; c++)
               fm_put(&fm, r, c, r == h - 1 || c == 0 ? 2 : c > w * 3 / 4 ? 3 : 1);
         if (w > 60)   /* a stepped top, an aerial */
         {
            for (r = h; r < h + 10; r++)
               for (c = w / 4; c < w * 3 / 4; c++)
                  fm_put(&fm, r, c, r == h + 9 || c == w / 4 ? 2 : 1);
            for (r = h + 10; r < h + 24; r++)
               fm_put(&fm, r, w / 2, 1);
         }
         for (wy = 8; wy < h - 6; wy += 8)   /* the windows, a few lit */
            for (wx = 5; wx < w - 6; wx += 7)
               if ((seed = seed * 1664525u + 1013904223u, seed >> 12) % 6 == 0)
               {
                  fm_put(&fm, wy, wx, 5);
                  fm_put(&fm, wy, wx + 1, 5);
                  fm_put(&fm, wy + 1, wx, 5);
                  fm_put(&fm, wy + 1, wx + 1, 5);
                  fm_put(&fm, wy + 2, wx, 5);
                  fm_put(&fm, wy + 2, wx + 1, 5);
               }
      }
      cx = BG_M + canvas_w * at / 1000;
      x0 = cx - w / 2;
      front_floor(buf, bw, bh, &fm, x0, base + below, acc);
      front_body(buf, bw, bh, &fm, x0, base + below, 0.46f, acc, orig, base - 34);
      free(fm.m);
   }
}

/* The stage's skyline: along the horizon, so the sky isn't empty: a faint range of mountains behind, and in front of them a
 * city of dark buildings (flat roofs, some stepped, some with an aerial) with a few small lit windows and a thin rim of the
 * accent colour along each roof. The same every time (a fixed seed). The tones come from the background's own colours, so it
 * follows the theme. */
static void bg_skyline(uint32_t *buf, int bw, int bh, int base, uint32_t cb, uint32_t accent, int style)
{
   const float acc[3] = { accent >> 16 & 255, accent >> 8 & 255, accent & 255 };
   const float far_c[3] = { (cb >> 16 & 255) * 1.00f + acc[0] * 0.16f, (cb >> 8 & 255) * 1.00f + acc[1] * 0.16f, (cb & 255) * 1.00f + acc[2] * 0.16f };
   const float near_c[3] = { (cb >> 16 & 255) * 0.92f + acc[0] * 0.26f, (cb >> 8 & 255) * 0.92f + acc[1] * 0.26f, (cb & 255) * 0.92f + acc[2] * 0.26f };
   const float lit[3] = { acc[0] * 0.85f + 255 * 0.15f, acc[1] * 0.85f + 255 * 0.15f, acc[2] * 0.85f + 255 * 0.15f };   /* (the theme's colour, a touch lighter) */
   unsigned seed = 0x2545f491u;
   int xx, yy, x;
   size_t sky_n = (size_t)bw * (size_t)(base + 1 > bh ? bh : base + 1);
   double tp0 = now_s(), tp1, tp2, tp3;
   uint32_t *before = malloc(sky_n * sizeof(uint32_t));   /* (the sky as it was, to tell where the skyline went) */
   uint32_t *orig = malloc((size_t)bw * bh * sizeof(uint32_t));   /* (the whole background as it was: solid shapes take their colour from it) */
   if (before)
      memcpy(before, buf, sky_n * sizeof(uint32_t));
   if (orig)
      memcpy(orig, buf, (size_t)bw * bh * sizeof(uint32_t));
if (style == 1)
      bg_sky_mountains(buf, bw, bh, base, cb, accent);
   else if (style == 2)
      bg_sky_forest(buf, bw, bh, base, accent, orig);
   else
   {
#define SKY_RND() (seed = seed * 1664525u + 1013904223u, seed >> 10)
   /* the mountains: a few slow waves added together */
   for (xx = 0; xx < bw; xx++)
   {
      float fx = (float)(xx - BG_M);
      int h = (int)(40 + 20 * sinf(fx * 0.0105f + 1.3f) + 12 * sinf(fx * 0.027f + 0.4f) + 6 * sinf(fx * 0.071f + 2.1f));
      if (h < 10)
         h = 10;
      for (yy = base - h; yy <= base; yy++)
         if (yy >= 0 && yy < bh)
            buf[(size_t)yy * bw + xx] = bg_toward(buf[(size_t)yy * bw + xx], far_c, yy == base - h ? 0.45f : 0.72f);
   }
   /* the city */
   x = 0;
   while (x < bw)
   {
      int w = 14 + (int)(SKY_RND() % 26), h = 24 + (int)(SKY_RND() % 66), step = SKY_RND() % 4 == 0, aerial = SKY_RND() % 6 == 0;
      int gap = (int)(SKY_RND() % 5), wx, wy, top;
      if (SKY_RND() % 8 == 0)
         h += 38;   /* now and then a tower */
      top = base - h;
      for (xx = x; xx < x + w && xx < bw; xx++)
         for (yy = top; yy <= base; yy++)
            if (yy >= 0 && yy < bh)
               buf[(size_t)yy * bw + xx] = bg_toward(buf[(size_t)yy * bw + xx], near_c, yy == top ? 0.55f : 0.94f);
      for (xx = x; xx < x + w && xx < bw; xx++)   /* the rim of light along the roof */
         if (top > 0 && top < bh)
            buf[(size_t)top * bw + xx] = bg_toward(buf[(size_t)top * bw + xx], acc, 0.16f);
      if (step && w > 16)
      {
         int sx = x + w / 4, sw = w / 2, sh = 8 + (int)(SKY_RND() % 8);
         for (xx = sx; xx < sx + sw && xx < bw; xx++)
            for (yy = top - sh; yy < top; yy++)
               if (yy >= 0 && yy < bh)
                  buf[(size_t)yy * bw + xx] = bg_toward(buf[(size_t)yy * bw + xx], near_c, yy == top - sh ? 0.55f : 0.94f);
         if (top - sh > 0 && top - sh < bh)
            for (xx = sx; xx < sx + sw && xx < bw; xx++)
               buf[(size_t)(top - sh) * bw + xx] = bg_toward(buf[(size_t)(top - sh) * bw + xx], acc, 0.16f);
      }
      if (aerial)
         for (yy = top - 14 - step * 8; yy < top - step * 8 && yy >= 0; yy++)
            if (x + w / 2 < bw)
               buf[(size_t)yy * bw + x + w / 2] = bg_toward(buf[(size_t)yy * bw + x + w / 2], near_c, 0.9f);
      /* a few windows lit, in rows */
      for (wy = top + 6; wy < base - 4; wy += 6)
         for (wx = x + 3; wx < x + w - 4; wx += 5)
            if (SKY_RND() % 11 == 0 && wx + 2 < bw)
               for (yy = wy; yy < wy + 3; yy++)
                  for (xx = wx; xx < wx + 2; xx++)
                     if (yy >= 0 && yy < bh)
                        buf[(size_t)yy * bw + xx] = bg_toward(buf[(size_t)yy * bw + xx], lit, 0.30f);
      x += w + gap;
   }
#undef SKY_RND
   }
   tp1 = now_s();
   /* its reflection on the floor: the skyline upside down, squashed, and only just there, fading with distance */
   if (before)
   {
      for (yy = 1; yy <= 44 && base + yy < bh; yy++)
      {
         int sy = base - yy * 2;
         float a = 0.5f * (1.0f - yy / 46.0f);
         if (sy < 0)
            break;
         for (xx = 0; xx < bw; xx++)
         {
            uint32_t now_px = buf[(size_t)sy * bw + xx];
            if (now_px != before[(size_t)sy * bw + xx])
            {
               float col[3] = { (now_px >> 16 & 255) * 0.7f, (now_px >> 8 & 255) * 0.7f, (now_px & 255) * 0.7f };
               buf[(size_t)(base + yy) * bw + xx] = bg_toward(buf[(size_t)(base + yy) * bw + xx], col, a);
            }
         }
      }
      free(before);
   }
   tp2 = now_s();
   bg_sky_front(buf, bw, bh, base, accent, style, orig);
   tp3 = now_s();
   mlog("skyline style %d: horizon %d ms, reflection %d ms, front %d ms", style, (int)((tp1 - tp0) * 1000), (int)((tp2 - tp1) * 1000), (int)((tp3 - tp2) * 1000));
   free(orig);
}

static uint32_t *bg_build(const canvas_t *c, int stage)
{
   /* a dark vertical blend lit from the top middle and, more faintly, from the
    * bottom middle (just above the hint bar). Each light is a wide soft glow
    * with a brighter, tighter core; dithered so the shallow steps don't band */
   uint32_t ct = TH(TH_BG_TOP), cb = TH(TH_BG_BOT), cl = TH(TH_BG_LIGHT);
   float top[3] = { ct >> 16 & 255, ct >> 8 & 255, ct & 255 }, bot[3] = { cb >> 16 & 255, cb >> 8 & 255, cb & 255 };
   float glow[3] = { cl >> 16 & 255, cl >> 8 & 255, cl & 255 }, core[3];
   int q, bw = c->w + 2 * BG_M, bh = c->h + 2 * BG_M, x, y, k, l;
   uint32_t *out_buf = malloc((size_t)bw * bh * 4);
   double t_start = now_s();
   const float gx = c->w * 0.5f, ty = -c->h * 0.08f, by = c->h - 40, yh = c->h * FLOOR_HORIZON;
   /* centre y, radii, soft strength, core radii, core strength */
   const float L[2][7] = {
      { ty, c->w * 0.93f, c->h * 0.95f, 0.45f, c->w * 0.45f, c->h * 0.50f, 0.55f },
      { by, c->w * 0.825f, c->h * 0.70f, 0.32f, c->w * 0.39f, c->h * 0.34f, 0.45f } };
   if (!out_buf)
      return NULL;
   for (q = 0; q < 3; q++)
      core[q] = glow[q] * 1.43f > 255 ? 255 : glow[q] * 1.43f;
   /* the lights, the pool and the vignette are smooth, so they're worked out on a coarse grid (every BG_STEP px) and
    * interpolated; the horizon's glow, the floor's tone (both depend on the row alone), the grid lines and the dither
    * are done per pixel */
   {
      enum { BG_STEP = 8 };
      int nx = bw / BG_STEP + 2, ny = bh / BG_STEP + 2, i, j;
      float *base = malloc((size_t)nx * ny * 3 * sizeof(float)), *rb = malloc((size_t)nx * 3 * sizeof(float));
      const uint32_t ac = C_ACCENT;
      const float acc[3] = { ac >> 16 & 255, ac >> 8 & 255, ac & 255 };
      if (!base || !rb)
      {
         free(base);
         free(rb);
         free(out_buf);
         return NULL;
      }
      for (j = 0; j < ny; j++)
         for (i = 0; i < nx; i++)
         {
            int xs = -BG_M + i * BG_STEP, ys = -BG_M + j * BG_STEP;
            float col[3], *o = base + ((size_t)j * nx + i) * 3;
            int yc = ys < 0 ? 0 : ys >= c->h ? c->h - 1 : ys;
            for (k = 0; k < 3; k++)
               col[k] = top[k] + (bot[k] - top[k]) * ((float)yc / c->h);
            for (l = 0; l < 2; l++)
            {
               float dx = (xs - gx) / L[l][1], dy = (ys - L[l][0]) / L[l][2];
               float cx = (xs - gx) / L[l][4], cy = (ys - L[l][0]) / L[l][5];
               float a = 1 - (dx * dx + dy * dy), b = 1 - (cx * cx + cy * cy);
               a = a > 0 ? a * a * L[l][3] : 0;
               b = b > 0 ? b * b * L[l][6] : 0;
               for (k = 0; k < 3; k++)
               {
                  col[k] += (glow[k] - col[k]) * a;
                  col[k] += (core[k] - col[k]) * b;
               }
            }
            /* depth, like the cards': a pool of light (the stage the focused card stands in) a little up and to the left,
             * and a vignette that darkens the edges and corners, so the screen reads as a space with the cards in front */
            {
               float qx = (xs - gx * 0.96f) / (c->w * BG_POOL_W), qy = (ys - c->h * 0.46f) / (c->h * BG_POOL_H);
               float r2 = qx * qx + qy * qy, pool = 1 - r2, vt = (r2 - 0.30f) / 1.2f, vig;
               pool = pool > 0 ? pool * pool * BG_POOL : 0;
               vt = vt < 0 ? 0 : vt > 1 ? 1 : vt;
               vig = vt * vt * (3 - 2 * vt) * BG_VIGNETTE;
               for (k = 0; k < 3; k++)
               {
                  col[k] += (core[k] - col[k]) * pool;
                  o[k] = col[k] * (1 - vig);
               }
            }
         }
      for (y = -BG_M; y < c->h + BG_M; y++)
      {
         int yy = y + BG_M, y0 = yy / BG_STEP;
         float fy = (yy % BG_STEP) / (float)BG_STEP, dyh = y - yh, glowh = 1.0f - fabsf(dyh) / 34.0f, gk = 0, fk = 0;
         float spx = 0, ispx = 0, dzl = 0, lfade = 0;
         const float *r0 = base + (size_t)y0 * nx * 3, *r1 = r0 + (size_t)nx * 3;
         for (i = 0; i < nx * 3; i++)
            rb[i] = r0[i] + (r1[i] - r0[i]) * fy;
         if (stage)
         {
            /* a floor below the horizon: darker, with a perspective grid (lines that meet at the horizon, spaced
             * further apart the nearer they are) fading out towards the horizon, and a thin bright glow along the
             * horizon itself (in the accent) */
            float fade;
            gk = glowh > 0 ? glowh * glowh * 0.75f : 0;
            if (dyh > 0)
            {
               /* the lines along the depth meet at the horizon's middle: their spacing on a row grows with the distance
                * from the horizon (0.9 px per px of dyh). The lines across are at equal steps of depth z = K / dyh, so
                * their spacing on screen is Sz * dyh^2 / K. */
               const float K = 3000.0f, Sz = 6.0f;
               float z = K / dyh;
               spx = 0.9f * dyh;
               ispx = 1 / spx;
               dzl = fabsf(z / Sz - floorf(z / Sz + 0.5f)) * Sz * dyh * dyh / K;
               fade = dyh / (c->h * 0.16f);
               fade = fade > 1 ? 1 : fade;
               lfade = dyh / (c->h * 0.30f);
               lfade = lfade > 1 ? 1 : lfade;
               fk = 0.62f * fade;   /* the floor itself: a deep tone of the accent, clearly apart from the lit sky above */
            }
         }
         for (x = -BG_M; x < c->w + BG_M; x++)
         {
            int xx = x + BG_M, i0 = xx / BG_STEP;
            float fx = (xx % BG_STEP) / (float)BG_STEP, d = dither(x & 1023, y & 1023), col[3];
            const float *p0 = rb + i0 * 3;
            uint32_t out = 0xff000000;
            for (k = 0; k < 3; k++)
               col[k] = p0[k] + (p0[k + 3] - p0[k]) * fx;
            if (gk > 0)
               for (k = 0; k < 3; k++)
                  col[k] += (acc[k] - col[k]) * gk;
            if (fk > 0)
            {
               float u = (x - gx) * ispx, dxl = fabsf(u - floorf(u + 0.5f)) * spx, line = 1.0f - (dxl < dzl ? dxl : dzl) / 1.8f;
               for (k = 0; k < 3; k++)
                  col[k] += (acc[k] * 0.28f - col[k]) * fk;
               if (line > 0)
                  for (k = 0; k < 3; k++)
                     col[k] += (acc[k] - col[k]) * line * lfade * FLOOR_GRID;
            }
            for (k = 0; k < 3; k++)
               out |= (uint32_t)(col[k] + d > 255 ? 255 : col[k] + d) << (16 - 8 * k);
            out_buf[(size_t)yy * bw + xx] = out;
         }
      }
      free(base);
      free(rb);
   }
   if (stage && theme_skyline)
      bg_skyline(out_buf, bw, bh, (int)yh + BG_M, cb, C_ACCENT, theme_sky_style);
   mlog("background %s built in %d ms", stage ? "stage" : "plain", (int)((now_s() - t_start) * 1000));
   return out_buf;
}

static void draw_background(canvas_t *c)
{
   uint32_t **slot = bg_stage_on ? &bg_stage : &bg;
   int y, dx = bg_dx, dy = bg_dy;
   if (!*slot)
      *slot = bg_build(c, bg_stage_on);
   if (!*slot)
   {
      gfx_clear(c, TH(TH_BG_TOP));
      return;
   }
   bg_cur = *slot;
   bg_w = c->w + 2 * BG_M;
   dx = dx < -BG_M ? -BG_M : dx > BG_M ? BG_M : dx;
   dy = dy < -BG_M ? -BG_M : dy > BG_M ? BG_M : dy;
   bg_dx = dx;
   bg_dy = dy;
   for (y = 0; y < c->h; y++)
      memcpy(c->px + (size_t)y * c->stride, bg_cur + (size_t)(y + BG_M + dy) * bg_w + BG_M + dx, (size_t)c->w * 4);
}

/* where item i sits relative to the row's animated position, looping: -n/2 .. n/2 */
static double item_offset(int r, int i)
{
   int n = row_count(r);
   double d = fmod(i - ui.anim_[r], n);
   if (d < -n / 2.0)
      d += n;
   if (d >= n / 2.0)
      d -= n;
   return d;
}

static void row_item(int r, int i, char *name, size_t nsz, char *sub, size_t ssz)
{
   if (r == ROW_CONS && !nvisible)
   {
      /* no games on the card yet: the row holds one card that says so */
      snprintf(name, nsz, "No Games Found");
      snprintf(sub, ssz, "Copy games to roms/ on the card");
   }
   else if (r == ROW_CONS)
   {
      system_t *s = &systems[visible[i]];
      snprintf(name, nsz, "%s", s->name);
      snprintf(sub, ssz, "%d game%s", s->count, s->count == 1 ? "" : "s");
   }
   else if (r == ROW_COLL)
   {
      int n = colls[i].count;
      snprintf(name, nsz, "%s", colls[i].name);
      if (n)
         snprintf(sub, ssz, "%d game%s", n, n == 1 ? "" : "s");
      else
         snprintf(sub, ssz, "%s", i == COLL_SEARCH ? "Find a game" : i == COLL_RECENT ? "Nothing played yet"
               : i == COLL_FAVS ? "No favorites yet" : "Empty");
   }
   else if (r == ROW_MEDIA)
   {
      int n = i < mf.n ? mf.count[i] : mf.loose;
      const char *what = mf.video ? (n == 1 ? "video" : "videos") : (n == 1 ? "song" : "songs");
      snprintf(name, nsz, "%s", i < mf.n ? mf.name[i] : mf.video ? "Other Videos" : "Other Music");
      if (n)
         snprintf(sub, ssz, "%d %s", n, what);
      else
         snprintf(sub, ssz, "Empty");
   }
   else
   {
      snprintf(name, nsz, "%s", tool_names[i]);
      snprintf(sub, ssz, "%s", i == TOOL_RETROARCH ? "Open without a game"
            : i == TOOL_PICTURES ? "From the Image Generator"
            : "");
   }
}

/* ------------------------------------------------------------- status */

static int bat_low;   /* on the battery and at or under the warning level: the stars twinkle more slowly (fewer frames) */
static int st_battery = -1, st_power;   /* refreshed every few seconds in the main loop */
/* ---- the click when you scroll: the factory menu's own sound (keytone.wav: a 4-byte
 * length, then 16-bit stereo samples at 8000 Hz), played from a small thread so the
 * menu never waits for the sound card. The sound card is opened for a click and let go
 * two seconds after the last one, so a game or a video can have it. */
#define KEYTONE "/oem/release/config/keytone.wav"
static struct
{
   pthread_t th;
   pthread_mutex_t m;
   pthread_cond_t c;
   int started, req, vol;       /* vol: 0 to 100 (Settings > Sound > Menu click volume, 50 unless set), 0 = silent */
   int16_t *data, *buf;
   int frames;
} kc = { 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0, 50, NULL, NULL, 0 };
static int play_mode;            /* this process is the video / music player (set in main) */

static void *click_main(void *arg)
{
   snd_pcm_t *pcm = NULL;
   (void)arg;
   for (;;)
   {
      struct timespec ts;
      int req;
      pthread_mutex_lock(&kc.m);
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 2;
      while (!kc.req)
         if (pthread_cond_timedwait(&kc.c, &kc.m, &ts) != 0)
            break;
      req = kc.req;
      kc.req = 0;
      pthread_mutex_unlock(&kc.m);
      if (!req)
      {
         if (pcm)
         {
            snd_pcm_close(pcm);   /* idle: leave the sound card free */
            pcm = NULL;
         }
         continue;
      }
      if (!pcm)
      {
         if (snd_pcm_open(&pcm, BOARD_PCM_DEVICE, SND_PCM_STREAM_PLAYBACK, 0) < 0)
         {
            pcm = NULL;
            continue;
         }
         if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 2, 8000, 1, 100000) < 0)
         {
            snd_pcm_close(pcm);
            pcm = NULL;
            continue;
         }
      }
      snd_pcm_drop(pcm);       /* a click while one is playing starts it again */
      snd_pcm_prepare(pcm);
      {
         /* its own volume, on top of the handheld's: the samples scaled (once the volume's known) */
         int v = kc.vol, i;
         if (v > 100)
            v = 100;
         if (!kc.buf && !(kc.buf = malloc((size_t)kc.frames * 4)))
            continue;
         for (i = 0; i < kc.frames * 2; i++)
            kc.buf[i] = (int16_t)(kc.data[i] * v / 100);
      }
      if (snd_pcm_writei(pcm, kc.buf, kc.frames) < 0)
      {
         snd_pcm_close(pcm);   /* something's wrong with it: open it afresh next time */
         pcm = NULL;
      }
   }
   return NULL;
}

static void key_click(void)
{
   if (play_mode || kc.vol <= 0)
      return;
   if (!kc.started)
   {
      FILE *f = fopen(KEYTONE, "rb");
      uint32_t len = 0;
      kc.started = -1;   /* tried (stays -1 if the file isn't there) */
      if (f && fread(&len, 4, 1, f) == 1 && len >= 4 && len <= (1u << 20) && (kc.data = malloc(len)) &&
            fread(kc.data, 1, len, f) == len)
      {
         kc.frames = (int)(len / 4);
         if (pthread_create(&kc.th, NULL, click_main, NULL) == 0)
         {
            pthread_detach(kc.th);
            kc.started = 1;
         }
      }
      if (f)
         fclose(f);
   }
   if (kc.started != 1)
      return;
   pthread_mutex_lock(&kc.m);
   kc.req = 1;
   pthread_cond_signal(&kc.c);
   pthread_mutex_unlock(&kc.m);
}

static int screen_off_s = 180, power_off_min = 10, clock_24h;   /* from g36/settings.cfg */
static int popup_lift;   /* messages and the volume bar move up this much (above a video's controls) */
static double vol_until;                /* volume bar visible until */
static int st_volume = -1;
#define CLOCK_SET "/etc/g36-clock-set"  /* the time was set by hand (Settings): show the clock */
#define GAMEBOX_JSON "/oem/release/config/gamebox.json"

static void draw_mask(canvas_t *c, const char *const *shape, int rows, int x, int y, uint32_t col)
{
   int i, j;
   for (j = 0; j < rows; j++)
      for (i = 0; shape[j][i]; i++)
         if (shape[j][i] == '#')
            gfx_fill_rect(c, x + i, y + j, 1, 1, col);
}

/* battery (and the clock, once it has been set), right-aligned at x, centred on cy;
 * returns its width */
static int draw_status(canvas_t *c, int x, int cy, int pill)
{
   static const char *const bolt[] = {
      "...##.", "..##..", ".##...", "######", "...##.", "..##..", ".##...", "##....",
   };
   char pct[16], clk[16] = "";
   int bw = 26, bh = 13, tw, cw = 0, w, bx, by = cy - bh / 2, fill;
   uint32_t col = st_power ? C_CHARGE : st_battery >= 0 && st_battery <= 20 ? C_LOW : C_TEXT;

   if (st_battery < 0)
      return 0;
   snprintf(pct, sizeof(pct), "%d%%", st_battery);
   if (file_exists(CLOCK_SET))
   {
      time_t now = time(NULL);
      struct tm tm;
      localtime_r(&now, &tm);
      if (clock_24h)
         snprintf(clk, sizeof(clk), "%d:%02d", tm.tm_hour, tm.tm_min);
      else
         snprintf(clk, sizeof(clk), "%d:%02d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_min,
               tm.tm_hour < 12 ? "AM" : "PM");
      cw = font_text_width(f_small, clk) + 16;
   }
   tw = font_text_width(f_small, pct);
   w = cw + tw + 8 + bw + 3;
   if (pill)
      gfx_fill_rect(c, x - w - 12, cy - 14, w + 24, 28, 0xc0000000);
   if (clk[0])
      gfx_draw_text(c, f_small, clk, x - w, cy - font_height(f_small) / 2, C_TEXT, 0);
   gfx_draw_text(c, f_small, pct, x - w + cw, cy - font_height(f_small) / 2,
         st_battery <= 20 && !st_power ? C_LOW : C_DIM, 0);
   bx = x - bw - 3;
   /* outline, nub, level */
   gfx_fill_rect(c, bx, by, bw, 2, col);
   gfx_fill_rect(c, bx, by + bh - 2, bw, 2, col);
   gfx_fill_rect(c, bx, by, 2, bh, col);
   gfx_fill_rect(c, bx + bw - 2, by, 2, bh, col);
   gfx_fill_rect(c, bx + bw, by + 4, 3, bh - 8, col);
   fill = (bw - 6) * (st_battery > 100 ? 100 : st_battery) / 100;
   gfx_fill_rect(c, bx + 3, by + 3, fill, bh - 6, col);
   if (st_power)
      draw_mask(c, bolt, 8, bx + bw / 2 - 3, by + 2, 0xff1c121f);
   return w + 12;
}

/* the volume, from the sound system's Master control (the factory volume helper
 * changes it; its own settings file is only saved later) */
static snd_mixer_t *mixer;
static snd_mixer_elem_t *master;

static void mixer_open(void)
{
   snd_mixer_selem_id_t *sid;
   if (snd_mixer_open(&mixer, 0) < 0)
      return;
   if (snd_mixer_attach(mixer, BOARD_MIXER_DEVICE) < 0 || snd_mixer_selem_register(mixer, NULL, NULL) < 0 ||
         snd_mixer_load(mixer) < 0)
   {
      snd_mixer_close(mixer);
      mixer = NULL;
      return;
   }
   snd_mixer_selem_id_alloca(&sid);
   snd_mixer_selem_id_set_name(sid, BOARD_MIXER_ELEMENT);
   master = snd_mixer_find_selem(mixer, sid);
}

static int read_volume(void)
{
   long v, lo, hi;
   char buf[512], *p;
   if (!mixer)
      mixer_open();
   if (master)
   {
      snd_mixer_handle_events(mixer);
      snd_mixer_selem_get_playback_volume_range(master, &lo, &hi);
      if (hi > lo && snd_mixer_selem_get_playback_volume(master, SND_MIXER_SCHN_FRONT_LEFT, &v) == 0)
         return (int)((v - lo) * 100 + (hi - lo) / 2) / (hi - lo);
   }
   /* fall back on the helper's saved value */
   if (read_file(GAMEBOX_JSON, buf, sizeof(buf)) < 0 || !(p = strstr(buf, "\"volume\"")))
      return -1;
   p = strchr(p, ':');
   return p ? atoi(p + 1) : -1;
}

static void draw_volume(canvas_t *c, double now)
{
   int w = 300, h = 54, x = c->w / 2 - w / 2, y = 372 - popup_lift, bar;
   char t[16];
   if (now > vol_until || st_volume < 0)
      return;
   gfx_fill_rect(c, x, y, w, h, C_PANEL);
   gfx_fill_rect(c, x, y, 4, h, C_ACCENT);
   gfx_draw_text(c, f_small, "Volume", x + 20, y + (h - font_height(f_small)) / 2, C_DIM, 0);
   bar = 150 * (st_volume > 100 ? 100 : st_volume) / 100;
   gfx_fill_rect(c, x + 90, y + h / 2 - 4, 150, 8, C_SEL);
   gfx_fill_rect(c, x + 90, y + h / 2 - 4, bar, 8, C_ACCENT);
   snprintf(t, sizeof(t), "%d", st_volume);
   gfx_draw_text(c, f_small, t, x + 252, y + (h - font_height(f_small)) / 2, C_TEXT, 0);
}

/* ---- Controls: the old two columns, as lists you move around in (Controls card, or Y on the home screen) ----
 * Left: the buttons in the menu. Right: the buttons in a game, for the console picked with L / R ("All consoles" first; a
 * console without a page of its own follows it). Each row shows the real button that does it; A on a row, then press the
 * button you want. Saved in g36/remap.cfg, one line per page:
 *    menu pad=a,b,x,y,l,r,select,start,menu
 *    <* or console id> pad=a,b,x,y,l,r,select,start,menu hot=select,b,r,l,x,a,left,right
 * pad: the button that acts as the A B X Y L R Select Start Menu (the menu's, or the game's; the last is the menu button's job:
 * video options in the menu, RetroArch's menu in a game; a line with only eight is the same with Menu on Menu); hot: the hold button, then the
 * buttons for quit, save state, load state, fast-forward, pause, save slot down, save slot up. The menu line is applied to
 * every press in the menu (input.c); for a game, retroarch_r turns the console's line into a changed copy of the controller's
 * autoconfig and hotkey settings. */
#define REMAP_FILE SDCARD "/g36/remap.cfg"
typedef struct { char scope[32]; unsigned char pad[9], hk[8]; int dev; } remap_t;   /* dev: the controller type told to the emulator, 0 = leave it */
#define DEV_DUALSHOCK 517   /* the PlayStation core's DualShock (1 standard, 261 analog, 517 dualshock, 773 negcon) */
static remap_t remap_tab[MAX_SYSTEMS + 2];
static int remap_n;
static const unsigned char hk_default[8] = { PB_SELECT, PB_B, PB_R, PB_L, PB_X, PB_A, PB_LEFT, PB_RIGHT };
static const char *const hk_label[8] = { "Hold for the shortcuts", "Quit the game", "Save state", "Load state", "Fast-forward on / off",
   "Pause", "Save slot down", "Save slot up" };

typedef struct { int kind; int id; char label[40], key[24]; } ctl_item_t;   /* kind: 0 menu button, 1 game button, 2 shortcut, 3 fixed, 4 controller type */
static struct
{
   int page[MAX_SYSTEMS + 2], npage, pi;      /* page: -1 all consoles, else a console's index */
   int col, row[2], top[2], capture;
   double capture_until;   /* when waiting for a button gives up */
   ctl_item_t it[2][24];
   int n[2], nsel[2];
} rm;

static int pb_find(const char *name)
{
   int i;
   for (i = 0; i < PB_N; i++)
      if (!strcmp(pb_name[i], name))
         return i;
   return -1;
}

static int remap_find(const char *scope)
{
   int i;
   for (i = 0; i < remap_n; i++)
      if (!strcmp(remap_tab[i].scope, scope))
         return i;
   return -1;
}

/* "a,b,x,..." -> button numbers; 1 if there are n of them, all different, each below lim */
static int remap_list(const char *v, unsigned char *out, int n, int lim)
{
   char tmp[100], *tok, *sp = NULL;
   int k = 0, seen = 0;
   snprintf(tmp, sizeof(tmp), "%s", v);
   for (tok = strtok_r(tmp, ",", &sp); tok && k < n; tok = strtok_r(NULL, ",", &sp), k++)
   {
      int b = pb_find(tok);
      if (b < 0 || b >= lim || (seen >> b & 1))
         return 0;
      seen |= 1 << b;
      out[k] = (unsigned char)b;
   }
   return k == n && !tok;
}

static void remap_load(void)
{
   FILE *f = fopen(REMAP_FILE, "r");
   char line[300];
   remap_n = 0;
   if (!f)
      return;
   while (fgets(line, sizeof(line), f) && remap_n < MAX_SYSTEMS + 2)
   {
      char *tok, *sp = NULL;
      remap_t e;
      int havepad = 0;
      if (line[0] == '#' || !(tok = strtok_r(line, " \t\r\n", &sp)) || strlen(tok) >= sizeof(e.scope))
         continue;
      memset(&e, 0, sizeof(e));
      snprintf(e.scope, sizeof(e.scope), "%s", tok);
      memcpy(e.hk, hk_default, 8);
      while ((tok = strtok_r(NULL, " \t\r\n", &sp)))
      {
         if (!strncmp(tok, "pad=", 4))
         {
            havepad = remap_list(tok + 4, e.pad, 9, 9);
            if (!havepad && remap_list(tok + 4, e.pad, 8, 8))
            {
               e.pad[8] = PB_MENU;
               havepad = 1;
            }
         }
         else if (!strncmp(tok, "hot=", 4))
         {
            int q, bad = !remap_list(tok + 4, e.hk, 8, PB_N);
            for (q = 0; q < 8; q++)
               if (e.hk[q] == PB_MENU)
                  bad = 1;   /* (the menu button can't be a shortcut button) */
            if (bad)
               memcpy(e.hk, hk_default, 8);
         }
         else if (!strncmp(tok, "dev=", 4))
            e.dev = atoi(tok + 4);
      }
      if (havepad && remap_find(e.scope) < 0)
         remap_tab[remap_n++] = e;
   }
   fclose(f);
}

static void remap_save(void)
{
   char tmp[300];
   FILE *f;
   int i, k;
   snprintf(tmp, sizeof(tmp), "%s.tmp", REMAP_FILE);
   if (!(f = fopen(tmp, "w")))
      return;
   fprintf(f, "# Controls screen. One line per page: menu, * (all consoles) or a console id; pad = the button that acts as\n");
   fprintf(f, "# A B X Y L R Select Start Menu; hot = hold button, quit, save state, load state, fast-forward, pause, slot down, slot up.\n");
   for (i = 0; i < remap_n; i++)
   {
      fprintf(f, "%s pad=", remap_tab[i].scope);
      for (k = 0; k < 9; k++)
         fprintf(f, "%s%s", k ? "," : "", pb_name[remap_tab[i].pad[k]]);
      if (strcmp(remap_tab[i].scope, "menu"))
      {
         fprintf(f, " hot=");
         for (k = 0; k < 8; k++)
            fprintf(f, "%s%s", k ? "," : "", pb_name[remap_tab[i].hk[k]]);
         if (remap_tab[i].dev)
            fprintf(f, " dev=%d", remap_tab[i].dev);
      }
      fprintf(f, "\n");
   }
   fflush(f);
   fsync(fileno(f));
   fclose(f);
   rename(tmp, REMAP_FILE);
}

/* the menu's buttons are changed in input.c, and the hint bar shows the real ones */
static void menu_map_apply(void)
{
   int i = remap_find("menu"), k, real[9];
   for (k = 0; k < 9; k++)
      menu_m[k] = (unsigned char)(i >= 0 ? remap_tab[i].pad[k] : k);
   menu_remapped = 0;
   for (k = 0; k < 9; k++)
   {
      real[k] = pb_code[menu_m[k]];
      if (menu_m[k] != k)
         menu_remapped = 1;
   }
   input_set_map(menu_remapped ? real : NULL);
}

static const char *ctl_scope(int col)
{
   return col == 0 ? "menu" : rm.page[rm.pi] < 0 ? "*" : systems[rm.page[rm.pi]].id;
}

/* what a list uses now: its own line, else (a console) the all-consoles line, else nothing changed; 1 when it has its own */
static int remap_effective(int col, unsigned char pad[9], unsigned char hk[8])
{
   int i = remap_find(ctl_scope(col)), own = i >= 0, k;
   if (i < 0 && col == 1 && rm.page[rm.pi] >= 0)
      i = remap_find("*");
   for (k = 0; k < 9; k++)
      pad[k] = (unsigned char)(i >= 0 ? remap_tab[i].pad[k] : k);
   for (k = 0; k < 8; k++)
      hk[k] = i >= 0 ? remap_tab[i].hk[k] : hk_default[k];
   return own;
}

static void remap_set(int col, const unsigned char pad[9], const unsigned char hk[8])
{
   int i = remap_find(ctl_scope(col));
   if (i < 0)
   {
      if (remap_n >= MAX_SYSTEMS + 2)
         return;
      i = remap_n++;
      memset(&remap_tab[i], 0, sizeof(remap_tab[i]));
      snprintf(remap_tab[i].scope, sizeof(remap_tab[i].scope), "%s", ctl_scope(col));
   }
   memcpy(remap_tab[i].pad, pad, 9);
   memcpy(remap_tab[i].hk, hk, 8);
   remap_save();
   menu_map_apply();
}

/* the controller type set for this console's own page (0 = standard) */
static int remap_dev(void)
{
   int i = remap_find(ctl_scope(1));
   return i >= 0 ? remap_tab[i].dev : 0;
}

static void remap_dev_toggle(void)
{
   unsigned char pad[9], hk[8];
   int i, v = remap_dev() ? 0 : DEV_DUALSHOCK;
   remap_effective(1, pad, hk);
   if (remap_find(ctl_scope(1)) < 0)
      remap_set(1, pad, hk);
   if ((i = remap_find(ctl_scope(1))) >= 0)
   {
      remap_tab[i].dev = v;
      remap_save();
   }
}

static void ctl_add(int col, int kind, int id, const char *label, const char *key)
{
   ctl_item_t *e = &rm.it[col][rm.n[col]++];
   e->kind = kind;
   e->id = id;
   snprintf(e->label, sizeof(e->label), "%s", label);
   snprintf(e->key, sizeof(e->key), "%s", key ? key : "");
}

/* the rows of both lists; the ones you can change come first */
static void ctl_build(void)
{
   static const char *const menu_what[9] = { "Open / play", "Back", "Search", "Favorite", "Previous letter", "Next letter",
      "All Games", "Search matches / remove recent", "Video options" };
   static const char *const two[] = { "nes", "famicom", "fds", "gb", "gbc", "mastersystem", "gamegear", "sg-1000", "atari2600",
      "atari5200", "atari7800", "atarilynx", "pcengine", "tg16", "pcenginecd", "ngp", "ngpc", NULL };
   int k, kind = 0, sc = rm.page[rm.pi];
   char lab[40];
   rm.n[0] = rm.n[1] = 0;
   for (k = 0; k < 9; k++)
      ctl_add(0, 0, k, menu_what[k], NULL);
   rm.nsel[0] = rm.n[0];
   ctl_add(0, 3, 0, "Move", "D-pad");
   ctl_add(0, 3, 0, "Choose the emulator", "Hold Select + A");
   ctl_add(0, 3, 0, "Screen off / on", "Tap Power");
   ctl_add(0, 3, 0, "Power off / restart", "Hold Power");
   ctl_add(0, 3, 0, "Volume", "Vol + / -");
   if (sc >= 0)
   {
      const char *id = systems[sc].id;
      for (k = 0; two[k]; k++)
         if (!strcmp(id, two[k]))
            kind = 1;
      if (!strcmp(id, "gba"))
         kind = 2;
   }
   for (k = 0; k < 8; k++)
      if (kind == 0 || (kind == 1 && (k < 2 || k > 5)) || (kind == 2 && k != 2 && k != 3))
      {
         snprintf(lab, sizeof(lab), "The game's %c%s", k < 6 ? "ABXYLR"[k] : ' ', k == 6 ? "Select" : k == 7 ? "Start" : "");
         if (k >= 6)
            snprintf(lab, sizeof(lab), "The game's %s", k == 6 ? "Select" : "Start");
         ctl_add(1, 1, k, lab, NULL);
      }
   ctl_add(1, 1, 8, "RetroArch's menu", NULL);
   for (k = 0; k < 8; k++)
      ctl_add(1, 2, k, hk_label[k], NULL);
   if (sc >= 0 && !strcmp(systems[sc].id, "psx"))
      ctl_add(1, 4, 0, "Controller type", NULL);
   rm.nsel[1] = rm.n[1];
   ctl_add(1, 3, 0, "Move", "D-pad");
   ctl_add(1, 3, 0, "Save and power off", "Hold Power");
   for (k = 0; k < 2; k++)
      if (rm.row[k] >= rm.nsel[k])
         rm.row[k] = rm.nsel[k] - 1;
}

static int controls_open(void)
{
   int i;
   remap_load();
   rm.npage = 0;
   rm.page[rm.npage++] = -1;
   for (i = 0; i < nsystems; i++)
      if (systems[i].count > 0)
         rm.page[rm.npage++] = i;
   rm.pi = rm.col = rm.capture = 0;
   rm.row[0] = rm.row[1] = rm.top[0] = rm.top[1] = 0;
   ctl_build();
   input_set_raw(1);
   return 1;
}

/* the item row i of a list uses button b now */
static void remap_assign(int col, const ctl_item_t *e, int b)
{
   unsigned char pad[9], hk[8];
   int k;
   remap_effective(col, pad, hk);
   if (e->kind == 2)
   {
      for (k = 0; k < 8; k++)
         if (k != e->id && hk[k] == b)
         {
            hk[k] = hk[e->id];
            break;
         }
      hk[e->id] = (unsigned char)b;
   }
   else
   {
      for (k = 0; k < 9; k++)
         if (k != e->id && pad[k] == b)
         {
            pad[k] = pad[e->id];
            break;
         }
      pad[e->id] = (unsigned char)b;
   }
   remap_set(col, pad, hk);
}

#define CTL_VIS 14
#define CTL_TIMEOUT 5   /* seconds to press the button you want before the change is dropped */
/* a real button press on the Controls screen; returns 1 when the screen should close */
static int remap_key(int code)
{
   int c = rm.col, b, k;
   if (rm.capture)
   {
      const ctl_item_t *e = &rm.it[c][rm.row[c]];
      /* (the Menu button can be chosen like the others; with no button for CTL_TIMEOUT seconds the change is dropped) */
      for (b = 0; b < PB_N; b++)
         if (code == pb_code[b] && (e->kind == 2 ? b != PB_MENU : b <= PB_MENU))
         {
            remap_assign(c, e, b);
            rm.capture = 0;
         }
      return 0;
   }
   if (code == BTN_EAST)
   {
      input_set_raw(0);
      return 1;
   }
   if (code == KEY_UP && rm.row[c] > 0)
      rm.row[c]--;
   else if (code == KEY_DOWN && rm.row[c] < rm.nsel[c] - 1)
      rm.row[c]++;
   else if (code == KEY_LEFT || code == KEY_RIGHT)
      rm.col = code == KEY_LEFT ? 0 : 1;
   else if ((code == BTN_TL || code == BTN_TR) && rm.npage > 1)
   {
      rm.pi = (rm.pi + (code == BTN_TL ? rm.npage - 1 : 1)) % rm.npage;
      rm.top[1] = 0;
      ctl_build();
   }
   else if (code == BTN_SOUTH && rm.it[c][rm.row[c]].kind == 4)
      remap_dev_toggle();
   else if (code == BTN_SOUTH)
   {
      rm.capture = 1;
      rm.capture_until = now_s() + CTL_TIMEOUT;
   }
   else if (code == BTN_START || code == KEY_MENU)
   {
      int i = remap_find(ctl_scope(c));
      if (i >= 0)
      {
         memmove(&remap_tab[i], &remap_tab[i + 1], (size_t)(remap_n - i - 1) * sizeof(remap_t));
         remap_n--;
         remap_save();
         menu_map_apply();
      }
   }
   if (rm.row[c] < rm.top[c])
      rm.top[c] = rm.row[c];
   if (rm.row[c] >= rm.top[c] + CTL_VIS)
      rm.top[c] = rm.row[c] - CTL_VIS + 1;
   for (k = 0; k < 2; k++)
      if (rm.n[k] <= CTL_VIS)
         rm.top[k] = 0;
   return 0;
}

static inline uint32_t blend256(uint32_t d, uint32_t s, unsigned a);
static void draw_title_pill(canvas_t *c, const char *title, int cx, int py0);
static void fill_round_rect_alpha(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col, int alpha);

static void draw_controls(canvas_t *c)
{
   unsigned char pad[2][9], hk[2][8];
   int own[2], col, i, sc = rm.page[rm.pi], cx0[2] = { 30, 450 }, rt[2] = { 418, 828 }, y0 = 114, rh = 22;
   char num[24];
   const char *name = sc < 0 ? "All consoles" : systems[sc].name;
   glyph_raw = 1;
   own[0] = remap_effective(0, pad[0], hk[0]);
   own[1] = remap_effective(1, pad[1], hk[1]);
   {
      /* behind it: the home screen's background without the cards, under a dark filter at 75%. The result is kept, so a frame is
       * one copy; it is made again when the theme's colours or the screen size change. */
      size_t n = (size_t)c->w * c->h;
      uint32_t key = (uint32_t)c->w * 2654435761u ^ (uint32_t)c->h * 40503u ^ TH(TH_BG_TOP) ^ TH(TH_BG_BOT) * 31u;
      if (!ctl_bgbuf || ctl_bgkey != key)
      {
         int bdx = bg_dx, bdy = bg_dy, x, y;
         free(ctl_bgbuf);
         ctl_bgbuf = malloc(n * 4);
         bg_dx = bg_dy = 0;
         bg_stage_on = 1;
         draw_background(c);
         bg_stage_on = 0;
         bg_dx = bdx;
         bg_dy = bdy;
         if (ctl_bgbuf)
         {
            for (y = 0; y < c->h; y++)
            {
               uint32_t *row = c->px + (size_t)y * c->stride;
               for (x = 0; x < c->w; x++)
                  row[x] = blend256(row[x], 0xff190f1d, 192);
               memcpy(ctl_bgbuf + (size_t)y * c->w, row, (size_t)c->w * 4);
            }
            ctl_bgkey = key;
         }
      }
      else
         for (i = 0; i < c->h; i++)
            memcpy(c->px + (size_t)i * c->stride, ctl_bgbuf + (size_t)i * c->w, (size_t)c->w * 4);
   }
   {
      /* a faint box round each column: a slightly darker inside and a thin line in the accent colour, barely there */
      static const int bx[2] = { 8, 438 }, bw[2] = { 426, 408 };
      uint32_t edge = blend256(0xff190f1d, C_ACCENT, 70);
      int k;
      for (k = 0; k < 2; k++)
      {
         fill_round_rect_alpha(c, bx[k], 51, bw[k], 381, 12, 0xff000000, 60);
         stroke_round_rect(c, bx[k], 51, bw[k], 381, 12, edge);
      }
   }
   draw_title_pill(c, "Controls", c->w / 2, 6);
   gfx_draw_text(c, f_small, "IN THE MENU", cx0[0], 54, C_ACCENT, 0);
   gfx_draw_text(c, f_small, "IN A GAME", cx0[1], 54, C_ACCENT, 0);
   {
      int nw = font_text_width(f_small, name), mw = nw > 200 ? 200 : nw, ty = 90 - font_height(f_small) / 2;
      const char *note = !own[1] && sc >= 0 ? "same as All consoles" : "";
      glyph_one(c, "L", cx0[1], 90, 1);
      gfx_draw_text(c, f_small, name, cx0[1] + 32, ty, C_TEXT, 200);
      glyph_one(c, "R", cx0[1] + 32 + mw + 8, 90, 1);
      snprintf(num, sizeof(num), "%d / %d", rm.pi + 1, rm.npage);
      gfx_draw_text(c, f_small, num, c->w - 20 - font_text_width(f_small, num), ty, C_FAINT, 0);
      gfx_draw_text(c, f_small, note, c->w - 20 - font_text_width(f_small, note), 54, C_FAINT, 0);
   }
   for (col = 0; col < 2; col++)
   {
      int sel_row = rm.row[col];
      for (i = rm.top[col]; i < rm.n[col] && i < rm.top[col] + CTL_VIS; i++)
      {
         const ctl_item_t *e = &rm.it[col][i];
         int y = y0 + (i - rm.top[col]) * rh + rh / 2, fh = font_height(f_small), x = cx0[col];
         if (e->kind != 3 && i == sel_row && col == rm.col)
            fill_round_rect(c, x - 10, y - rh / 2, col ? c->w - x - 8 : 410, rh, 8, C_SEL);
         if (e->kind == 3)
         {
            int lw = font_text_width(f_small, e->label);
            gfx_draw_text(c, f_small, e->key, x, y - fh / 2, C_DIM, col ? 110 : 150);
            gfx_draw_text(c, f_small, e->label, rt[col] - (lw > 250 ? 250 : lw), y - fh / 2, C_FAINT, 250);
            continue;
         }
         if (e->kind == 4)
            gfx_draw_text(c, f_small, remap_dev() ? "DualShock" : "Standard", x, y - fh / 2, remap_dev() ? C_ACCENT : C_TEXT, 120);
         else if (i == sel_row && col == rm.col && rm.capture)
         {
            /* in place of the whole row, so nothing runs into its name */
            char msg[64];
            int left = (int)ceil(rm.capture_until - now_s());
            snprintf(msg, sizeof(msg), "Press a button  (%d)", left < 0 ? 0 : left);
            gfx_draw_text(c, f_small, msg, x, y - fh / 2, C_ACCENT, 0);
         }
         else if (e->kind == 2)
         {
            int w = glyph_one(c, pb_glyph[hk[col][0]], x, y, 1);
            if (e->id > 0)
            {
               gfx_draw_text(c, f_small, "+", x + w + 6, y - fh / 2, C_DIM, 0);
               glyph_one(c, pb_glyph[hk[col][e->id]], x + w + 22, y, 1);
            }
         }
         else
            glyph_one(c, pb_glyph[pad[col][e->id]], x, y, 1);
         {
            /* the name of what the button does sits against the right edge, so it stays in view while the button is being chosen */
            int lw = font_text_width(f_small, e->label);
            gfx_draw_text(c, f_small, e->label, rt[col] - (lw > 300 ? 300 : lw), y - fh / 2,
                  col == rm.col && i == sel_row ? C_TEXT : C_DIM, 300);
         }
      }
      if (rm.n[col] > CTL_VIS)
      {
         int bar = (CTL_VIS * CTL_VIS * rh) / rm.n[col], by = y0 + rm.top[col] * (CTL_VIS * rh - bar) / (rm.n[col] - CTL_VIS);
         gfx_fill_rect(c, col ? c->w - 8 : 434, by, 3, bar, C_FAINT);
      }
   }
   {
      char wait_hint[32];
      int left = (int)ceil(rm.capture_until - now_s());
      snprintf(wait_hint, sizeof(wait_hint), "%ds  Cancel", left < 0 ? 0 : left);
      draw_hints(c, rm.capture ? wait_hint
         : "A  Change     Dpad  Navigation     L / R  Console     Start / Menu  Reset     B  Back");
   }
   glyph_raw = 0;
}

/* Home screen: every item is a card. The focused card is full size; cards shrink the
 * further they are from it, sideways and in the rows above and below, which stay
 * partly visible at the screen edges. */
#define CARD_W    460
#define CARD_H    210
#define CARD_GAP  410     /* card centres along a row, at full size */
#define SIDE_PUSH 20      /* the cards beside the focused one sit this much further out, so more of the background shows */
#define ROW_GAP   235     /* row centres */
#define HOME_CY   222     /* centre of the focused row */
#define GS        224     /* a game card in the games carousel: square, a touch taller than the home screen's cards */

typedef struct
{
   int r, i, cx, cy;
   double scale, depth;
   double rowd;      /* the card's row offset: <0 above the focused row, >0 below */
} card_t;

static int side_push(double off)
{
   double a = fabs(off) < 1 ? fabs(off) : 1;
   return (int)(a * SIDE_PUSH + 0.5) * (off < 0 ? -1 : 1);
}

static double card_scale_x(double dx)
{
   double s = dx <= 1 ? 1 - 0.28 * dx : 0.72 - 0.12 * (dx - 1);
   return s < 0.45 ? 0.45 : s;
}

static double card_scale_y(double dy)
{
   return 1 - 0.32 * (dy < 1 ? dy : 1);
}

/* Each card is drawn once at full size and then only scaled, so it looks the same
 * wherever it is; it's redrawn only when its text changes (a game count, say). */
typedef struct
{
   image_t img;           /* full size */
   image_t *half;         /* half size, for small cards in motion */
   image_t *scaled;       /* smooth copy at the size it last rested at */
   int last_w, last_h;    /* size asked for on the previous frame */
   char key[160];
} cardimg_t;

static cardimg_t card_cache[NROWS + 1][MAX_SYSTEMS];

/* a card's logo: theme/logos/<console folder>.png, or <collection or tool name>.png */
#define LOGO_DIR SDCARD "/theme/logos"
/* collections without a logo get their name in this font, coloured like the logos */
#define LOGO_FONT SDCARD "/theme/fonts/Montserrat-BlackItalic.ttf"

static font_t *f_logo;
static int f_logo_tried;

/* the drawn names' colours, top to bottom: the Set 2 logos' gradient, or a metallic
 * gray (Settings) */
typedef struct { int at; uint32_t c; } gstop_t;
static gstop_t grad_logo[5] = {   /* the theme's lettering (theme_apply fills it in) */
   { 0, 0xc07dff }, { 35, 0xff7fe6 }, { 55, 0xff5a8f }, { 75, 0xff944c }, { 100, 0xffd816 },
};
static const gstop_t grad_gray[5] = {
   { 0, 0xf4f4f8 }, { 35, 0xd2d2dc }, { 55, 0xb4b4c0 }, { 75, 0x93939f }, { 100, 0x74747f },
};
static const gstop_t grad_red[5] = {   /* a warning: the "No Games Found" card */
   { 0, 0xe02a2e }, { 35, 0xc81a24 }, { 55, 0xae1019 }, { 75, 0x8c0a12 }, { 100, 0x5e050b },
};
static const gstop_t *grad = grad_logo;   /* the one in use for the card being drawn */
static uint32_t grad_edge = 0xff5a1ec6;   /* set from the theme before each use */
#define C_LET_EDGE TH(TH_LET_EDGE)

static uint32_t logo_gradient(int y, int h)
{
   const gstop_t *stops = grad;
   int t = h > 1 ? y * 100 / (h - 1) : 0, i;
   for (i = 1; i < 5 && t > stops[i].at; i++)
      ;
   if (i > 4)
      i = 4;
   {
      int a = stops[i - 1].at, b = stops[i].at, f = b > a ? (t - a) * 256 / (b - a) : 0;
      uint32_t c0 = stops[i - 1].c, c1 = stops[i].c, r, g, bl;
      if (f < 0)
         f = 0;
      if (f > 256)
         f = 256;
      r  = ((c0 >> 16 & 255) * (256 - f) + (c1 >> 16 & 255) * f) >> 8;
      g  = ((c0 >> 8 & 255) * (256 - f) + (c1 >> 8 & 255) * f) >> 8;
      bl = ((c0 & 255) * (256 - f) + (c1 & 255) * f) >> 8;
      return 0xff000000 | r << 16 | g << 8 | bl;
   }
}
#define LOGO_EDGE grad_edge
#define LOGO_LINE_H 46   /* letter height of the drawn names (what FAVORITES gets across a card) */

static uint32_t mix(uint32_t dst, uint32_t src, unsigned a)
{
   unsigned ia = 255 - a;
   return 0xff000000 | (((src >> 16 & 255) * a + (dst >> 16 & 255) * ia) / 255) << 16
        | (((src >> 8 & 255) * a + (dst >> 8 & 255) * ia) / 255) << 8
        | ((src & 255) * a + (dst & 255) * ia) / 255;
}

/* the name, uppercase, in the logo font: its coverage (in the blue channel), trimmed
 * and fitted into box_w x box_h; NULL without the font */
static int two_lines;   /* the last gradient_mask() split the name onto two lines */

static image_t *gradient_mask(const char *name, int box_w, int box_h)
{
   char up[96];
   canvas_t m;
   image_t full, *fit;
   int i, tw, th, x0, x1, y0, y1, w, h;

   if (!f_logo_tried)
   {
      f_logo_tried = 1;
      if (access(LOGO_FONT, F_OK) == 0)
         f_logo = font_load(LOGO_FONT, 96);
   }
   two_lines = 0;
   if (!f_logo)
      return NULL;
   for (i = 0; name[i] && i < (int)sizeof(up) - 1; i++)
      up[i] = (char)toupper((unsigned char)name[i]);
   up[i] = 0;

   {
      /* a long name of several words (one line would come out under ~40% of the box
       * height) goes on two lines, split at the space nearest the middle */
      char l1[96], l2[96] = "";
      int fh = font_height(f_logo), w1, w2 = 0, two = 0, best = -1;
      snprintf(l1, sizeof(l1), "%s", up);
      if (strchr(up, ' ') && (long)font_text_width(f_logo, up) * box_h > (long)fh * 0.72 * box_w * 2.5)
      {
         int len = (int)strlen(up), k;
         for (k = 0; k < len; k++)
            if (up[k] == ' ' && (best < 0 || abs(k - len / 2) < abs(best - len / 2)))
               best = k;
         snprintf(l1, sizeof(l1), "%.*s", best, up);
         snprintf(l2, sizeof(l2), "%s", up + best + 1);
         two = 1;
      }
      w1 = font_text_width(f_logo, l1);
      if (two)
         w2 = font_text_width(f_logo, l2);
      /* white on black at a large size: the brightness is the letters' coverage */
      tw = (w1 > w2 ? w1 : w2) + 40;
      th = fh * (two ? 2 : 1) + 20 - (two ? fh / 5 : 0);
      m.w = m.stride = tw;
      m.h = th;
      if (!(m.px = malloc((size_t)tw * th * 4)))
         return NULL;
      gfx_fill_rect(&m, 0, 0, tw, th, 0xff000000);
      gfx_draw_text(&m, f_logo, l1, tw / 2 - w1 / 2, 10, 0xffffffff, 0);
      if (two)
      {
         gfx_draw_text(&m, f_logo, l2, tw / 2 - w2 / 2, 10 + fh - fh / 5, 0xffffffff, 0);
         two_lines = 1;
      }
   }

   /* trim to the letters */
   x0 = tw; x1 = -1; y0 = th; y1 = -1;
   for (h = 0; h < th; h++)
      for (w = 0; w < tw; w++)
         if (m.px[(size_t)h * tw + w] & 0xff)
         {
            if (w < x0) x0 = w;
            if (w > x1) x1 = w;
            if (h < y0) y0 = h;
            if (h > y1) y1 = h;
         }
   if (x1 < 0)
   {
      free(m.px);
      return NULL;
   }
   full.w = tw;
   full.h = th;
   full.px = m.px;
   w = x1 - x0 + 1;
   h = y1 - y0 + 1;
   /* fit: scale the trimmed letters down into the box, keeping their shape */
   {
      /* every name gets the same letter height (LOGO_LINE_H per line), smaller only
       * if it wouldn't fit across the card */
      int fh = two_lines ? LOGO_LINE_H * 2 + LOGO_LINE_H / 3 : LOGO_LINE_H, fw = w * fh / h;
      image_t crop;
      (void)box_h;   /* the height comes from LOGO_LINE_H now */
      if (fw > box_w)
      {
         fw = box_w;
         fh = h * box_w / w;
      }
      crop.w = w;
      crop.h = h;
      if (!(crop.px = malloc((size_t)w * h * 4)))
      {
         free(m.px);
         return NULL;
      }
      for (i = 0; i < h; i++)
         memcpy(crop.px + (size_t)i * w, full.px + (size_t)(y0 + i) * tw + x0, (size_t)w * 4);
      free(m.px);
      fit = image_scaled(&crop, fw, fh);
      free(crop.px);
   }
   return fit;
}

/* the height of one line of the lettering: edge and outline go by this, so every card
 * matches whether its name is on one line or two */
static int gradient_line_h(const image_t *fit)
{
   return two_lines ? fit->h * 3 / 7 : fit->h;
}

static int gradient_edge(const image_t *fit)
{
   return gradient_line_h(fit) / 16 + 2;
}

/* the white outline's thickness */
static int gradient_outline(const image_t *fit)
{
   return gradient_line_h(fit) / 60 + 1;
}

/* how tall the lettering is on the card, outline and edge included */
static int gradient_height(const image_t *fit)
{
   return fit->h + 2 * gradient_outline(fit) + gradient_edge(fit);
}

/* the lettering glows (the warning card): a soft red halo behind it, like a lit sign */
static int glow_on;
#define GLOW_R 14
#define GLOW_COLOUR 0xffff6a6a
#define GLOW_GAIN 3

/* one box blur along rows (horiz) or columns, radius r */
static void blur_pass(const unsigned char *in, unsigned char *out, int w, int h, int r, int horiz)
{
   int n = horiz ? w : h, lines = horiz ? h : w, line, i;
   for (line = 0; line < lines; line++)
   {
      int sum = 0;
      for (i = 0; i <= r && i < n; i++)
         sum += in[horiz ? line * w + i : i * w + line];
      for (i = 0; i < n; i++)
      {
         int add = i + r + 1, del = i - r;
         out[horiz ? line * w + i : i * w + line] = (unsigned char)(sum / (2 * r + 1));
         if (add < n)
            sum += in[horiz ? line * w + add : add * w + line];
         if (del >= 0)
            sum -= in[horiz ? line * w + del : del * w + line];
      }
   }
}

/* the halo, drawn under the lettering whose coverage is in the picture's blue channel */
static void draw_glow(canvas_t *cv, const image_t *fit, int ox, int oy)
{
   int R = GLOW_R, gw = fit->w + 2 * R, gh = fit->h + 2 * R, x, y, pass;
   unsigned char *a = calloc((size_t)gw * gh, 1), *b = malloc((size_t)gw * gh);
   if (!a || !b)
   {
      free(a);
      free(b);
      return;
   }
   for (y = 0; y < fit->h; y++)
      for (x = 0; x < fit->w; x++)
         a[(size_t)(y + R) * gw + x + R] = (unsigned char)(fit->px[(size_t)y * fit->w + x] & 0xff);
   for (pass = 0; pass < 2; pass++)
   {
      blur_pass(a, b, gw, gh, R / 2, 1);
      blur_pass(b, a, gw, gh, R / 2, 0);
   }
   for (y = 0; y < gh; y++)
      for (x = 0; x < gw; x++)
      {
         unsigned v = a[(size_t)y * gw + x] * GLOW_GAIN;
         int px = ox - R + x, py = oy - R + y;
         if (!v || px < 0 || py < 0 || px >= cv->w || py >= cv->h)
            continue;
         if (v > 120)
            v = 120;
         cv->px[(size_t)py * cv->stride + px] = mix(cv->px[(size_t)py * cv->stride + px], GLOW_COLOUR, v);
      }
   free(a);
   free(b);
}

/* Like the Set 2 logos, from the back: a purple-blue 3D edge (down and right), a white
 * outline (the letters thickened), then the letters in the gradient. top is where the
 * outline starts. */
/* A shadow cast onto a card, made when the card is built (so free while scrolling): a mask (0..255, mw x mh, its top left at
 * x, y on the canvas) moved by dx, dy (down and right: the light is at the top left, as in card_depth), softened by two
 * box blurs of radius blur, and mixed over the card in col at opacity 0..255. */
static void box_blur_u8(unsigned char *m, int w, int h, int rad)
{
   unsigned char *tmp = malloc((size_t)w * h);
   int x, y, pass;
   if (!tmp || rad < 1)
   {
      free(tmp);
      return;
   }
   for (pass = 0; pass < 2; pass++)
   {
      for (y = 0; y < h; y++)   /* horizontal */
      {
         int sum = 0, n = 2 * rad + 1;
         for (x = -rad; x <= rad; x++)
            sum += x >= 0 && x < w ? m[y * w + x] : 0;
         for (x = 0; x < w; x++)
         {
            tmp[y * w + x] = (unsigned char)(sum / n);
            sum += (x + rad + 1 < w ? m[y * w + x + rad + 1] : 0) - (x - rad >= 0 ? m[y * w + x - rad] : 0);
         }
      }
      for (x = 0; x < w; x++)   /* vertical */
      {
         int sum = 0, n = 2 * rad + 1;
         for (y = -rad; y <= rad; y++)
            sum += y >= 0 && y < h ? tmp[y * w + x] : 0;
         for (y = 0; y < h; y++)
         {
            m[y * w + x] = (unsigned char)(sum / n);
            sum += (y + rad + 1 < h ? tmp[(y + rad + 1) * w + x] : 0) - (y - rad >= 0 ? tmp[(y - rad) * w + x] : 0);
         }
      }
   }
   free(tmp);
}

static void cast_mask_shadow(canvas_t *cv, const unsigned char *mask, int mw, int mh, int x, int y, int dx, int dy, int blur,
                             uint32_t col, int opacity)
{
   int pad = blur * 2 + 2, bw = mw + 2 * pad, bh = mh + 2 * pad, i, j;
   unsigned char *m = calloc((size_t)bw * bh, 1);
   if (!m)
      return;
   for (j = 0; j < mh; j++)
      memcpy(m + (size_t)(j + pad) * bw + pad, mask + (size_t)j * mw, (size_t)mw);
   box_blur_u8(m, bw, bh, blur);
   for (j = 0; j < bh; j++)
   {
      int cy = y - pad + dy + j;
      if (cy < 0 || cy >= cv->h)
         continue;
      for (i = 0; i < bw; i++)
      {
         int cx = x - pad + dx + i;
         unsigned a = (unsigned)m[j * bw + i] * opacity / 255;
         if (a > 0 && cx >= 0 && cx < cv->w)
            cv->px[(size_t)cy * cv->stride + cx] = mix(cv->px[(size_t)cy * cv->stride + cx], col, a);
      }
   }
   free(m);
}

static uint32_t letter_shadow_col;   /* not 0: draw_gradient_mask casts a shadow of the letters in this colour (the cards' words) */

static void draw_gradient_mask(canvas_t *cv, const image_t *fit, int cx, int top, int two)
{
   int p = gradient_outline(fit), edge = gradient_edge(fit);
   int dw = fit->w + 2 * p, dh = fit->h + 2 * p, x, y, i, j;
   int ox = cx - fit->w / 2, oy = top + p;
   unsigned char *grow = calloc((size_t)dw * dh, 1);

   if (glow_on)
      draw_glow(cv, fit, ox, oy);
   if (grow)
   {
      /* the letters thickened by p pixels (round, so corners stay smooth) */
      for (y = 0; y < fit->h; y++)
         for (x = 0; x < fit->w; x++)
         {
            unsigned a = fit->px[(size_t)y * fit->w + x] & 0xff;
            if (!a)
               continue;
            for (j = -p; j <= p; j++)
               for (i = -p; i <= p; i++)
               {
                  unsigned char *g;
                  if (i * i + j * j > p * p + p)
                     continue;
                  g = &grow[(size_t)(y + p + j) * dw + x + p + i];
                  if (*g < a)
                     *g = (unsigned char)a;
               }
         }
      if (letter_shadow_col)
         cast_mask_shadow(cv, grow, dw, dh, ox - p, oy - p, edge + 3, edge + 7, 4, letter_shadow_col, 190);
      for (y = 0; y < dh; y++)
         for (x = 0; x < dw; x++)
         {
            unsigned a = grow[(size_t)y * dw + x];
            int ex = ox - p + x + edge, ey = oy - p + y + edge;
            if (a && ex >= 0 && ex < cv->w && ey >= 0 && ey < cv->h)
               cv->px[(size_t)ey * cv->stride + ex] = mix(cv->px[(size_t)ey * cv->stride + ex], LOGO_EDGE, a);
         }
      for (y = 0; y < dh; y++)
         for (x = 0; x < dw; x++)
         {
            unsigned a = grow[(size_t)y * dw + x];
            int wx = ox - p + x, wy = oy - p + y;
            if (a && wx >= 0 && wx < cv->w && wy >= 0 && wy < cv->h)
               cv->px[(size_t)wy * cv->stride + wx] = mix(cv->px[(size_t)wy * cv->stride + wx], 0xffffffff, a);
         }
      free(grow);
   }
   for (y = 0; y < fit->h; y++)
   {
      /* each line gets the whole gradient */
      int half = fit->h / 2;
      uint32_t col = two ? logo_gradient(y < half ? y : y - half, half) : logo_gradient(y, fit->h);
      for (x = 0; x < fit->w; x++)
      {
         unsigned a = fit->px[(size_t)y * fit->w + x] & 0xff;
         if (a && ox + x >= 0 && ox + x < cv->w && oy + y < cv->h)
            cv->px[(size_t)(oy + y) * cv->stride + ox + x] = mix(cv->px[(size_t)(oy + y) * cv->stride + ox + x], col, a);
      }
   }
}

/* a drawn folder (a tab, the back, and a lighter front, in the theme's accent) of w x h at x, y; bg is the
 * colour behind it, which the folder's colours are mixed from */
static uint32_t mix_colour(uint32_t a, uint32_t b, int t256)
{
   uint32_t out = 0xff000000;
   int k;
   for (k = 0; k <= 16; k += 8)
      out |= ((((a >> k) & 0xff) * (256 - t256) + ((b >> k) & 0xff) * t256) >> 8) << k;
   return out;
}

/* like fill_round_rect, but see-through: alpha 0..255 (for the soft shadow) */
/* a rounded rectangle shaded left to right (one column at a time, like the smooth vertical one) */
static void fill_round_rect_hgrad(canvas_t *c, int x, int y, int w, int h, int r, uint32_t left_col, uint32_t right_col)
{
   canvas_t sub = *c;
   int i;
   for (i = 0; i < w; i++)
   {
      if (x + i < 0 || x + i >= c->w)
         continue;
      sub.px = c->px + x + i;
      sub.w = 1;
      fill_round_rect(&sub, -i, y, w, h, r, mix_colour(left_col, right_col, w > 1 ? 256 * i / (w - 1) : 0));
   }
}

static void fill_round_rect_alpha(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col, int alpha)
{
   int x, y;
   for (y = y0 < 0 ? 0 : y0; y < y0 + h && y < c->h; y++)
   {
      float py = y + 0.5f, dy = 0;
      int mid0 = x0 + r, mid1 = x0 + w - r - 1;   /* the straight middle: no horizontal distance */
      if (py < y0 + r)
         dy = y0 + r - py;
      else if (py > y0 + h - r)
         dy = py - (y0 + h - r);
      for (x = x0 < 0 ? 0 : x0; x < x0 + w && x < c->w; x++)
      {
         float px = x + 0.5f, dx = 0, cov;
         if (x >= mid0 && x <= mid1)
         {
            /* the middle of the row is one strength all along: worked out once, then only blended */
            int end = mid1 < c->w - 1 ? mid1 : c->w - 1, xx;
            cov = r + 0.5f - dy;
            if (cov > 0)
            {
               int a = (int)((cov > 1 ? 1.0f : cov) * alpha / 255.0f * 256);
               uint32_t *d = &c->px[(size_t)y * c->stride], rb = (col & 0xff00ff) * a, g = (col & 0x00ff00) * a;
               for (xx = x; xx <= end; xx++)
                  d[xx] = 0xff000000 | (((d[xx] & 0xff00ff) * (256 - a) + rb) >> 8 & 0xff00ff) |
                          (((d[xx] & 0x00ff00) * (256 - a) + g) >> 8 & 0x00ff00);
            }
            x = end;
            continue;
         }
         if (px < x0 + r)
            dx = x0 + r - px;
         else if (px > x0 + w - r)
            dx = px - (x0 + w - r);
         cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
         if (cov <= 0)
            continue;
         fill_px(c, x, y, col, (cov > 1 ? 1.0f : cov) * alpha / 255.0f);
      }
   }
}

/* a rounded rectangle with a vertical gradient from top_col to bot_col, drawn in bands (each band is the
 * whole shape clipped to a few rows, so the corners stay smooth) */
static void fill_round_rect_vgrad(canvas_t *c, int x, int y, int w, int h, int r, uint32_t top_col, uint32_t bot_col)
{
   int bands = h < 24 ? 4 : h / 6, b;
   canvas_t sub = *c;
   if (bands < 2)
      bands = 2;
   for (b = 0; b < bands; b++)
   {
      int y0 = y + h * b / bands, y1 = y + h * (b + 1) / bands;
      if (y1 <= y0 || y0 >= c->h || y1 <= 0)
         continue;
      sub.px = c->px + (size_t)(y0 < 0 ? 0 : y0) * c->stride;
      sub.h = y1 - (y0 < 0 ? 0 : y0);
      fill_round_rect(&sub, x, y - (y0 < 0 ? 0 : y0), w, h, r, mix_colour(top_col, bot_col, 256 * b / (bands - 1)));
   }
}

/* A folder: a soft shadow, the back with its tab, two sheets of paper sticking out, and the front, which is
 * lighter at the top (with a bright edge) and deeper at the bottom. The colours come from the theme's
 * accent mixed into bg, the colour behind it. */
static void draw_folder_shape(canvas_t *c, int x, int y, int w, int h, uint32_t bg)
{
   uint32_t back_top = mix_colour(bg, C_ACCENT, 150), back_bot = mix_colour(bg, C_ACCENT, 105);
   uint32_t front_top = mix_colour(bg, C_ACCENT, 225), front_bot = mix_colour(bg, C_ACCENT, 165);
   uint32_t edge = mix_colour(front_top, 0xffffffff, 70);
   uint32_t paper_back = mix_colour(0xffffffff, C_ACCENT, 105), paper_front = mix_colour(0xffffffff, C_ACCENT, 60);
   int r = h * 11 / 104, tab_w = w * 56 / 140, tab_h = h * 30 / 104, step = h * 14 / 104, lower = h * 30 / 104;
   int pr = r / 2 > 2 ? r / 2 : 2, edge_h = h / 70 + 1;
   canvas_t sub;

   if (h < 40)
   {
      /* small (a list row's thumbnail): no shadow or papers, which only blur at this size; a darker back with a clear
       * tab, and a bright front, so it still reads as a folder */
      uint32_t sback = mix_colour(bg, C_ACCENT, 135), sfront_top = mix_colour(bg, C_ACCENT, 250), sfront_bot = mix_colour(bg, C_ACCENT, 190);
      int sr = h / 8 > 2 ? h / 8 : 2, stab_h = h * 36 / 100, sstep = h * 14 / 100, slower = h * 32 / 100;
      fill_round_rect(c, x, y, w * 46 / 100, stab_h + 2, sr, sback);                 /* the tab */
      fill_round_rect(c, x, y + sstep, w, h - sstep, sr, sback);                      /* the back */
      fill_round_rect_vgrad(c, x, y + slower, w, h - slower, sr, sfront_top, sfront_bot);   /* the front */
      return;
   }

   /* the shadow: two soft, see-through layers a little below and around it (in proportion to the folder's size) */
   {
      int e1 = h * 5 / 196 > 1 ? h * 5 / 196 : 1, e2 = h * 11 / 196 > 2 ? h * 11 / 196 : 2;
      fill_round_rect_alpha(c, x - e1, y + step + h / 22, w + 2 * e1, h - step + 2, r + e1, 0xff000000, 22);
      fill_round_rect_alpha(c, x - e2, y + step + h / 14, w + 2 * e2, h - step, r + e2, 0xff000000, 14);
   }
   /* the back and its tab */
   fill_round_rect_vgrad(c, x, y, tab_w, tab_h + step, h * 9 / 104, back_top, back_bot);
   fill_round_rect_vgrad(c, x, y + step, w, h - step, r, back_top, back_bot);
   /* two sheets, the one behind sticking out further, narrower than the folder */
   fill_round_rect(c, x + w * 9 / 100, y + step - h / 20, w * 82 / 100, h / 2, pr, paper_back);
   fill_round_rect(c, x + w * 15 / 100, y + step + h / 40, w * 70 / 100, h / 2, pr, paper_front);
   /* the front, lighter at the top with a bright edge along it */
   fill_round_rect_vgrad(c, x, y + lower, w, h - lower, r, front_top, front_bot);
   sub = *c;
   sub.px = c->px + (size_t)(y + lower < 0 ? 0 : y + lower) * c->stride;
   sub.h = edge_h;
   if (y + lower >= 0 && y + lower < c->h)
      fill_round_rect(&sub, x, 0, w, h - lower, r, edge);
}

static image_t *card_logo(int r, int i)
{
   char path[300];
   if (r == ROW_CONS && !nvisible)
      return NULL;
   if (r == ROW_CONS)
      snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, systems[visible[i]].id);
   else if (r == ROW_COLL)
      snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, colls[i].name);
   else if (r == ROW_MEDIA)
      snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, i < mf.n ? mf.name[i] : mf.video ? "Other Videos" : "Other Music");
   else
      snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, tool_names[i]);
   return access(path, F_OK) == 0 ? image_load(path) : NULL;
}

/* Cards with a logo are drawn in a middle tone of the theme's colour instead of the dark card: most logos are dark or
 * mid-coloured (navy, red) and sank into the dark card, but some are white (Game Gear, PlayStation ...) and would sink
 * into a light one. A relative luminance of about 0.24 is where the darkest logo and white have about the same contrast
 * (3 to 4 : 1). tone_card() mixes the theme's colour towards white until it gets there. */
static float lum_lin(int v)
{
   float c = v / 255.0f;
   return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static float rel_lum(uint32_t c)
{
   return 0.2126f * lum_lin((c >> 16) & 0xff) + 0.7152f * lum_lin((c >> 8) & 0xff) + 0.0722f * lum_lin(c & 0xff);
}

static uint32_t tone_card(uint32_t base, float target)
{
   int lo = 0, hi = 256, t;
   while (hi - lo > 1)
   {
      t = (lo + hi) / 2;
      if (rel_lum(mix_colour(base, 0xffffffff, t)) < target)
         lo = t;
      else
         hi = t;
   }
   return mix_colour(base, 0xffffffff, lo);
}

/* Depth for a card image (made once, cached with the card, so it costs nothing while scrolling). The card is lit as
 * a pillow: its height rises from the edge over PILLOW_W pixels (a quarter-sine profile) to a flat top, so the
 * surface tilts most at the edge and not at all in the middle. A light from the top left (like Photoshop's angle and
 * altitude) gives each pixel a diffuse value from its tilt, and a "gloss contour" (a wavy curve, as in Photoshop's bevel
 * styles) turns that smooth ramp into rings of light and dark that follow the card's outline. The result is one table
 * for the whole card size, worked out once, and applied to each card as a mix towards white (+) or dark (-).
 * strength 0..256 scales it (the dark cards get less than the mid-tone ones, where it shows more). */
#define PILLOW_SLOPE  1.6f    /* the surface's tilt at the edge (it falls off with the square of the way in, so the middle is gently domed) */
#define PILLOW_LIGHT  115     /* the brightest part's mix towards white, of 256 */
#define PILLOW_SHADE  195     /* the darkest part's mix towards dark, of 256 */
static short *pillow_map;     /* CARD_W x CARD_H, made on first use */

/* The height is a dome: it rises from the card's outline all the way to the middle (no flat top, which looked pressed
 * against the screen). With q = the position from the centre as a share of the card's half size, the tilt of the surface
 * is (sign(x) * qx^2, sign(y) * qy^2) * PILLOW_SLOPE: steepest at the edge, gentle but never zero across the face, and a
 * smooth polynomial everywhere (no normalising, no creases or rings from a changing direction). A light from the top left
 * gives each pixel a diffuse value from the tilt, and that is the whole shading: one smooth gradient with no rings drawn
 * in, the rings are just the lines of equal light. */
static short *pillow_build(int W, int H)
{
   const float a = W / 2.0f, b = H / 2.0f;
   float lx = -0.30f, ly = -0.55f, lz = 0.78f, ln = sqrtf(lx * lx + ly * ly + lz * lz), d0;
   int x, y;
   short *map = malloc((size_t)W * H * sizeof(short));
   if (!map)
      return NULL;
   lx /= ln;
   ly /= ln;
   lz /= ln;
   d0 = lz;   /* a level surface's value */
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float qx = (x + 0.5f - a) / a, qy = (y + 0.5f - b) / b;
         float nx = (qx < 0 ? -1.0f : 1.0f) * qx * qx * PILLOW_SLOPE, ny = (qy < 0 ? -1.0f : 1.0f) * qy * qy * PILLOW_SLOPE;
         float nn = sqrtf(nx * nx + ny * ny + 1.0f), dif = (nx * lx + ny * ly + lz) / nn, t, v;
         dif = dif < 0 ? 0 : dif;
         /* -1 (darkest) .. 0 (a level surface) .. +1 (brightest) */
         t = dif >= d0 ? (dif - d0) / (1.0f - d0) : (dif - d0) / d0;
         v = t > 1 ? 1 : t < -1 ? -1 : t;
         map[y * W + x] = (short)(v >= 0 ? v * PILLOW_LIGHT : v * PILLOW_SHADE);
      }
   return map;
}

static void pillow_make(void)
{
   pillow_map = pillow_build(CARD_W, CARD_H);
}

/* the same shading on a game card (square) */
static short *pillow_sq;

static void card_depth_sq(canvas_t *cv, uint32_t body_bot, int strength)
{
   int x, y;
   uint32_t dark = mix_colour(body_bot, 0xff000000, 170);
   if (!pillow_sq)
      pillow_sq = pillow_build(GS, GS);
   if (!pillow_sq || cv->w != GS || cv->h != GS)
      return;
   for (y = 0; y < cv->h; y++)
   {
      uint32_t *row = cv->px + (size_t)y * cv->stride;
      const short *m = pillow_sq + (size_t)y * GS;
      for (x = 0; x < cv->w; x++)
      {
         int a = m[x] * strength / 256;
         if (a > 0)
            row[x] = mix_colour(row[x], 0xffffffff, a);
         else if (a < 0)
            row[x] = mix_colour(row[x], dark, -a);
      }
   }
}

static void card_depth(canvas_t *cv, uint32_t body_top, uint32_t body_bot, int strength)
{
   int x, y;
   uint32_t dark = mix_colour(body_bot, 0xff000000, 170);
   if (!pillow_map)
      pillow_make();
   if (!pillow_map || cv->w != CARD_W || cv->h != CARD_H)
      return;
   (void)body_top;
   for (y = 0; y < cv->h; y++)
   {
      uint32_t *row = cv->px + (size_t)y * cv->stride;
      const short *m = pillow_map + (size_t)y * CARD_W;
      for (x = 0; x < cv->w; x++)
      {
         int a = m[x] * strength / 256;
         if (a > 0)
            row[x] = mix_colour(row[x], 0xffffffff, a);
         else if (a < 0)
            row[x] = mix_colour(row[x], dark, -a);
      }
   }
}

/* A logo's shadow on the card: its alpha, shrunk to the size it's drawn at, as the shadow's shape (see cast_mask_shadow). */
static void cast_logo_shadow(canvas_t *cv, const image_t *logo, int x, int y, int w, int h, int dx, int dy, int blur,
                             uint32_t col, int opacity)
{
   unsigned char *m = malloc((size_t)w * h);
   int i, j;
   if (!m)
      return;
   for (j = 0; j < h; j++)
      for (i = 0; i < w; i++)
         m[(size_t)j * w + i] = (unsigned char)(logo->px[(size_t)(j * logo->h / h) * logo->w + i * logo->w / w] >> 24);
   cast_mask_shadow(cv, m, w, h, x, y, dx, dy, blur, col, opacity);
   free(m);
}

/* ------------------------------------------------------------- borders around games
 * A game that isn't 16:9 leaves bars at the sides of the screen. Just before a game starts the menu paints a backdrop for
 * the whole screen into /tmp/g36-bezel.raw: the menu's own background in the current theme, a glow in the accent colour
 * along where the game's edges will be, and the console's logo on a plaque in each bar wide enough. Our RetroArch's video
 * driver (oga_gfx.c) shows the parts of it that lie outside the game picture. The game's shape is guessed from the
 * console (the emulator decides the real one, see retroarch_r): a game that differs a little just covers a bit more of the
 * backdrop, or shows a bit more. Settings -> Borders around games turns it off. File: "G36B", width, height, 4 spare bytes,
 * then width*height 0xAARRGGBB pixels. */
#define BEZEL_FILE "/tmp/g36-bezel.raw"

/* a copy of an image turned a quarter, anticlockwise (so text on it reads upwards) */
static image_t *image_rot_ccw(const image_t *in)
{
   image_t *o = malloc(sizeof(*o));
   int x, y;
   if (!o)
      return NULL;
   o->w  = in->h;
   o->h  = in->w;
   o->px = malloc((size_t)o->w * o->h * 4);
   if (!o->px)
   {
      free(o);
      return NULL;
   }
   for (y = 0; y < in->h; y++)
      for (x = 0; x < in->w; x++)
         o->px[(size_t)(in->w - 1 - x) * o->w + y] = in->px[(size_t)y * in->w + x];
   return o;
}

static double bezel_aspect(const char *id)
{
   if (!strcmp(id, "gba"))
      return 1.5;
   if (!strcmp(id, "gb") || !strcmp(id, "gbc") || !strcmp(id, "gamegear"))
      return 160.0 / 144.0;
   if (!strcmp(id, "ngp") || !strcmp(id, "ngpc"))
      return 20.0 / 19.0;
   if (!strcmp(id, "atarilynx"))
      return 1.6;
   return 4.0 / 3.0;
}

/* the system a game belongs to: roms/<id>/... */
static void bezel_write_rom(const char *rom)
{
   char id[64] = "", tmp[64];
   canvas_t *scr = gfx_canvas(), cv;
   const char *p = strncmp(rom, ROMS "/", sizeof(ROMS)) ? NULL : rom + sizeof(ROMS);
   int W, H, ew, gx0, gx1, bar, x, y, d, side;
   uint32_t *px, accent;
   double t0 = now_s();
   FILE *f;

   if (p)
   {
      size_t n = strcspn(p, "/");
      if (n && n < sizeof(id))
      {
         memcpy(id, p, n);
         id[n] = 0;
      }
   }
   if (!scr || !id[0] || !setting_int("game_borders", 1))
   {
      unlink(BEZEL_FILE);
      return;
   }
   W = scr->w;
   H = scr->h;
   ew = (int)(H * bezel_aspect(id) + 0.5);
   if (ew > W - 16)
   {
      unlink(BEZEL_FILE);   /* no bars to fill */
      return;
   }
   gx0 = (W - ew) / 2;
   gx1 = gx0 + ew;
   bar = gx0;
   if (!bg)
      bg = bg_build(scr, 0);
   if (!bg)
      return;
   px = malloc((size_t)W * H * 4);
   if (!px)
      return;
   cv.w = cv.stride = W;
   cv.h = H;
   cv.px = px;
   for (y = 0; y < H; y++)
      memcpy(px + (size_t)y * W, bg + (size_t)(y + BG_M) * (W + 2 * BG_M) + BG_M, (size_t)W * 4);

   /* the light of the game spilling into the bars: the theme's light colour mixed in, strongest at the game's edge and
    * fading across the bar, plus a thin bright line in the accent colour at the edge itself */
   accent = TH(TH_ACCENT);
   {
      uint32_t spill = mix_colour(TH(TH_BG_LIGHT), accent, 90);
      for (side = 0; side < 2; side++)
         for (d = 0; d < bar; d++)
         {
            float k = 1.0f - (float)d / bar;
            int a = 40 + (int)(150.0f * k * k);
            if (d < 2)
               a = 215 - d * 45;
            x = side ? gx1 + d : gx0 - 1 - d;
            for (y = 0; y < H; y++)
            {
               /* a little less at the very top and bottom, so the bars feel lit from the middle */
               float v = 1.0f - 0.45f * fabsf((float)y / H - 0.5f) * 2.0f;
               int ay = d < 2 ? a : (int)(a * v);
               px[(size_t)y * W + x] = mix_colour(px[(size_t)y * W + x], d < 2 ? accent : spill, ay);
            }
         }
   }

   /* the console's logo on one plaque in the left bar, turned to read upwards so it fits even a narrow bar */
   if (bar >= 44)
   {
      char path[300];
      image_t *logo;
      snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, id);
      if (access(path, F_OK) == 0 && (logo = image_load(path)))
      {
         int t = bar - 18 > 124 ? 124 : bar - 18;               /* the plaque's thickness: across the bar */
         int lh = t - 16, lw = logo->w * lh / logo->h, len, x0, y0;
         image_t *rl;
         if (lw > H - 120)
         {
            lw = H - 120;
            lh = logo->h * lw / logo->w;
         }
         len = lw + 30;                                          /* its length: up the bar */
         x0  = (bar - t) / 2;
         y0  = (H - len) / 2;
         if ((rl = image_rot_ccw(logo)))
         {
            int lx = x0 + (t - lh) / 2, ly = y0 + (len - lw) / 2;
            /* the light is the game window, to the right: the shadows fall to the left, and the plaque is brighter on its
             * right side with a thin bright rim there */
            int yy;
            fill_round_rect_alpha(&cv, x0 - 8, y0 + 1, t, len, 12, 0xff000000, 55);
            fill_round_rect_alpha(&cv, x0 - 4, y0 + 1, t, len, 12, 0xff000000, 90);
            fill_round_rect_hgrad(&cv, x0, y0, t, len, 12, TH(TH_CARDL_BOT), TH(TH_CARDL_TOP));
            for (yy = y0 + 12; yy < y0 + len - 12; yy++)
               fill_px(&cv, x0 + t - 2, yy, 0xffffffff, 0.30f);
            cast_logo_shadow(&cv, rl, lx, ly, lh, lw, -4, 1, 2, mix_colour(TH(TH_CARDL_BOT), 0xff000000, 200), 190);
            gfx_draw_image_scaled(&cv, rl, lx, ly, lh, lw);
            image_free(rl);
         }
         image_free(logo);
      }
   }

   for (y = 0; y < W * H; y++)
      px[y] |= 0xff000000;
   snprintf(tmp, sizeof(tmp), "%s.tmp", BEZEL_FILE);
   if ((f = fopen(tmp, "wb")))
   {
      uint32_t head[4] = { 0x42363347u /* "G36B" */, (uint32_t)W, (uint32_t)H, 0 };
      int ok = fwrite(head, sizeof(head), 1, f) == 1 && fwrite(px, (size_t)W * H * 4, 1, f) == 1;
      ok = fclose(f) == 0 && ok;
      if (ok)
         rename(tmp, BEZEL_FILE);
      else
         unlink(tmp);
   }
   free(px);
   mlog("borders: %s, game %d px wide, bars %d px: %.0f ms", id, ew, bar, (now_s() - t0) * 1000);
}

/* The same shadow for text drawn on a card: a dark copy a little down and right, then the text itself on top */
static void draw_centered_shadow(canvas_t *cv, font_t *f, const char *t, int cx, int y, uint32_t col, uint32_t shadow)
{
   draw_centered(cv, f, t, cx + 2, y + 3, shadow);
   draw_centered(cv, f, t, cx + 1, y + 2, shadow);
   draw_centered(cv, f, t, cx, y, col);
}

/* a theme change makes every card on screen draw again (20-30 ms each on the handheld): rather than all in one frame, as
 * many as fit in CARD_BUDGET seconds a frame, the rest showing their old picture meanwhile */
#define CARD_BUDGET 0.02
static double card_spent;      /* time spent drawing cards this frame */
static int card_pending;       /* a card was left with its old picture */
static int preview_defer;      /* the theme just changed: the editor's preview shows its wheel one frame before the work */

static cardimg_t *card_image_(int r, int i)
{
   cardimg_t *ci = &card_cache[r][i < MAX_SYSTEMS ? i : 0];
   char name[96], sub[48], key[160];
   canvas_t cv;
   image_t *logo;
   int nh, sh, ty, tw, mid;

   row_item(r, i, name, sizeof(name), sub, sizeof(sub));
   snprintf(key, sizeof(key), "%s|%s", name, sub);
   if (ci->img.px && !strcmp(ci->key, key))
      return ci;
   if (ci->img.px && !ci->key[0] && card_spent > CARD_BUDGET)
   {
      card_pending = 1;   /* a theme change left this one to redo: the old picture stays up until there's time in a frame */
      return ci;
   }
   if (!ci->img.px)
   {
      ci->img.w = CARD_W;
      ci->img.h = CARD_H;
      ci->img.px = malloc((size_t)CARD_W * CARD_H * 4);
      if (!ci->img.px)
         return NULL;
   }
   snprintf(ci->key, sizeof(ci->key), "%s", key);
   cv.w = cv.stride = CARD_W;
   cv.h = CARD_H;
   cv.px = ci->img.px;
   {
      /* the Consoles row's logo cards and the Collections cards are the theme's light card colours (derived from the card colour and accent, see derive_light_cards);
       * the rest (Tools, the players) keep the dark card */
      mid = r == ROW_COLL || (r != ROW_MEDIA && r != ROW_TOOLS && !(r == ROW_CONS && !nvisible));
      uint32_t top = TH(TH_CARD_TOP), bot = TH(TH_CARD_BOT);
      if (mid)
      {
         top = TH(TH_CARDL_TOP);
         bot = TH(TH_CARDL_BOT);
      }
      fill_vgradient(&cv, 0, 0, CARD_W, CARD_H, top, bot);
      card_depth(&cv, top, bot, mid ? 256 : 110);
   }
   /* no subtitle (Settings): the name sits in the middle */
   sh = sub[0] ? font_height(f_mid) + 14 : 0;
   if ((logo = card_logo(r, i)))
   {
      /* the logo (already shrunk to fit 400x120) in place of the name */
      int lw = logo->w > CARD_W - 40 ? CARD_W - 40 : logo->w, lh = logo->h * lw / logo->w, ly;
      if (lh > 120)
      {
         lw = lw * 120 / lh;
         lh = 120;
      }
      ly = (CARD_H - lh - sh) / 2;
      cast_logo_shadow(&cv, logo, CARD_W / 2 - lw / 2, ly, lw, lh, 5, 8, 4, mix_colour(TH(TH_CARD_BOT), 0xff000000, 200), 190);
      gfx_draw_image_scaled(&cv, logo, CARD_W / 2 - lw / 2, ly, lw, lh);
      draw_centered_shadow(&cv, f_mid, sub, CARD_W / 2, ly + lh + 14, 0xffffffff, mix_colour(TH(TH_CARD_BOT), 0xff000000, 130));
      image_free(logo);
   }
   else if ((r != ROW_CONS || !nvisible) && access(LOGO_FONT, F_OK) == 0)
   {
      /* collections and tools without a logo: the name in the logos' style */
      image_t *mask = gradient_mask(name, 400, 90);
      /* Settings and the players' folders are gray; everything else has the logos' colours */
      int gray = (r == ROW_TOOLS && !strcmp(tool_names[i], "Settings")) || r == ROW_MEDIA;
      int warn = r == ROW_CONS;   /* (only the empty library's card gets here) */
      grad = warn ? grad_red : gray ? grad_gray : grad_logo;
      grad_edge = warn ? 0xff2a0205 : gray ? 0xff3b3946 : C_LET_EDGE;
      int lh = mask ? gradient_height(mask) : 0;
      ty = (CARD_H - lh - sh) / 2;
      if (mask)
      {
         glow_on = warn;
         letter_shadow_col = warn ? 0 : mix_colour(TH(TH_CARD_BOT), 0xff000000, 200);
         draw_gradient_mask(&cv, mask, CARD_W / 2, ty, two_lines);
         letter_shadow_col = 0;
         glow_on = 0;
         image_free(mask);
      }
      draw_centered_shadow(&cv, f_mid, sub, CARD_W / 2, ty + lh + 14, mid ? 0xffffffff : C_DIM,
                           mix_colour(TH(TH_CARD_BOT), 0xff000000, 130));
   }
   else
   {
      nh = font_height(f_huge);
      ty = (CARD_H - nh - (sh ? sh - 8 : 0)) / 2;
      tw = font_text_width(f_huge, name);
      if (tw > CARD_W - 40)
         tw = CARD_W - 40;
      gfx_draw_text(&cv, f_huge, name, CARD_W / 2 - tw / 2, ty, C_TEXT, CARD_W - 40);
      draw_centered(&cv, f_mid, sub, CARD_W / 2, ty + nh + 6, C_DIM);
   }
   /* the picture changed: drop the copies made from the old one */
   if (ci->half)
      image_free(ci->half);
   if (ci->scaled)
      image_free(ci->scaled);
   ci->half = image_half(&ci->img);
   ci->scaled = NULL;
   return ci;
}

static cardimg_t *card_image(int r, int i)
{
   double t0 = now_s();
   cardimg_t *ci = card_image_(r, i);
   card_spent += now_s() - t0;
   return ci;
}

static volatile int timing;   /* kill -USR2: log how long frames take */
static double pf[12];          /* where a frame's time goes, summed (timing only): see PF_ */
enum { PF_BG, PF_IMG, PF_BLIT, PF_ROUND, PF_STROKE, PF_LABELS, PF_CARDS, PF_PILL, PF_STATUS, PF_HINTS };
#define PF_T() (timing ? now_s() : 0.0)
#define PF_ADD(k, t0) do { if (timing) pf[k] += now_s() - (t0); } while (0)

/* While the menu sits idle, the cards that haven't been drawn yet are made (one at a time,
 * the ones nearest the selection first), so scrolling never has to make one in the middle of a
 * move: that was a frame of 20 to 30 ms each. 1 if one was made (more may be left). */
static int card_prewarm(void)
{
   int d, rr, side;
   for (d = 0; d <= MAX_SYSTEMS / 2; d++)
      for (rr = 0; rr < NROWS; rr++)
      {
         int r = (cur_row() + rr) % NROWS, n = row_count(r);
         if (d > n / 2)
            continue;
         for (side = -1; side <= 1; side += 2)
         {
            int i = wrap(ui.sel_[r] + side * d, n);
            cardimg_t *ci = &card_cache[r][i < MAX_SYSTEMS ? i : 0];
            if (ci->img.px && ci->key[0])
               continue;
            card_image(r, i);
            return 1;
         }
      }
   return 0;
}

static int smooth_budget;   /* smooth copies allowed this frame, so a stop doesn't hitch */

/* round off a card's corners by putting the background back outside radius r */
/* d with the source at `a` of 256 (0..256) over it, both opaque: the red and blue together, the green on its own */
static inline uint32_t blend256(uint32_t d, uint32_t s, unsigned a)
{
   return 0xff000000 | (((s & 0xff00ff) * a + (d & 0xff00ff) * (256 - a)) >> 8 & 0xff00ff) |
          (((s & 0x00ff00) * a + (d & 0x00ff00) * (256 - a)) >> 8 & 0x00ff00);
}

static void round_corners(canvas_t *c, int x, int y, int w, int h, int r)
{
   int i, j, corner;
   double r1 = r - 1.0;
   if (!bg_cur || r < 2)
      return;
   for (corner = 0; corner < 4; corner++)
   {
      int cx = corner & 1 ? x + w - r : x + r, cy = corner & 2 ? y + h - r : y + r;
      int x0 = corner & 1 ? x + w - r : x, y0 = corner & 2 ? y + h - r : y;
      for (j = y0; j < y0 + r; j++)
      {
         double dy = j + 0.5 - cy;
         int i0 = x0, i1 = x0 + r;
         if (j < 0 || j >= c->h)
            continue;
         if (fabs(dy) < r1)
         {
            /* the part of the row inside the curve is left alone: only the pixels outside it are visited */
            double sx = sqrt(r1 * r1 - dy * dy);
            if (corner & 1)
               i0 = (int)floor(cx + sx - 0.5) > x0 ? (int)floor(cx + sx - 0.5) : x0;
            else
               i1 = (int)ceil(cx - sx - 0.5) + 1 < x0 + r ? (int)ceil(cx - sx - 0.5) + 1 : x0 + r;
         }
         for (i = i0; i < i1; i++)
         {
            double dx = i + 0.5 - cx, d2 = dx * dx + dy * dy;
            unsigned a;
            uint32_t bgp, *dst;
            if (i < 0 || i >= c->w || d2 <= r1 * r1)
               continue;
            /* outside: background; on the edge: a blend, for a smooth curve */
            a = d2 >= (double)r * r ? 255 : (unsigned)((sqrt(d2) - r1) * 255);
            bgp = bg_cur[(size_t)(j + BG_M + bg_dy) * bg_w + i + BG_M + bg_dx] | 0xff000000;
            dst = &c->px[(size_t)j * c->stride + i];
            *dst = a == 255 ? bgp : mix(*dst, bgp, a);
         }
      }
   }
}

/* a thin line just inside the edge of a rounded rectangle, smoothed: the distance to the
 * edge (negative inside) picks the pixels within about a pixel of it */
static void stroke_px(canvas_t *c, int x, int y, uint32_t col, float cov)
{
   uint32_t *d = &c->px[(size_t)y * c->stride + x], o = *d, out = 0xff000000;
   int k, a = cov >= 1 ? 256 : (int)(cov * 256);
   for (k = 0; k <= 16; k += 8)
      out |= (((col >> k & 0xff) * a + (o >> k & 0xff) * (256 - a)) >> 8) << k;
   *d = out;
}

static void stroke_round_rect_scan(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col)
{
   float cx = x0 + w / 2.0f, cy = y0 + h / 2.0f, hx = w / 2.0f - r, hy = h / 2.0f - r;
   int x, y;
   for (y = y0 < 0 ? 0 : y0; y < y0 + h && y < c->h; y++)
   {
      /* rows away from the corners only touch the two sides */
      int mid = y > y0 + r + 2 && y < y0 + h - r - 3;
      for (x = x0 < 0 ? 0 : x0; x < x0 + w && x < c->w; x++)
      {
         float qx, qy, ox, oy, sd, cov;
         if (mid && x > x0 + 3 && x < x0 + w - 4)
         {
            x = x0 + w - 5;
            continue;
         }
         qx = fabsf(x + 0.5f - cx) - hx;
         qy = fabsf(y + 0.5f - cy) - hy;
         ox = qx > 0 ? qx : 0;
         oy = qy > 0 ? qy : 0;
         sd = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
         cov = 1 - (fabsf(sd + 1.0f) - 0.6f) / 0.8f;   /* full for about 1.2 px, fading at both sides */
         if (cov > 1)
            cov = 1;
         if (!mid && x >= x0 + r + 2 && x <= x0 + w - r - 3)
         {
            /* the straight part of the top or bottom edge: the same all along the row (the
             * distance there only depends on the row), so it's worked out once and filled */
            int end = x0 + w - r - 3;
            if (end >= c->w)
               end = c->w - 1;
            if (cov > 0)
            {
               int xx;
               for (xx = x; xx <= end; xx++)
                  stroke_px(c, xx, y, col, cov);
            }
            x = end;   /* (the loop's own step goes on from here) */
            continue;
         }
         if (cov > 0)
            stroke_px(c, x, y, col, cov);
      }
   }
}

/* the outline's coverage at a pixel, from its distances out of the straight part (see stroke_round_rect) */
static inline float stroke_cov(float qx, float qy, int r)
{
   float ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0, sd, cov;
   sd = sqrtf(ox * ox + oy * oy) + (qx > qy ? (qx < 0 ? qx : 0) : (qy < 0 ? qy : 0)) - r;
   cov = 1 - (fabsf(sd + 1.0f) - 0.6f) / 0.8f;   /* full for about 1.2 px, fading at both sides */
   return cov > 1 ? 1 : cov;
}

/* A thin outline just inside the edge. Only the few pixels along it are looked at: on each row, the stretch of the straight top
 * or bottom edge (all alike), and where the curve of a corner or the side passes (found from the radius, not by trying every
 * pixel of the corner). */
static void stroke_round_rect(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col)
{
   float cx = x0 + w / 2.0f, cy = y0 + h / 2.0f, hx = w / 2.0f - r, hy = h / 2.0f - r;
   float lo = r - 2.5f, hi = r + 0.5f;   /* the radii between which the coverage is above 0 */
   int y, x, sn[2] = { 0, 0 }, sx[2][12], k;
   unsigned sa[2][12];
   if (r < 6)
   {
      stroke_round_rect_scan(c, x0, y0, w, h, r, col);
      return;
   }
   {
      /* on the straight sides every row is alike (the coverage only depends on how far in from the edge): worked out once */
      int s, xa, xb;
      for (s = 0; s < 2; s++)
      {
         xa = s == 0 ? (int)floorf(cx - hx - hi - 0.5f) - 1 : (int)floorf(cx + hx + lo - 0.5f) - 1;
         xb = s == 0 ? (int)ceilf(cx - hx - lo - 0.5f) + 1 : (int)ceilf(cx + hx + hi - 0.5f) + 1;
         for (x = xa; x <= xb && sn[s] < 12; x++)
         {
            float qx = fabsf(x + 0.5f - cx) - hx, cov;
            if (qx <= 0 || x < x0 || x >= x0 + w)
               continue;
            cov = stroke_cov(qx, 0.0f, r);
            if (cov > 0)
            {
               sx[s][sn[s]] = x;
               sa[s][sn[s]++] = cov >= 1 ? 256 : (unsigned)(cov * 256);
            }
         }
      }
   }
   for (y = y0 < 0 ? 0 : y0; y < y0 + h && y < c->h; y++)
   {
      float qy = fabsf(y + 0.5f - cy) - hy, qy2 = qy > 0 ? qy * qy : 0, qlo, qhi, covm = 0;
      int s, xa, xb, ma = 1, mb = 0;
      if (qy <= 0)
      {
         uint32_t *row = c->px + (size_t)y * c->stride;
         for (s = 0; s < 2; s++)
            for (k = 0; k < sn[s]; k++)
               if (sx[s][k] >= 0 && sx[s][k] < c->w)
                  row[sx[s][k]] = sa[s][k] == 256 ? 0xff000000 | col : blend256(row[sx[s][k]], col, sa[s][k]);
         continue;
      }
      if (qy > 0)
      {
         if (hi * hi <= qy2)
            continue;
         qlo = lo * lo > qy2 ? sqrtf(lo * lo - qy2) : 0;
         qhi = sqrtf(hi * hi - qy2);
         covm = stroke_cov(-1.0f, qy, r);
         ma = (int)ceilf(cx - hx - 0.5f) - 1;   /* (the straight stretch, with a pixel to spare at each end) */
         mb = (int)floorf(cx + hx - 0.5f) + 1;
      }
      else
      {
         qlo = lo;
         qhi = hi;
      }
      /* the two bands: left (s = 0) and right (s = 1) */
      for (s = 0; s < 2; s++)
      {
         if (s == 0)
         {
            xa = (int)floorf(cx - hx - qhi - 0.5f) - 1;
            xb = (int)ceilf(cx - hx - qlo - 0.5f) + 1;
         }
         else
         {
            xa = (int)floorf(cx + hx + qlo - 0.5f) - 1;
            xb = (int)ceilf(cx + hx + qhi - 0.5f) + 1;
         }
         for (x = xa < 0 ? 0 : xa; x <= xb && x < c->w; x++)
         {
            float qx = fabsf(x + 0.5f - cx) - hx, cov;
            if (qx <= 0 || x < x0 || x >= x0 + w)
               continue;   /* (the straight stretch has its own pass) */
            cov = stroke_cov(qx, qy, r);
            if (cov > 0)
               stroke_px(c, x, y, col, cov);
         }
      }
      if (covm > 0)
      {
         /* the straight stretch: first and last pixel checked one by one, the rest of it all alike (a plain fill when solid) */
         int xs = ma < 0 ? 0 : ma, xe = mb < c->w - 1 ? mb : c->w - 1, fa, fb;
         unsigned a256 = covm >= 1 ? 256 : (unsigned)(covm * 256);
         uint32_t *row = c->px + (size_t)y * c->stride, solid = 0xff000000 | col;
         for (fa = xs; fa <= xe; fa++)
            if (fabsf(fa + 0.5f - cx) - hx <= 0 && fa >= x0 && fa < x0 + w)
               break;
         for (fb = xe; fb >= xs; fb--)
            if (fabsf(fb + 0.5f - cx) - hx <= 0 && fb >= x0 && fb < x0 + w)
               break;
         for (x = fa; x <= fb; x++)
            row[x] = a256 == 256 ? solid : blend256(row[x], col, a256);
      }
   }
}

/* The Video and Music Players' folders without a logo of their own are not a rectangular card but a folder:
 * drawn straight onto the screen (it is only a few rounded rectangles, so it can be any size and its edges
 * are smooth), with the name under it and the count under that. Neighbours are smaller, and dimmer. */
static int media_has_logo(int i)
{
   char path[300];
   snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, i < mf.n ? mf.name[i] : mf.video ? "Other Videos" : "Other Music");
   return access(path, F_OK) == 0;
}

static int folder_progress(int card, char *ep, size_t esz, double *secs, int *fin, int *watched, int *total);
static void fmt_time(double s, char *out, size_t sz);

static void draw_folder_card(canvas_t *c, const card_t *k)
{
   char name[96], sub[48], ep[200] = "", l1[64], l2[240], tail[48];
   double s = k->scale, secs = 0;
   font_t *ft = s >= 0.9 ? f_big : s >= 0.6 ? f_mid : f_small, *fs = s >= 0.9 ? f_mid : f_small;
   int fw = (int)(264 * s), fh = (int)(196 * s), gap = (int)(8 * s + 0.5), tl, sh, total, top, ty, tw, maxw = (int)(CARD_W * s) - 20;
   int bar = s >= 0.6, lh = font_height(f_small), has = 0, fin = 0, watched = 0, n_all = 0, lines = 1;
   uint32_t ink = k->depth < 0.5 ? C_TEXT : C_DIM;

   row_item(k->r, k->i, name, sizeof(name), sub, sizeof(sub));
   tl = font_height(ft);
   if (bar)
   {
      /* the text bar replaces the count: "5/11 videos watched", and under it the video you're on and how far in */
      has = mf.video ? folder_progress(k->i, ep, sizeof(ep), &secs, &fin, &watched, &n_all) : 0;
      /* "Watching 5/11 videos": the place you're at; a folder watched to the end says "Watched"; one not started, how many there are */
      if (!mf.video)
         snprintf(l1, sizeof(l1), "%s", sub);   /* music has no progress: the count, in the same kind of bar */
      else if (!has)
         snprintf(l1, sizeof(l1), "%d video%s", n_all, n_all == 1 ? "" : "s");
      else if (fin)
         snprintf(l1, sizeof(l1), "Watched %d/%d videos", watched, n_all);
      else
         snprintf(l1, sizeof(l1), "Watching %d/%d videos", watched + 1 > n_all ? n_all : watched + 1, n_all);
      l2[0] = tail[0] = 0;
      if (has && fin)
         snprintf(l2, sizeof(l2), "%s", ep[0] ? ep : "");
      else if (has)
      {
         snprintf(l2, sizeof(l2), "%s", ep);
         if (secs > 0)
            fmt_time(secs, tail, sizeof(tail));
         else
            snprintf(tail, sizeof(tail), "up next");
      }
      if (has && fin && ep[0])
         snprintf(l2, sizeof(l2), "Finished %s", ep);
      lines = l2[0] ? 2 : 1;
      sh = lines * lh + (lines - 1) * 3 + 14;
   }
   else
      sh = sub[0] && s >= 0.6 ? font_height(fs) + 2 : 0;
   total = fh + gap + tl + (bar ? gap + sh : sh);
   top = k->cy - total / 2;
   draw_folder_shape(c, k->cx - fw / 2, top, fw, fh, TH(TH_CARD_TOP));
   ty = top + fh + gap;
   tw = font_text_width(ft, name);
   if (tw > maxw)
      tw = maxw;
   gfx_draw_text(c, ft, name, k->cx - tw / 2, ty, ink, maxw);
   if (!bar && sh)
      draw_centered(c, fs, sub, k->cx, ty + tl + 2, C_DIM);
   if (bar)
   {
      int by = ty + tl + gap, bw, x, w1 = font_text_width(f_small, l1), w2 = 0, wt = 0, room;
      uint32_t fill = mix_colour(TH(TH_CARD_TOP), 0xffffffff, 24);
      if (l2[0])
      {
         wt = tail[0] ? font_text_width(f_small, tail) : 0;
         w2 = font_text_width(f_small, l2);
         room = maxw - 28 - (wt ? wt + 16 : 0);
         if (w2 > room)
            w2 = room;
         w2 += wt ? wt + 16 : 0;
      }
      bw = (w1 > w2 ? w1 : w2) + 28;
      x = k->cx - bw / 2;
      fill_round_rect(c, x, by, bw, sh, lines == 2 ? 16 : sh / 2, fill);
      gfx_draw_text(c, f_small, l1, k->cx - w1 / 2, by + 7, has ? C_TEXT : C_DIM, 0);
      if (l2[0])
      {
         int x2 = k->cx - w2 / 2, ew = w2 - (wt ? wt + 16 : 0);
         gfx_draw_text(c, f_small, l2, x2, by + 7 + lh + 3, C_DIM, ew);
         if (wt)
            gfx_draw_text(c, f_small, tail, x2 + ew + 16, by + 7 + lh + 3, C_ACCENT, 0);
      }
   }
}

/* A soft shadow under a card (at the card's current size): a few rows below its bottom edge, fading out, a little narrower
 * than the card so it doesn't show round the corners. Only about 12 rows of the card's width, so it's cheap enough to draw
 * every frame. */
#define CARD_SLAB 10   /* the card's thickness at full size, in pixels */
static void card_shadow(canvas_t *c, int x, int y, int w, int h, int r, int lift)
{
   /* the focused card is lifted: its shadow is deeper and reaches further, so it stands out from the ones beside it.
    * The ends fade out over feather pixels (smoothstep), so there is no hard edge */
   int rows = lift ? 12 + h / 7 : 4 + h / 14, j, xx, x0 = x + r / 2, x1 = x + w - r / 2, base = y + h - 3;
   float feather = (lift ? 70.0f : 26.0f) * w / CARD_W, prof[80];
   int nprof = (int)ceilf(feather) < 80 ? (int)ceilf(feather) : 79;
   for (j = 0; j <= nprof; j++)
   {
      float t = j >= feather ? 1.0f : j / feather;
      prof[j] = t * t * (3.0f - 2.0f * t);   /* the ends' fade-out, by distance from the end */
   }
   for (j = 0; j < rows; j++)
   {
      float f = 1.0f - (float)j / rows;
      int yy = base + j, xs = x0 + j / 3, xe = x1 - j / 3;
      float cov = f * f * (lift ? 0.62f : 0.42f);
      uint32_t *row;
      if (yy < 0 || yy >= c->h)
         continue;
      row = c->px + (size_t)yy * c->stride;
      for (xx = xs < 0 ? 0 : xs; xx < xe && xx < c->w; xx++)
      {
         int d = xx - xs < xe - 1 - xx ? xx - xs : xe - 1 - xx;
         unsigned a = (unsigned)((d >= nprof ? cov : cov * prof[d]) * 256);
         if (d >= nprof)
         {
            /* the stretch between the faded ends: one strength all along */
            int end = xe - nprof < c->w ? xe - nprof : c->w;
            for (; xx < end; xx++)
               row[xx] = 0xff000000 | (((row[xx] & 0xff00ff) * (256 - a) >> 8) & 0xff00ff) | (((row[xx] & 0x00ff00) * (256 - a) >> 8) & 0x00ff00);
            xx--;
            continue;
         }
         row[xx] = 0xff000000 | (((row[xx] & 0xff00ff) * (256 - a) >> 8) & 0xff00ff) | (((row[xx] & 0x00ff00) * (256 - a) >> 8) & 0x00ff00);
      }
   }
}

/* Is this row's card a mid-tone one (see card_image), and what is its bottom colour */
static int card_row_is_mid(int r)
{
   return r == ROW_COLL || (r != ROW_MEDIA && r != ROW_TOOLS);
}

static uint32_t card_row_bottom(int r)
{
   return card_row_is_mid(r) ? TH(TH_CARDL_BOT) : TH(TH_CARD_BOT);
}

/* coverage (0..1) of the pixel centre (px, py) by a rounded rectangle */
static float rr_cov(float px, float py, float x0, float y0, float w, float h, float r)
{
   float qx = fabsf(px - (x0 + w / 2)) - (w / 2 - r), qy = fabsf(py - (y0 + h / 2)) - (h / 2 - r);
   float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0, m = qx > qy ? qx : qy;
   float sd = sqrtf(mx * mx + my * my) + (m < 0 ? m : 0) - r, cov = 0.5f - sd;
   return cov < 0 ? 0 : cov > 1 ? 1 : cov;
}

/* the slab's corner weights (0..255) for one corner radius and depth, kept for the last few sizes in use */
typedef struct
{
   int r, T;
   unsigned char *left, *right;
} slab_maps_t;

static const slab_maps_t *slab_maps(int r, int T)
{
   static slab_maps_t cache[16];
   static int next;
   int i, off = T / 3, rows = r + T, px, py;
   slab_maps_t *m;
   for (i = 0; i < 16; i++)
      if (cache[i].left && cache[i].r == r && cache[i].T == T)
         return &cache[i];
   m = &cache[next++ & 15];
   free(m->left);
   free(m->right);
   m->r = r;
   m->T = T;
   m->left = malloc((size_t)r * rows);
   m->right = malloc((size_t)(r + off) * rows);
   if (!m->left || !m->right)
   {
      free(m->left);
      free(m->right);
      m->left = m->right = NULL;
      return NULL;
   }
   /* a virtual card at (0, 0), 4r wide and 2r high (any width past 2r will do), with the slab at (off, T) */
   for (py = 0; py < rows; py++)
   {
      float yv = r + py + 0.5f;
      for (px = 0; px < r + off; px++)
      {
         float cc, cs, wg;
         if (px < r)
         {
            cc = rr_cov(px + 0.5f, yv, 0, 0, 4.0f * r, 2.0f * r, (float)r);
            cs = rr_cov(px + 0.5f, yv, (float)off, (float)T, 4.0f * r, 2.0f * r, (float)r);
            wg = cs * (1 - cc);
            m->left[py * r + px] = (unsigned char)(wg * 255);
         }
         cc = rr_cov(3.0f * r + px + 0.5f, yv, 0, 0, 4.0f * r, 2.0f * r, (float)r);
         cs = rr_cov(3.0f * r + px + 0.5f, yv, (float)off, (float)T, 4.0f * r, 2.0f * r, (float)r);
         wg = cs * (1 - cc);
         m->right[py * (r + off) + px] = (unsigned char)(wg * 255);
      }
   }
   return m;
}

/* The card's thickness: a slab behind and below it, T pixels deep (and a third of that to the right), so the card is a solid
 * object with an edge and not a sheet. Only the part of the slab the card doesn't cover is painted, after the card, with the
 * rounded corners worked out per pixel; the long straight parts are filled a row at a time. */
static void card_slab(canvas_t *c, int x, int y, int w, int h, int r, int T, uint32_t top_col, uint32_t bot_col)
{
   int off = T / 3, py, px;
   const slab_maps_t *mp;
   if (T < 2)
      return;
   mp = slab_maps(r, T);
   if (!mp)
      return;
   for (py = y + h - r; py < y + h + T; py++)
   {
      uint32_t col = mix_colour(top_col, bot_col, 256 * (py - (y + h) < 0 ? 0 : py - (y + h)) / T);
      if (py < 0 || py >= c->h)
         continue;
      /* the straight middle: the card's edge and the slab's edge are horizontal lines */
      {
         float cc = 0.5f - (py + 0.5f - (y + h)), cs = 0.5f - (py + 0.5f - (y + h + T));
         float wgt;
         cc = cc < 0 ? 0 : cc > 1 ? 1 : cc;
         cs = cs < 0 ? 0 : cs > 1 ? 1 : cs;
         wgt = cs * (1 - cc);
         if (wgt > 0)
         {
            unsigned a256 = (unsigned)(wgt * 255), end = (unsigned)(x + w - r + off < c->w ? x + w - r + off : c->w);
            a256 += a256 >> 7;
            for (px = x + r < 0 ? 0 : x + r; px < (int)end; px++)
               c->px[(size_t)py * c->stride + px] = blend256(c->px[(size_t)py * c->stride + px], col, a256);
         }
      }
      /* the two corners (the right one runs on to the slab's right edge): their weights come from a table kept for this
       * radius and depth (worked out once with the square roots, then only blended) */
      for (px = x; px < x + w + off; px++)
      {
         int idx;
         unsigned wb;
         if (px >= x + r && px < x + w - r + off)
         {
            px = x + w - r + off - 1;
            continue;
         }
         if (px < 0 || px >= c->w)
            continue;
         idx = px < x + r ? (py - (y + h - r)) * r + (px - x) : (py - (y + h - r)) * (r + off) + (px - (x + w - r));
         wb = px < x + r ? mp->left[idx] : mp->right[idx];
         if (wb)
            c->px[(size_t)py * c->stride + px] = blend256(c->px[(size_t)py * c->stride + px], col, wb + (wb >> 7));
      }
   }
   /* the right side, between the corners */
   for (py = y + r; py < y + h - r; py++)
   {
      if (py < 0 || py >= c->h)
         continue;
      for (px = x + w - 1; px < x + w + off + 1; px++)
      {
         float cc = 0.5f - (px + 0.5f - (x + w)), cs = 0.5f - (px + 0.5f - (x + w + off)), wgt;
         cc = cc < 0 ? 0 : cc > 1 ? 1 : cc;
         cs = cs < 0 ? 0 : cs > 1 ? 1 : cs;
         wgt = cs * (1 - cc);
         if (wgt > 0 && px >= 0 && px < c->w)
            c->px[(size_t)py * c->stride + px] = mix(c->px[(size_t)py * c->stride + px], top_col, (unsigned)(wgt * 255));
      }
   }
}

/* The focused card's reflection on a glossy floor: its bottom rows, upside down, under it, fading out. Drawn before the cards,
 * so the row below covers it where they meet. The corners follow the card's rounding. */
static void card_reflection(canvas_t *c, const cardimg_t *ci, int x, int y, int w, int h, int r, int T)
{
   int rows = h / 4, dy, xx, y0 = y + h + T + 4, sw = ci->img.w, sh = ci->img.h;
   for (dy = 0; dy < rows; dy++)
   {
      int yy = y0 + dy, srow = sh - 1 - dy * sh / h, inset = dy < r ? r - (int)sqrtf((float)(r * r - (r - dy) * (r - dy))) : 0;
      float f = 1.0f - (float)dy / rows;
      unsigned a = (unsigned)(f * f * 0.34f * 255), a256 = a + (a >> 7);
      const uint32_t *src;
      uint32_t *dst;
      int xa = x + inset, xb = x + w - inset;
      uint64_t pos, step;
      if (yy < 0 || yy >= c->h || srow < 0)
         continue;
      src = ci->img.px + (size_t)srow * sw;
      dst = c->px + (size_t)yy * c->stride;
      /* the source column for each pixel, stepped along rather than divided out for every pixel */
      step = (((uint64_t)sw << 32) + w - 1) / w;   /* (rounded up, so the whole part is exactly column * sw / w) */
      xx = xa < 0 ? 0 : xa;
      pos = (uint64_t)(xx - x) * step;
      for (; xx < xb && xx < c->w; xx++, pos += step)
         dst[xx] = blend256(dst[xx], src[pos >> 32] | 0xff000000, a256);
   }
}

/* A glint: a soft white diagonal band that sweeps across a card once when it's chosen (from the button press on). p 0..1 is how far along. */
static void card_glint(canvas_t *c, int x, int y, int w, int h, int r, double p)
{
   float e = (float)(p * p * (3 - 2 * p)), half = w * 0.11f, centre0 = x - half * 1.5f + (w + 3 * half) * e;
   float peak = 0.34f * sinf((float)p * 3.14159f);
   int j;
   for (j = 0; j < h; j++)
   {
      int yy = y + j, inset = 0, x0, x1, xx;
      float centre = centre0 + (j - h / 2) * 0.55f;
      if (yy < 0 || yy >= c->h)
         continue;
      if (j < r)
         inset = r - (int)sqrtf((float)(r * r - (r - j) * (r - j)));
      else if (j >= h - r)
         inset = r - (int)sqrtf((float)(r * r - (j - (h - r)) * (j - (h - r))));
      x0 = (int)(centre - half) < x + inset ? x + inset : (int)(centre - half);
      x1 = (int)(centre + half) > x + w - inset ? x + w - inset : (int)(centre + half);
      for (xx = x0; xx < x1; xx++)
      {
         float u = (xx - centre) / half, a = 1 - fabsf(u);
         if (xx >= 0 && xx < c->w && a > 0)
            c->px[(size_t)yy * c->stride + xx] = mix(c->px[(size_t)yy * c->stride + xx], 0xffffffff, (unsigned)(a * a * peak * 255));
      }
   }
}

static void draw_card(canvas_t *c, const card_t *k, int focus)
{
   double t0 = PF_T(), t1;
   cardimg_t *ci;
   int w = (int)(CARD_W * k->scale), h = (int)(CARD_H * k->scale), slab_t;
   if (k->r == ROW_MEDIA && !media_has_logo(k->i))
   {
      draw_folder_card(c, k);
      return;
   }
   ci = card_image(k->r, k->i);
   int x = k->cx - w / 2, y = k->cy - h / 2;
   PF_ADD(PF_IMG, t0);
   t0 = PF_T();

   if (!ci)
      return;
   slab_t = (int)(CARD_SLAB * k->scale + 0.5);
   card_shadow(c, x, y + slab_t, w, h, (int)(28 * k->scale), focus);
   if (w == CARD_W && h == CARD_H)
      gfx_draw_image_smooth(c, &ci->img, x, y, w, h);              /* full size: a copy */
   else if (ci->scaled && ci->scaled->w == w && ci->scaled->h == h)
      gfx_draw_image_smooth(c, ci->scaled, x, y, w, h);            /* resting: the kept copy */
   else if (w == ci->last_w && h == ci->last_h && smooth_budget > 0)
   {
      smooth_budget--;
      /* it has stopped at this size: make the smooth copy once */
      if (ci->scaled)
         image_free(ci->scaled);
      ci->scaled = image_scaled(&ci->img, w, h);
      gfx_draw_image_smooth(c, ci->scaled ? ci->scaled : &ci->img, x, y, w, h);
   }
   else
      /* moving: quick scaling, from the half-size copy when it's small */
      gfx_draw_image_fast(c, ci->half && w <= ci->half->w ? ci->half : &ci->img, x, y, w, h);
   ci->last_w = w;
   ci->last_h = h;
   PF_ADD(PF_BLIT, t0);
   t1 = PF_T();
   round_corners(c, x, y, w, h, (int)(28 * k->scale));
   PF_ADD(PF_ROUND, t1);
   t1 = PF_T();
   /* a thin outline, a little lighter than the background */
   stroke_round_rect(c, x, y, w, h, (int)(28 * k->scale), TH(TH_CARD_LINE));
   PF_ADD(PF_STROKE, t1);
   {
      uint32_t bot = card_row_bottom(k->r);
      card_slab(c, x, y, w, h, (int)(28 * k->scale), slab_t, mix_colour(bot, 0xff000000, 110), mix_colour(bot, 0xff000000, 200));
   }
}

/* a filled rectangle with corners of radius r, smoothed at the edges
 * (r = half the size makes a circle or a pill) */
static void fill_px(canvas_t *c, int x, int y, uint32_t col, float cov)
{
   uint32_t *d = &c->px[(size_t)y * c->stride + x], o = *d, out = 0xff000000;
   int k, a = cov >= 1 ? 256 : (int)(cov * 256);
   for (k = 0; k <= 16; k += 8)
      out |= (((col >> k & 0xff) * a + (o >> k & 0xff) * (256 - a)) >> 8) << k;
   *d = out;
}

static void fill_round_rect(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col)
{
   int x, y;
   for (y = y0 < 0 ? 0 : y0; y < y0 + h && y < c->h; y++)
   {
      /* the row's distance to the rectangle shrunk by r (the same all along its straight middle) */
      float py = y + 0.5f, dy = 0;
      int mid0 = x0 + r, mid1 = x0 + w - r - 1;   /* the middle: no horizontal distance */
      if (py < y0 + r)
         dy = y0 + r - py;
      else if (py > y0 + h - r)
         dy = py - (y0 + h - r);
      for (x = x0 < 0 ? 0 : x0; x < x0 + w && x < c->w; x++)
      {
         /* distance from the pixel centre to the rectangle shrunk by r */
         float px = x + 0.5f, dx = 0, cov;
         if (x >= mid0 && x <= mid1)
         {
            /* the middle: filled as one run (it's solid, or one fixed blend across the row) */
            int end = mid1 < c->w - 1 ? mid1 : c->w - 1;
            cov = r + 0.5f - dy;
            if (cov > 0)
            {
               if (cov >= 1)
               {
                  uint32_t solid = 0xff000000 | (col & 0xffffff), *d = &c->px[(size_t)y * c->stride];
                  int xx;
                  for (xx = x; xx <= end; xx++)
                     d[xx] = solid;
               }
               else
               {
                  int xx;
                  for (xx = x; xx <= end; xx++)
                     fill_px(c, xx, y, col, cov);
               }
            }
            x = end;
            continue;
         }
         if (px < x0 + r)
            dx = x0 + r - px;
         else if (px > x0 + w - r)
            dx = px - (x0 + w - r);
         cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
         if (cov <= 0)
            continue;
         fill_px(c, x, y, col, cov);
      }
   }
}

static int card_cmp(const void *a, const void *b)
{
   const card_t *x = a, *y = b;
   return x->depth < y->depth ? 1 : x->depth > y->depth ? -1 : 0;
}

/* rows loop: distance of row r from the focused position, in -1.5 .. 1.5 */
static double row_offset(int r)
{
   double d = fmod(r - ui.row_anim, NROWS);
   if (d < -NROWS / 2.0)
      d += NROWS;
   if (d >= NROWS / 2.0)
      d -= NROWS;
   return d;
}

static void draw_toast(canvas_t *c, double now)
{
   int tw, x;
   if (!ui.toast[0] || now > ui.toast_until)
      return;
   if (strlen(ui.toast) <= 2)
   {
      /* a letter jump: the letter, big, in the middle of the screen */
      gfx_fill_rect(c, c->w / 2 - 50, c->h / 2 - 50, 100, 100, 0xff2d1a33);
      gfx_fill_rect(c, c->w / 2 - 50, c->h / 2 + 46, 100, 4, C_ACCENT);
      draw_centered(c, f_huge, ui.toast, c->w / 2, c->h / 2 - font_height(f_huge) / 2, C_TEXT);
      return;
   }
   tw = font_text_width(f_mid, ui.toast);
   x = c->w / 2 - tw / 2 - 20;
   fill_round_rect(c, x - 2, 370 - popup_lift, tw + 44, 48, 24, C_ACCENT);   /* a thin accent rim round a rounded pill */
   fill_round_rect(c, x, 372 - popup_lift, tw + 40, 44, 22, C_PANEL);
   draw_centered(c, f_mid, ui.toast, c->w / 2, 380 - popup_lift, C_TEXT);
}

/* above a row: its name, underlined; below: where in the row you are, as dots */
/* A translucent pill: the position dots barely move (the row title went back to a solid pill), so they are made once as a small picture with its own
 * see-through (the shadow outside, a dark or coloured edge, the fill at fill_a of full strength with a highlight along the
 * top) and then only blended over whatever is behind them, which now shows through: the floor, the stars, the cards of
 * the next row. The picture is made again only when its size or colours change. */
typedef struct
{
   image_t img;
   char key[96];
} pillspr_t;

#define PILL_PAD 14
static void pill_sprite(pillspr_t *sp, int w, int h, int edge, uint32_t edge_col, float edge_a, uint32_t fill, float fill_a, float hi_a, float shadow_a)
{
   int sw = w + 2 * PILL_PAD, sh = h + 2 * PILL_PAD + 6, x, y, r = h / 2;
   if (sp->img.w != sw || sp->img.h != sh || !sp->img.px)
   {
      free(sp->img.px);
      sp->img.px = malloc((size_t)sw * sh * 4);
      sp->img.w = sw;
      sp->img.h = sh;
   }
   if (!sp->img.px)
      return;
   for (y = 0; y < sh; y++)
      for (x = 0; x < sw; x++)
      {
         float px = x + 0.5f, py = y + 0.5f;
         float outer = rr_cov(px, py, (float)(PILL_PAD - edge), (float)(PILL_PAD - edge), (float)(w + 2 * edge), (float)(h + 2 * edge), (float)(r + edge));
         float inner = rr_cov(px, py, (float)PILL_PAD, (float)PILL_PAD, (float)w, (float)h, (float)r);
         float sh1 = rr_cov(px, py, (float)(PILL_PAD - 4), (float)(PILL_PAD + 5 - 4), (float)(w + 8), (float)(h + 8), (float)(r + 4)) * shadow_a * 0.8f;
         float sh2 = rr_cov(px, py, (float)PILL_PAD, (float)(PILL_PAD + 3), (float)w, (float)h, (float)r) * shadow_a;
         float ca[3] = { 0, 0, 0 }, aa = sh1 + sh2 * (1 - sh1), t, hi;
         int k;
         /* the shadow is only outside the pill: nothing dark shows through its see-through body */
         aa *= 1 - outer;
         /* the edge */
         t = edge_a * outer * (1 - inner);
         if (t > 0)
         {
            float ec[3] = { (edge_col >> 16 & 255) / 255.0f, (edge_col >> 8 & 255) / 255.0f, (edge_col & 255) / 255.0f }, na = t + aa * (1 - t);
            for (k = 0; k < 3; k++)
               ca[k] = (ec[k] * t + ca[k] * aa * (1 - t)) / na;
            aa = na;
         }
         /* the fill, with a highlight at the top that fades out by the bottom */
         t = fill_a * inner;
         hi = hi_a * (1.0f - (float)(y - PILL_PAD) / (h * 0.9f));
         if (t > 0)
         {
            float fc[3] = { (fill >> 16 & 255) / 255.0f, (fill >> 8 & 255) / 255.0f, (fill & 255) / 255.0f }, na = t + aa * (1 - t);
            hi = hi < 0 ? 0 : hi;
            for (k = 0; k < 3; k++)
            {
               float c0 = fc[k] + (1.0f - fc[k]) * hi;
               ca[k] = (c0 * t + ca[k] * aa * (1 - t)) / na;
            }
            aa = na;
         }
         sp->img.px[(size_t)y * sw + x] = ((uint32_t)(aa * 255 + 0.5f) << 24) | ((uint32_t)(ca[0] * 255 + 0.5f) << 16) |
                                          ((uint32_t)(ca[1] * 255 + 0.5f) << 8) | (uint32_t)(ca[2] * 255 + 0.5f);
      }
}

/* A picture with see-through edges made from drawing code: `fn` draws on a canvas of w x h, once on black and once on white; what
 * shows through is (white - black), and the colour is what's left of the black one. (Drawn live it would have to be blended
 * over whatever is behind, every frame.) */
static void sprite_from_draw(image_t *im, int w, int h, void (*fn)(canvas_t *, const void *), const void *arg)
{
   int n = w * h, i;
   uint32_t *bk = malloc((size_t)n * 4), *wh = malloc((size_t)n * 4);
   canvas_t cb = { w, h, w, bk }, cw = { w, h, w, wh };
   if (!bk || !wh)
   {
      free(bk);
      free(wh);
      return;
   }
   if (im->w != w || im->h != h || !im->px)
   {
      free(im->px);
      im->px = malloc((size_t)n * 4);
      im->w = w;
      im->h = h;
   }
   if (im->px)
   {
      for (i = 0; i < n; i++)
      {
         bk[i] = 0xff000000;
         wh[i] = 0xffffffff;
      }
      fn(&cb, arg);
      fn(&cw, arg);
      for (i = 0; i < n; i++)
      {
         int q, diff = 0, a;
         uint32_t out = 0;
         for (q = 0; q <= 16; q += 8)
            diff += 255 - (int)((wh[i] >> q & 255) - (bk[i] >> q & 255));
         a = diff / 3;   /* how much of what's behind is hidden */
         a = a < 0 ? 0 : a > 255 ? 255 : a;
         if (a > 0)
            for (q = 0; q <= 16; q += 8)
            {
               int cc = (int)(bk[i] >> q & 255) * 255 / a;
               out |= (uint32_t)(cc > 255 ? 255 : cc) << q;
            }
         im->px[i] = out | (uint32_t)a << 24;
      }
   }
   free(bk);
   free(wh);
}

/* Such a picture put on the canvas: pixels with no alpha skipped, solid ones copied, only the soft edges blended */
static void sprite_blit(canvas_t *c, const image_t *im, int x0, int y0)
{
   int x, y;
   for (y = 0; y < im->h; y++)
   {
      const uint32_t *src = im->px + (size_t)y * im->w;
      uint32_t *dst;
      if (y0 + y < 0 || y0 + y >= c->h)
         continue;
      dst = c->px + (size_t)(y0 + y) * c->stride + x0;
      for (x = 0; x < im->w; x++)
      {
         uint32_t s = src[x];
         unsigned a = s >> 24, o;
         if (!a || x0 + x < 0 || x0 + x >= c->w)
            continue;
         if (a == 255)
         {
            dst[x] = s;
            continue;
         }
         o = dst[x];
         a++;
         dst[x] = 0xff000000 | ((((s >> 16 & 255) * a + (o >> 16 & 255) * (256 - a)) >> 8) << 16) |
                  ((((s >> 8 & 255) * a + (o >> 8 & 255) * (256 - a)) >> 8) << 8) |
                  (((s & 255) * a + (o & 255) * (256 - a)) >> 8);
      }
   }
}

typedef struct { font_t *tf; const char *title; int pw, ph; } titlepill_t;

/* the row's name pill, drawn with its top left at (8, 4) on a canvas 18 px taller than the pill and 16 px wider */
static void row_title_pill(canvas_t *c, const void *arg)
{
   const titlepill_t *tp = arg;
   int pw = tp->pw, ph = tp->ph, px = 8, py0 = 4;
   {
      /* a soft shadow under the pill (only the rows below it are drawn, so it stays cheap) */
      canvas_t sub = *c;
      int top = py0 + ph - 8;
      sub.px = c->px + (size_t)top * c->stride;
      sub.h = 18;
      fill_round_rect_alpha(&sub, px - 4, py0 + 5 - top, pw + 8, ph, ph / 2, 0x000000, 100);
      fill_round_rect_alpha(&sub, px, py0 + 3 - top, pw, ph, ph / 2, 0x000000, 120);
   }
   fill_round_rect(c, px - 2, py0 - 2, pw + 4, ph + 4, (ph + 4) / 2, 0xc0000000);   /* a dark edge, so it reads on any card */
   fill_round_rect(c, px, py0, pw, ph, ph / 2, C_ACCENT);
   {
      /* a light gradient over it: a soft highlight along the top, fading out by the bottom */
      int j, rad = ph / 2;
      for (j = 0; j < ph; j++)
      {
         float dy = fabsf(j + 0.5f - ph / 2.0f);
         int inset = rad - (int)sqrtf((float)(rad * rad) - dy * dy) + 1, a = (int)(96 * (1.0f - (float)j / (ph * 0.9f)));
         if (a > 0 && pw - 2 * inset > 0)
            gfx_fill_rect(c, px + inset, py0 + j, pw - 2 * inset, 1, ((uint32_t)a << 24) | 0xffffff);
      }
   }
   draw_centered(c, tp->tf, tp->title, px + pw / 2, py0 + (ph - font_height(tp->tf)) / 2, 0xffffffff);
}

/* a screen's title as the same pill the rows' names use, centred on cx with its top at py0 (made once for each text) */
static void draw_title_pill(canvas_t *c, const char *title, int cx, int py0)
{
   static struct { char key[96]; image_t img; } spr;
   font_t *tf = f_label ? f_label : f_title ? f_title : f_small;
   int ph = font_height(tf) + 6, pw = font_text_width(tf, title) + 48;
   char key[96];
   snprintf(key, sizeof(key), "%s,%d,%d,%06x", title, pw, ph, C_ACCENT & 0xffffff);
   if (strcmp(key, spr.key) || !spr.img.px)
   {
      titlepill_t tp = { tf, title, pw, ph };
      sprite_from_draw(&spr.img, pw + 16, ph + 18, row_title_pill, &tp);
      snprintf(spr.key, sizeof(spr.key), "%s", key);
   }
   if (spr.img.px)
      sprite_blit(c, &spr.img, cx - pw / 2 - 8, py0 - 4);
}

static void draw_row_label(canvas_t *c, const char *title, int cy, int r)
{
   /* The row's name as a big pill in the theme's accent, in large white letters, so it's
    * plain which row you're on. Above the focused card, over the row that peeks out. */
   static pillspr_t dspr;
   static struct { char key[96]; image_t img; } tspr[NROWS + 1];   /* the name pills, one for each row, made when first needed */
   font_t *tf = f_label ? f_label : f_title ? f_title : f_small;
   int tw = font_text_width(tf, title), cnt = row_count(r), i;
   int ph = font_height(tf) + 6, pw = tw + 48, px = c->w / 2 - pw / 2, py0 = cy - CARD_H / 2 - ph - 5;   /* (the gap above the card is about 60 px: stays clear of the row above) */
   char key[96];
   snprintf(key, sizeof(key), "%s,%d,%d,%06x", title, pw, ph, C_ACCENT & 0xffffff);
   if (strcmp(key, tspr[r].key) || !tspr[r].img.px)
   {
      titlepill_t tp = { tf, title, pw, ph };
      sprite_from_draw(&tspr[r].img, pw + 16, ph + 18, row_title_pill, &tp);
      snprintf(tspr[r].key, sizeof(tspr[r].key), "%s", key);
   }
   if (tspr[r].img.px)
      sprite_blit(c, &tspr[r].img, px - 8, py0 - 4);
   if (cnt > 1)
   {
      /* round dots on a small pill just big enough to hold them */
      int x0 = c->w / 2 - cnt * 7 + 3, py = cy + 110 + CARD_SLAB + 6;   /* clear of the card and its slab */
      int dw = cnt * 14 - 6 + 12;
      snprintf(key, sizeof(key), "%d,%06x,%06x,%d", dw, C_ACCENT & 0xffffff, TH(TH_DOTS) & 0xffffff, theme_glass_pill);
      if (strcmp(key, dspr.key) || !dspr.img.px)
      {
         pill_sprite(&dspr, dw, 20, 1, C_ACCENT, theme_glass_pill ? 0.95f : 1.0f, TH(TH_DOTS), theme_glass_pill ? 0.55f : 1.0f, 0.0f, 0.50f);
         snprintf(dspr.key, sizeof(dspr.key), "%s", key);
      }
      sprite_blit(c, &dspr.img, x0 - 6 - PILL_PAD, py - PILL_PAD);
      for (i = 0; i < cnt; i++)
         fill_round_rect(c, x0 + i * 14, py + 6, 8, 8, 4, i == ui.sel_[r] ? C_ACCENT : C_FAINT);
   }
}

/* Gentle stars in the sky of the home screen: STARS fixed points (made once from a fixed seed, so they are always in the same
 * places) that each fade softly in and out on their own slow cycle (3 to 7 seconds); the bigger ones have a small cross. They are
 * drawn over everything on the home screen except the cards of the focused row (their rectangles are left clear) and the
 * labels drawn after them, so the cards of the rows above and below sparkle but the row you're on doesn't. The whole
 * effect's strength is set in the Theme card (theme_stars: 0 is off, 100 all of them). At the idle
 * redraw rate (about 10 a frame a second) the fading is smooth enough, and each star is a handful of pixels. */
#define STARS 210   /* at 100%; the Theme card's slider sets how many of them show (50% = 105) */
static struct { short x, y; unsigned char size, hue, saved; float phase, speed, peak; uint32_t under[25]; } stars[STARS];
static int stars_made;

static int theme_stars = 50;  /* how many of the home screen's twinkling stars show, 0..100 (Theme card: Stars slider; saved in theme.cfg) */
static int theme_glint = 1;   /* the glint that sweeps across a card when it's chosen (Theme card: Glint On/Off) */

typedef struct { int x, y, w, h; } rect_t;
static int home_clean;       /* the screen is a finished quiet home frame that only the stars need touching up on */
static double home_clean_t;
static rect_t star_ex[28];   /* where the stars are kept out of in the last full home frame (see draw_home) */
static int star_nex;

/* Puts back what the stars covered (the 5x5 patches saved when they were drawn) */
static void stars_restore(canvas_t *c)
{
   int i, sx, sy;
   for (i = 0; i < STARS; i++)
      if (stars[i].saved)
      {
         for (sy = 0; sy < 5; sy++)
            for (sx = 0; sx < 5; sx++)
               c->px[(size_t)(stars[i].y - 2 + sy) * c->stride + stars[i].x - 2 + sx] = stars[i].under[sy * 5 + sx];
         stars[i].saved = 0;
      }
}

static void stars_draw(canvas_t *c, double now, const rect_t *ex, int nex)
{
   int i, e;
   for (i = 0; i < STARS; i++)
      stars[i].saved = 0;   /* (only the ones drawn now have something to put back) */
   if (theme_stars <= 0)
      return;
   if (!stars_made)
   {
      unsigned seed = 0x9e3779b9u;
      int span = c->h - 52;   /* everywhere above the hint bar */
      for (i = 0; i < STARS; i++)
      {
         seed = seed * 1664525u + 1013904223u;
         stars[i].x = (short)(8 + (seed >> 8) % (unsigned)(c->w - 16));
         seed = seed * 1664525u + 1013904223u;
         stars[i].y = (short)(6 + (seed >> 8) % (unsigned)(span - 6));
         seed = seed * 1664525u + 1013904223u;
         stars[i].size = (seed >> 24) < 85 ? 2 : 1;   /* about one in three has a cross */
         stars[i].hue = (unsigned char)((seed >> 16) & 3);
         seed = seed * 1664525u + 1013904223u;
         stars[i].phase = (seed >> 8) % 6283 / 1000.0f;
         stars[i].speed = 0.9f + (seed >> 20) % 100 / 100.0f * 1.0f;   /* radians a second: 3 to 7 s a cycle */
         stars[i].peak = 0.75f + (seed >> 4) % 26 / 100.0f;
      }
      stars_made = 1;
   }
   for (i = 0; i < STARS * theme_stars / 100; i++)
   {
      float tw = 0.5f + 0.5f * sinf((float)now * stars[i].speed + stars[i].phase);
      float v = (0.12f + 0.88f * tw * tw) * stars[i].peak;   /* mostly dim with a soft peak, never quite out */
      uint32_t col = stars[i].hue == 0 ? 0xffffe8c8u : stars[i].hue == 1 ? 0xffd8e4ffu : 0xffffffffu;
      int x = stars[i].x, y = stars[i].y;
      if (v < 0.04f)
         continue;
      for (e = 0; e < nex; e++)   /* (not over the cards of the focused row: their rectangles are left clear) */
         if (x >= ex[e].x - 3 && x < ex[e].x + ex[e].w + 3 && y >= ex[e].y - 3 && y < ex[e].y + ex[e].h + 3)
            break;
      if (e < nex)
         continue;
      {
         /* what is under it (a 5x5 patch), so a quiet frame can put it back and draw the stars again without redrawing the screen */
         int sx, sy;
         for (sy = 0; sy < 5; sy++)
            for (sx = 0; sx < 5; sx++)
               stars[i].under[sy * 5 + sx] = c->px[(size_t)(y - 2 + sy) * c->stride + x - 2 + sx];
         stars[i].saved = 1;
      }
      {
         /* on a light card (a white star would vanish) the star gets a soft dark halo and is drawn at full strength, so it
          * reads as a point of light on a glossy surface */
         uint32_t u = c->px[(size_t)y * c->stride + x];
         int luma = (int)(((u >> 16 & 255) * 3 + (u >> 8 & 255) * 6 + (u & 255)) / 10);
         if (luma > 135 && x > 1 && y > 1 && x < c->w - 2 && y < c->h - 2)
         {
            float h = v * 0.45f;
            fill_px(c, x - 1, y, 0x401020, h);
            fill_px(c, x + 1, y, 0x401020, h);
            fill_px(c, x, y - 1, 0x401020, h);
            fill_px(c, x, y + 1, 0x401020, h);
            fill_px(c, x, y, 0xffffff, v > 0.35f ? 1.0f : v * 2.8f);
            if (stars[i].size > 1)
            {
               fill_px(c, x - 2, y, 0xffffff, v * 0.8f);
               fill_px(c, x + 2, y, 0xffffff, v * 0.8f);
               fill_px(c, x, y - 2, 0xffffff, v * 0.8f);
               fill_px(c, x, y + 2, 0xffffff, v * 0.8f);
            }
            continue;
         }
      }
      fill_px(c, x, y, col, v);
      if (stars[i].size > 1)
      {
         float a = v * 0.7f;
         fill_px(c, x - 1, y, col, a);
         fill_px(c, x + 1, y, col, a);
         fill_px(c, x, y - 1, col, a);
         fill_px(c, x, y + 1, col, a);
         if (v > 0.5f)
         {
            fill_px(c, x - 2, y, col, a * 0.5f);
            fill_px(c, x + 2, y, col, a * 0.5f);
            fill_px(c, x, y - 2, col, a * 0.5f);
            fill_px(c, x, y + 2, col, a * 0.5f);
         }
      }
      else
      {
         /* a faint halo so a single pixel doesn't just flicker */
         fill_px(c, x + 1, y, col, v * 0.4f);
         fill_px(c, x, y + 1, col, v * 0.4f);
         fill_px(c, x - 1, y, col, v * 0.2f);
         fill_px(c, x, y - 1, col, v * 0.2f);
      }
   }
}

static int glint_r = -1, glint_i = -1;
static double glint_t0, glint_until;   /* the glint on the chosen card: when it starts, and when frames can slow again */

static void draw_home(canvas_t *c, double now)
{
   static card_t cards[NROWS * (MAX_SYSTEMS + 4)];
   int r, i, n = 0, focus_row = cur_row();
   double focus_d = 9;

   double tb = PF_T(), tc;
   bg_stage_on = 1;
   draw_background(c);
   bg_stage_on = 0;
   PF_ADD(PF_BG, tb);
   for (r = 0; r < NROWS; r++)
   {
      double d = row_offset(r), sy = card_scale_y(fabs(d));
      int cy = HOME_CY + (int)(d * ROW_GAP), cnt = row_count(r);
      if (fabs(d) >= 1.4)
         continue;   /* passing round the back while the rows loop */
      if (fabs(d) < focus_d)
         focus_d = fabs(d);
      for (i = 0; i < cnt; i++)
      {
         double off = item_offset(r, i), sx = card_scale_x(fabs(off));
         int cx = c->w / 2 + (int)(off * CARD_GAP * sy) + side_push(off);
         if (cx < -CARD_W || cx > c->w + CARD_W)
            continue;
         cards[n].r = r;
         cards[n].i = i;
         cards[n].cx = cx;
         cards[n].cy = cy;
         cards[n].scale = sx * sy;
         cards[n].depth = fabs(d) * 10 + fabs(off);
         cards[n].rowd = d;
         n++;
      }
   }
   qsort(cards, n, sizeof(card_t), card_cmp);   /* far ones first, the focused card on top */
   smooth_budget = 2;
   tc = PF_T();
   {
      /* the focused card: its reflection goes down first (the other cards cover it where they meet), and once it has been
       * on for a moment it gets its glint */
      const card_t *fk = NULL;
      for (i = 0; i < n; i++)
         if (cards[i].r == focus_row && cards[i].i == ui.sel_[focus_row] && cards[i].depth < 0.5)
            fk = &cards[i];
      if (fk && !(fk->r == ROW_MEDIA && !media_has_logo(fk->i)))
      {
         cardimg_t *fci = card_image(fk->r, fk->i);
         if (fci && fci->img.px)
            card_reflection(c, fci, fk->cx - (int)(CARD_W * fk->scale) / 2, fk->cy - (int)(CARD_H * fk->scale) / 2,
                            (int)(CARD_W * fk->scale), (int)(CARD_H * fk->scale), (int)(28 * fk->scale), (int)(CARD_SLAB * fk->scale + 0.5));
      }
      /* the glint belongs to the card that has just been chosen (the one the row is moving to), and starts the moment
       * the button is pressed, while the card is still on its way in */
      {
         int tgt_i = ui.sel_[focus_row];
         if (focus_row != glint_r || tgt_i != glint_i)
         {
            glint_r = focus_row;
            glint_i = tgt_i;
            glint_t0 = now;
            glint_until = theme_glint ? now + 0.7 : 0;
         }
         for (i = 0; i < n; i++)
         {
            draw_card(c, &cards[i], cards[i].r == focus_row && cards[i].i == ui.sel_[focus_row] && cards[i].depth < 0.5);
            if (theme_glint && cards[i].r == focus_row && cards[i].i == tgt_i && now >= glint_t0 && now < glint_t0 + 0.6 &&
                !(focus_row == ROW_MEDIA && !media_has_logo(tgt_i)))
            {
               /* (drawn straight after its card, so the cards in front of it still cover it) */
               int gw = (int)(CARD_W * cards[i].scale), gh = (int)(CARD_H * cards[i].scale);
               card_glint(c, cards[i].cx - gw / 2, cards[i].cy - gh / 2, gw, gh, (int)(28 * cards[i].scale), (now - glint_t0) / 0.6);
            }
         }
      }
      {
         rect_t *ex = star_ex;
         int nex = 0;
         for (i = 0; i < n && nex < 24; i++)
            if (cards[i].r == focus_row && fabs(cards[i].rowd) < 0.5)
            {
               int cw = (int)(CARD_W * cards[i].scale), ch = (int)(CARD_H * cards[i].scale);
               ex[nex].x = cards[i].cx - cw / 2;
               ex[nex].y = cards[i].cy - ch / 2;
               ex[nex].w = cw + CARD_SLAB;
               ex[nex].h = ch + CARD_SLAB;
               nex++;
            }
         if (focus_d < 0.5)
         {
            /* the things drawn over the stars afterwards (the row's name, the dots, the battery) are kept clear of them: a quiet
             * frame puts back what a star covered, which would otherwise take those pixels back too */
            font_t *tf = f_label ? f_label : f_title ? f_title : f_small;
            int ph = font_height(tf) + 6, pw = font_text_width(tf, row_titles[focus_row]) + 48;
            int cy = HOME_CY + (int)(row_offset(focus_row) * ROW_GAP), cnt = row_count(focus_row);
            ex[nex].x = c->w / 2 - pw / 2 - 8;
            ex[nex].w = pw + 16;
            ex[nex].y = cy - CARD_H / 2 - ph - 14;
            ex[nex].h = ph + 34;
            nex++;
            if (cnt > 1)
            {
               ex[nex].x = c->w / 2 - cnt * 7 - 14;
               ex[nex].w = cnt * 14 + 28;
               ex[nex].y = cy + 110 + CARD_SLAB - 6;
               ex[nex].h = 44;
               nex++;
            }
            ex[nex].x = 560;
            ex[nex].y = 78;
            ex[nex].w = c->w - 560;
            ex[nex].h = 44;
            nex++;
         }
         star_nex = nex;
         stars_draw(c, now, ex, nex);
      }
   }
   PF_ADD(PF_CARDS, tc);
   tc = PF_T();

   /* the focused row's name and position */
   r = focus_row;
   if (focus_d < 0.5)
   {
      double tp = PF_T();
      draw_row_label(c, row_titles[r], HOME_CY + (int)(row_offset(r) * ROW_GAP), r);
      PF_ADD(PF_PILL, tp);
   }
   /* the battery sits just above the focused card, level with the row's title */
   {
      double ts = PF_T();
      draw_status(c, c->w / 2 + CARD_W / 2, HOME_CY - CARD_H / 2 - 18, 0);
      PF_ADD(PF_STATUS, ts);
   }
   draw_toast(c, now);
   draw_volume(c, now);
   {
      double th = PF_T();
      draw_hints(c, "A  Open     Dpad  Navigation     X  Search     Y  Controls     Select  All Games");
      PF_ADD(PF_HINTS, th);
   }
   PF_ADD(PF_LABELS, tc);
}

#define ROW_H    32
#define LIST_Y   58
#define LIST_ROWS ((480 - LIST_Y - 48) / ROW_H)

/* A game that closed within seconds without RetroArch saying it ran didn't start:
 * show why, from its log. Emulators log harmless "errors" too, so those are skipped. */
#define LAST_SESSION "/tmp/last-session"
#define LAST_LOG     "/tmp/ra-prev.log"
static int check_launch(void)
{
   FILE *f = fopen(LAST_SESSION, "r");
   char line[1300], key[1300] = "";
   long secs = 99;
   int ran = 0, nerr = 0, sys, gi;

   if (!f)
      return 0;
   if (fgets(line, sizeof(line), f))
   {
      char *sp = strchr(line, ' ');
      secs = atol(line);
      if (sp)
         snprintf(key, sizeof(key), "%s", trim(sp + 1));
   }
   fclose(f);
   unlink(LAST_SESSION);
   if (secs >= 10 || !(f = fopen(LAST_LOG, "r")))
      return 0;
   err_line1[0] = err_line2[0] = 0;
   while (fgets(line, sizeof(line), f))
   {
      char *m;
      line[strcspn(line, "\n")] = 0;
      if (strstr(line, "Content ran for a total"))
         ran = 1;
      if (strncmp(line, "[ERROR]", 7) || strstr(line, "GET_VARIABLE"))
         continue;
      m = trim(line + 7);
      /* drop a "[Tag]" after [ERROR] */
      if (*m == '[' && strchr(m, ']'))
         m = trim(strchr(m, ']') + 1);
      if (!*m)
         continue;
      if (nerr == 0)
         snprintf(err_line1, sizeof(err_line1), "%s", m);
      else if (strcmp(m, err_line1))
         snprintf(err_line2, sizeof(err_line2), "%s", m);   /* keeps the last one */
      nerr++;
   }
   fclose(f);
   if (ran)
      return 0;
   {
      /* say the common causes plainly; RetroArch's own line goes underneath */
      char raw[160];
      snprintf(raw, sizeof(raw), "%s", err_line1);
      if (strcasestr(raw, "bios") || strcasestr(err_line2, "bios"))
      {
         mlog("launch error from RetroArch: %s", raw);
         snprintf(err_line1, sizeof(err_line1), "A BIOS file is missing or isn't the right one.");
         snprintf(err_line2, sizeof(err_line2), "BIOS files go in bios/ (see README.txt).");
      }
      else if (strcasestr(raw, "load content") || strcasestr(raw, "read content") || strcasestr(err_line2, "load content"))
      {
         mlog("launch error from RetroArch: %s", raw);
         snprintf(err_line1, sizeof(err_line1), "The game file couldn't be loaded.");
         snprintf(err_line2, sizeof(err_line2), "It may be damaged, or not for this console.");
      }
   }
   if (!nerr)
      snprintf(err_line1, sizeof(err_line1), "It closed right after starting.");
   if (!err_line2[0])
      snprintf(err_line2, sizeof(err_line2), "Details: g36/logs/launch-1.log");
   snprintf(err_title, sizeof(err_title), "Couldn't start %s",
         game_find(key, &sys, &gi) ? systems[sys].games[gi].label : "the game");
   mlog("launch failed: %s: %s / %s", key, err_line1, err_line2);
   {
      /* it never ran, so it isn't "recently played" */
      char **keys;
      int n = read_keys(RECENT_FILE, &keys), i, j = 0;
      for (i = 0; i < n; i++)
         if (strcmp(keys[i], key))
            keys[j++] = keys[i];
         else
            free(keys[i]);
      if (j != n)
         write_keys(RECENT_FILE, keys, j);
      free_keys(keys, j);
   }
   return 1;
}

/* play time, from g36/playtime.txt ("<path inside roms/> = <seconds>"), read at start */
#define PLAYTIME_FILE SDCARD "/g36/playtime.txt"
static char **pt_keys;
static long *pt_secs;
static int pt_n;

static void load_playtime(void)
{
   FILE *f = fopen(PLAYTIME_FILE, "r");
   char line[1300];
   int cap = 0;
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *eq = strstr(line, " = ");
      if (line[0] == '#' || !eq)
         continue;
      *eq = 0;
      if (pt_n == cap)
      {
         cap = cap ? cap * 2 : 64;
         pt_keys = realloc(pt_keys, cap * sizeof(char *));
         pt_secs = realloc(pt_secs, cap * sizeof(long));
      }
      pt_keys[pt_n] = strdup(line);
      pt_secs[pt_n] = atol(eq + 3);
      pt_n++;
   }
   fclose(f);
}

static long playtime(const char *key)
{
   int i;
   for (i = 0; i < pt_n; i++)
      if (!strcmp(pt_keys[i], key))
         return pt_secs[i];
   return 0;
}

static void draw_playtime(canvas_t *c, const entry_t *e, int x, int y)
{
   char key[1300], text[64];
   long t;
   entry_key(e, key, sizeof(key));
   t = playtime(key);
   if (!t)
      snprintf(text, sizeof(text), "Not played yet");
   else if (t < 60)
      snprintf(text, sizeof(text), "Played under a minute");
   else if (t < 3600)
      snprintf(text, sizeof(text), "Played %ld min", t / 60);
   else
      snprintf(text, sizeof(text), "Played %ld h %ld min", t / 3600, t / 60 % 60);
   gfx_draw_text(c, f_small, text, x, y, C_DIM, 260);
}

/* which emulator a game will start with, for the details panel; looked up again
 * only when the selection changes (or the picker saved something) */
static int emu_cache_dirty = 1;

static void draw_emulator(canvas_t *c, const entry_t *e, int x, int y)
{
   static char rom_cached[1300], core[64];
   static int own;
   char dir[1024], rom[1300], text[120];

   if (ui.list_all)
      snprintf(dir, sizeof(dir), "%s/%s", ROMS, systems[e->sys].id);
   else
      current_dir(dir, sizeof(dir));
   snprintf(rom, sizeof(rom), "%s/%s", dir, e->name);
   if (emu_cache_dirty || strcmp(rom, rom_cached))
   {
      char game[64];
      game_core(rom, game, sizeof(game));
      own = game[0] && strcmp(game, systems[e->sys].ncores ? systems[e->sys].cores[0] : "");
      pick_core(&systems[e->sys], rom, core, sizeof(core));
      snprintf(rom_cached, sizeof(rom_cached), "%s", rom);
      emu_cache_dirty = 0;
   }
   snprintf(text, sizeof(text), own ? "%s (this game)" : "%s", core_title(core));
   gfx_draw_text(c, f_small, text, x, y, own ? C_ACCENT : C_DIM, 260);
}

/* emulator picker: a panel listing the console's emulators */
static void draw_picker(canvas_t *c)
{
   const system_t *s = &systems[pick.sys];
   int rows = pick.n, w = 560, h = 120 + rows * 44, x = c->w / 2 - w / 2, y = (c->h - h) / 2 - 10, i;
   char title[300];

   gfx_fill_rect(c, 0, 0, c->w, c->h, 0xb0000000);
   gfx_fill_rect(c, x, y, w, h, C_PANEL);
   gfx_fill_rect(c, x, y, w, 4, C_ACCENT);
   snprintf(title, sizeof(title), "Emulator for %s", pick.label);
   gfx_draw_text(c, f_big, title, x + 24, y + 18, C_TEXT, w - 48);
   for (i = 0; i < rows; i++)
   {
      int ry = y + 70 + i * 44, sel = i == pick.sel;
      const char *tag = !strcmp(pick.cores[i], pick.game) ? "this game"
            : !strcmp(pick.cores[i], s->cores[0]) ? "console default" : "";
      if (sel)
      {
         gfx_fill_rect(c, x + 16, ry, w - 32, 40, C_SEL);
         gfx_fill_rect(c, x + 16, ry, 4, 40, C_ACCENT);
      }
      gfx_draw_text(c, f_mid, core_title(pick.cores[i]), x + 34, ry + (40 - font_height(f_mid)) / 2,
            sel ? C_TEXT : C_DIM, 0);
      if (*tag)
         gfx_draw_text(c, f_small, tag, x + w - 34 - font_text_width(f_small, tag),
               ry + (40 - font_height(f_small)) / 2, C_FAINT, 0);
   }
   draw_centered(c, f_small, "A  Choose     B  Cancel", c->w / 2, y + h - 34, C_FAINT);
}

/* the details panel's picture: thumbnails/large/<console>/<game>.png (made by
 * g36-thumbs beside the small one); the last few are kept */
#define LARGE_CACHE 8
static struct { char key[1300]; image_t *img; int tried; } large_c[LARGE_CACHE];
static int large_next;

static const image_t *large_for(const entry_t *e)
{
   char key[1300], path[1500], base[1024], *dot;
   const char *slash = strrchr(e->name, '/');
   int i;
   snprintf(key, sizeof(key), "%s/%s", systems[e->sys].id, e->name);
   for (i = 0; i < LARGE_CACHE; i++)
      if (large_c[i].tried && !strcmp(large_c[i].key, key))
         return large_c[i].img;
   snprintf(base, sizeof(base), "%s", slash ? slash + 1 : e->name);
   if ((dot = strrchr(base, '.')))
      *dot = 0;
   i = large_next;
   large_next = (large_next + 1) % LARGE_CACHE;
   image_free(large_c[i].img);
   large_c[i].img = NULL;
   snprintf(large_c[i].key, sizeof(large_c[i].key), "%s", key);
   large_c[i].tried = 1;
   snprintf(path, sizeof(path), THUMB_DIR "/large/%s/%s.png", systems[e->sys].id, base);
   if (access(path, F_OK) == 0)
      large_c[i].img = image_load(path);
   return large_c[i].img;
}

static void large_clear(void)
{
   int i;
   gcard_clear();   /* (the games carousel's cards hold those pictures too) */
   for (i = 0; i < LARGE_CACHE; i++)
   {
      image_free(large_c[i].img);
      large_c[i].img = NULL;
      large_c[i].tried = 0;
   }
}

/* the folder in the middle of a game list's picture box */
static void draw_folder_icon(canvas_t *c)
{
   int w = 140, h = 104;
   draw_folder_shape(c, 567 + (260 - w) / 2, LIST_Y + 15 + (195 - h) / 2, w, h, TH(TH_PICBOX));
}

/* a game with no picture: the picture box says so, and (barely visible) how to get pictures:
 * the Get Artwork program in the card's "Get Artwork" folder, run on a computer */
#define ART_HINT_COLOUR 0x50b8a8d8   /* the faint text (see-through); the outline is the box colour lifted a little */

/* A game with no picture shows its console's logo on a card-coloured plate instead of a message: the big one for the picture
 * box (260x195) and a small one for the list's thumbnail square. Each is made once per console and kept; a theme change
 * that touches the colours used makes them again. A console without a logo keeps the old words (big box) or the gray square. */
#define SYSART_W 260
#define SYSART_H 195
static image_t *sysart[2][MAX_SYSTEMS];     /* [0] the picture box, [1] the thumbnail */
static unsigned char sysart_none[2][MAX_SYSTEMS];
static uint32_t sysart_sig;

/* a console's colour, from its logo: the average of its colourful pixels (white and gray logos have none: the theme's accent) */
static uint32_t sysart_colour(const image_t *logo)
{
   double r = 0, g = 0, b = 0, wsum = 0;
   int i, n;
   if (!logo)
      return TH(TH_ACCENT);
   n = logo->w * logo->h;
   for (i = 0; i < n; i++)
   {
      uint32_t px = logo->px[i];
      int pr = px >> 16 & 255, pg = px >> 8 & 255, pb = px & 255, mx, mn;
      if ((px >> 24) < 200)
         continue;
      mx = pr > pg ? (pr > pb ? pr : pb) : (pg > pb ? pg : pb);
      mn = pr < pg ? (pr < pb ? pr : pb) : (pg < pb ? pg : pb);
      if (mx < 40)
         continue;
      {
         double sat = (double)(mx - mn) / mx, w = sat * sat * sat;
         r += pr * w;
         g += pg * w;
         b += pb * w;
         wsum += w;
      }
   }
   if (wsum < 8.0)
      return TH(TH_ACCENT);
   return 0xff000000 | (uint32_t)(r / wsum) << 16 | (uint32_t)(g / wsum) << 8 | (uint32_t)(b / wsum);
}

/* the little pictures that stand in for a game's thumbnail: a cartridge, or a disc for the disc-based consoles. Light from
 * the top left like the menu's cards, so the shadow falls down and right. 24x24; no text. */
static void glyph_cart(canvas_t *cv, int ox, int oy, uint32_t light, uint32_t dark, uint32_t label)
{
   /* a game cartridge: the wide body with grip ridges across the top and a label sticker, stepping in at the shoulders
    * to the narrower foot that carries the contacts */
   fill_round_rect_alpha(cv, ox + 5, oy + 4, 16, 14, 2, 0xff000000, 95);
   fill_round_rect_alpha(cv, ox + 7, oy + 15, 12, 7, 1, 0xff000000, 95);
   fill_round_rect(cv, ox + 4, oy + 3, 16, 14, 2, light);          /* the body */
   fill_round_rect(cv, ox + 6, oy + 15, 12, 6, 1, light);          /* the foot */
   fill_round_rect_alpha(cv, ox + 7, oy + 4, 10, 1, 0, dark, 170); /* the grip ridges */
   fill_round_rect_alpha(cv, ox + 7, oy + 6, 10, 1, 0, dark, 170);
   fill_round_rect(cv, ox + 6, oy + 8, 12, 7, 1, label);           /* the label */
   fill_round_rect(cv, ox + 8, oy + 18, 8, 3, 0, dark);            /* the contacts */
}

static void glyph_disc(canvas_t *cv, int ox, int oy, uint32_t light, uint32_t dark, uint32_t hole)
{
   int x, y;
   for (y = 0; y < 24; y++)
      for (x = 0; x < 24; x++)
      {
         float d = hypotf(x + 0.5f - 12.0f, y + 0.5f - 12.0f), ds = hypotf(x + 0.5f - 13.0f, y + 0.5f - 13.0f), cov;
         cov = 9.5f - ds;                                    /* the shadow */
         if (cov > 0)
            fill_px(cv, ox + x, oy + y, 0xff000000, (cov > 1 ? 1.0f : cov) * 0.37f);
         cov = 9.5f - d;                                     /* the disc */
         if (cov > 0)
            fill_px(cv, ox + x, oy + y, light, cov > 1 ? 1.0f : cov);
         cov = 1.0f - fabsf(d - 5.4f);                       /* the ring round the hub */
         if (cov > 0 && d < 9.0f)
            fill_px(cv, ox + x, oy + y, dark, cov * 0.45f);
         cov = 3.1f - d;                                     /* the hole: what's behind the disc shows through */
         if (cov > 0)
            fill_px(cv, ox + x, oy + y, hole, cov > 1 ? 1.0f : cov);
      }
}

static image_t *sysart_make(int sys, int small)
{
   char path[300];
   image_t *logo = NULL, *im;
   canvas_t cv;
   int w = small ? THUMB_PX : SYSART_W, h = small ? THUMB_PX : SYSART_H, lw, lh, bw, bh;

   snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, systems[sys].id);
   if (access(path, F_OK) == 0)
      logo = image_load(path);
   if (!logo && !small)
      return NULL;
   if (!(im = malloc(sizeof(*im))))
   {
      image_free(logo);
      return NULL;
   }
   im->w  = w;
   im->h  = h;
   im->px = malloc((size_t)w * h * 4);
   if (!im->px)
   {
      free(im);
      image_free(logo);
      return NULL;
   }
   cv.w = cv.stride = w;
   cv.h = h;
   cv.px = im->px;
   gfx_fill_rect(&cv, 0, 0, w, h, small ? 0 : TH(TH_PICBOX));
   if (small)
   {
      /* an icon with nothing behind it, like the hint bar's buttons: a cartridge, or a disc for the disc-based consoles,
       * in a light tint of the console's colour. Painted once on black and once on white; the difference between the two
       * gives each pixel's see-through amount, so the soft shadow stays soft over whatever the row looks like. */
      uint32_t col = sysart_colour(logo);
      const char *id = systems[sys].id;
      int disc = !strcmp(id, "psx") || !strcmp(id, "segacd") || !strcmp(id, "pcenginecd") || !strcmp(id, "neogeocd")
         || !strcmp(id, "fds");
      uint32_t light = mix_colour(col, 0xffffffff, 215), dark = mix_colour(col, 0xff000000, 150);
      static image_t *cart_img;
      static int cart_tried;
      uint32_t *onw = malloc((size_t)w * h * 4);
      int pass, k;
      if (!cart_tried)
      {
         cart_tried = 1;
         cart_img   = image_load_mem(cartridge_icon_png, (int)sizeof(cartridge_icon_png));
      }
      if (!onw)
      {
         image_free(logo);
         free(im->px);
         free(im);
         return NULL;
      }
      for (pass = 0; pass < 2; pass++)
      {
         cv.px = pass ? onw : im->px;
         gfx_fill_rect(&cv, 0, 0, w, h, pass ? 0xffffffff : 0xff000000);
         if (disc)
            glyph_disc(&cv, 0, 0, light, dark, pass ? 0xffffffff : 0xff000000);
         else if (cart_img)
         {
            cast_logo_shadow(&cv, cart_img, 2, 1, 20, 22, 1, 1, 1, 0xff000000, 120);
            gfx_draw_image_smooth(&cv, cart_img, 2, 1, 20, 22);
         }
         else
            glyph_cart(&cv, 0, 0, light, dark, mix_colour(col, 0xffffffff, 25));
      }
      for (k = 0; k < w * h; k++)
      {
         uint32_t bk = im->px[k], wh = onw[k];
         int a = 255 - (int)((wh & 0xff) - (bk & 0xff)), ch, out = 0;   /* white minus black = what was see-through */
         if (a < 0) a = 0;
         if (a > 255) a = 255;
         for (ch = 0; ch < 24; ch += 8)
         {
            int v = a ? (int)((bk >> ch & 0xff) * 255 / a) : 0;           /* the colour without the black behind it */
            out |= (v > 255 ? 255 : v) << ch;
         }
         im->px[k] = (uint32_t)a << 24 | out;
      }
      free(onw);
      image_free(logo);
      return im;
   }
   fill_round_rect_vgrad(&cv, 10, 10, w - 20, h - 20, 12, TH(TH_CARDL_TOP), TH(TH_CARDL_BOT));
   bw = w - 20 - 56;
   bh = h - 20 - 64;
   lw = bw;
   lh = logo->h * lw / logo->w;
   if (lh > bh)
   {
      lh = bh;
      lw = logo->w * lh / logo->h;
   }
   if (lw < 1) lw = 1;
   if (lh < 1) lh = 1;
   /* lit from the top left like the menu's cards: the shadow falls down and right */
   cast_logo_shadow(&cv, logo, (w - lw) / 2, (h - lh) / 2 - 4, lw, lh, 5, 8, 4,
         mix_colour(TH(TH_CARDL_BOT), 0xff000000, 200), 190);
   gfx_draw_image_scaled(&cv, logo, (w - lw) / 2, (h - lh) / 2 - 4, lw, lh);
   image_free(logo);
   return im;
}

static const image_t *sysart_get(int sys, int small)
{
   uint32_t sig = TH(TH_CARDL_TOP) * 31u ^ TH(TH_CARDL_BOT) * 17u ^ TH(TH_PICBOX) * 7u ^ (uint32_t)C_THUMB ^ TH(TH_ACCENT) * 13u;
   if (sys < 0 || sys >= nsystems)
      return NULL;
   if (sig != sysart_sig)
   {
      int i, k;
      for (k = 0; k < 2; k++)
         for (i = 0; i < MAX_SYSTEMS; i++)
         {
            image_free(sysart[k][i]);
            sysart[k][i] = NULL;
            sysart_none[k][i] = 0;
         }
      sysart_sig = sig;
   }
   if (!sysart[small][sys] && !sysart_none[small][sys])
   {
      sysart[small][sys] = sysart_make(sys, small);
      if (!sysart[small][sys])
         sysart_none[small][sys] = 1;
   }
   return sysart[small][sys];
}

static void draw_no_art_text(canvas_t *c)
{
   static const char *const words = "Plug the SD card into a computer and run Get Artwork from it to download box art.";
   int x = 567, y = LIST_Y + 15, w = 260, h = 195, lines = 0, ty, i;
   char line[6][96];
   const char *p = words;

   while (*p && lines < 6)
   {
      /* the longest run of words that fits */
      char cand[96];
      int n = 0, last_space = -1, fit = 0;
      while (p[n] && n < (int)sizeof(cand) - 1)
      {
         cand[n] = p[n];
         cand[n + 1] = 0;
         if (p[n] == ' ')
            last_space = n;
         if (font_text_width(f_small, cand) > w - 40)
            break;
         n++;
         fit = n;
      }
      if (!p[fit])
         last_space = fit;                 /* the rest fits */
      else if (last_space <= 0)
         last_space = fit > 0 ? fit : 1;   /* a word longer than the box */
      memcpy(line[lines], p, (size_t)last_space);
      line[lines][last_space] = 0;
      lines++;
      p += last_space;
      while (*p == ' ')
         p++;
   }

   {
      /* the outline is solid, so it's blended by hand: the box's colour, 12% of the way to the text's */
      uint32_t bg = TH(TH_PICBOX), line_col = 0xff000000;
      int k;
      for (k = 0; k <= 16; k += 8)
         line_col |= ((((bg >> k) & 0xff) * 225 + ((ART_HINT_COLOUR >> k) & 0xff) * 31) >> 8) << k;
      stroke_round_rect(c, x + 8, y + 8, w - 16, h - 16, 8, line_col);
   }
   ty = y + (h - (lines + 1) * 22 - 6) / 2;
   draw_centered(c, f_small, "No picture yet", x + w / 2, ty, ART_HINT_COLOUR);
   for (i = 0; i < lines; i++)
      draw_centered(c, f_small, line[i], x + w / 2, ty + 28 + i * 22, ART_HINT_COLOUR);
}

static void draw_no_art_hint(canvas_t *c, int sys)
{
   const image_t *a = sysart_get(sys, 0);
   if (a)
      gfx_draw_image(c, a, 567, LIST_Y + 15);
   else
      draw_no_art_text(c);
}

/* what's inside the selected folder: a small list under its name. It comes from the library in memory (no
 * disk), is kept for the folder that's selected, and when it doesn't fit it crawls up slowly and steadily
 * (after a pause at the top); after the last one, and a one-line gap, the first comes round again. The menu
 * draws every refresh while it moves. Drawn into a sub-canvas, with the lines fading out at the top and
 * bottom of the box. */
#define FOLD_LINES 4
#define FOLD_LH    22
#define FOLD_MAX   30       /* a longer folder shows its first 30 and then goes round again; the rest is for the folder itself */
#define FOLD_HOLD  2.0      /* seconds the first lines are shown before it starts to move */
#define FOLD_SPEED 9.0      /* pixels a second: a slow, constant crawl */
#define FOLD_FADE  12       /* the lines fade out over this many pixels at the top and bottom of the box */

static struct { char key[1200]; int ngames; list_t l; double since; } fold_c;
static int fold_sliding;    /* set while the list is moving: the main loop then draws every refresh */
static double fold_wake;    /* when the list starts to move (0 = not waiting for that): the main loop wakes up for it */

/* The whole list is drawn once into a tall strip (twice end to end, so it wraps), and each frame the
 * visible window is copied out of it, the two nearest rows blended by the fraction of a pixel the
 * list has moved. That is what makes a slow crawl glide: whole-pixel steps at 9 px/s were a tick every
 * five or six frames. */
static struct { uint32_t *px; int h, ok; uint32_t pan, dim, faint; } fold_tex;

static void fold_render(int lines, int scrolling)
{
   int period = (lines + 1) * FOLD_LH, rows = FOLD_LINES * FOLD_LH, h = scrolling ? period + rows + 2 : rows;
   int pass, i, k;
   canvas_t t;
   char text[600];

   free(fold_tex.px);
   fold_tex.ok = 0;
   fold_tex.px = malloc((size_t)260 * h * 4);
   if (!fold_tex.px)
      return;
   for (k = 0; k < 260 * h; k++)
      fold_tex.px[k] = C_PANEL | 0xff000000u;
   t.w = 260;
   t.h = h;
   t.stride = 260;
   t.px = fold_tex.px;
   for (pass = 0; pass < (scrolling ? 2 : 1); pass++)
      for (i = 0; i < lines; i++)
      {
         const entry_t *it = &fold_c.l.e[i];
         int y = i * FOLD_LH + pass * period;
         if (y >= h)
            continue;
         if (it && it->is_dir)
            snprintf(text, sizeof(text), "[ %s ]", it->label);
         else
            snprintf(text, sizeof(text), "%s", it->label);
         gfx_draw_text(&t, f_small, text, 0, y + (FOLD_LH - font_height(f_small)) / 2, it && it->is_dir ? C_DIM : C_FAINT, 260);
      }
   fold_tex.h = h;
   fold_tex.pan = C_PANEL;
   fold_tex.dim = C_DIM;
   fold_tex.faint = C_FAINT;
   fold_tex.ok = 1;
}

/* 1 when the list kept for the selected folder is not the one for key (and version, which changes when what's
 * in the folder does), so the caller must fill fold_c.l again; the crawl starts over then */
static int fold_stale(const char *key, int version, double now)
{
   if (!strcmp(key, fold_c.key) && fold_c.ngames == version)
      return 0;
   snprintf(fold_c.key, sizeof(fold_c.key), "%s", key);
   fold_c.ngames = version;
   fold_c.since = now;
   fold_tex.ok = 0;
   return 1;
}

static void fold_show(canvas_t *c, double now);

/* the games list's version: what's inside the selected folder, from the library in memory */
static void draw_folder_contents(canvas_t *c, const entry_t *e, double now)
{
   const system_t *s = &systems[e->sys];
   char key[1200], path[1100];

   if (ui.folder[0])
      snprintf(path, sizeof(path), "%s/%s", ui.folder, e->name);
   else
      snprintf(path, sizeof(path), "%s", e->name);
   snprintf(key, sizeof(key), "%d|%s", e->sys, path);
   if (fold_stale(key, s->ngames, now))
      list_from_library(&fold_c.l, s, path);
   fold_show(c, now);
}

/* draws fold_c.l under a folder's name: the lines, crawling when there are more than fit */
static void fold_show(canvas_t *c, double now)
{
   int lines, i, r, iy, fr, scrolling, rows = FOLD_LINES * FOLD_LH, ty = LIST_Y + 258;
   double period, ofs = 0;

   if (!fold_c.l.n)
      return;
   lines = fold_c.l.n < FOLD_MAX ? fold_c.l.n : FOLD_MAX;
   scrolling = lines > FOLD_LINES;

   period = (lines + 1) * FOLD_LH;   /* the lines, a gap, and then it comes round again */
   if (scrolling)
   {
      double t = now - fold_c.since - FOLD_HOLD;       /* a look at the first lines, then it crawls */
      if (t < 0)
         fold_wake = fold_c.since + FOLD_HOLD;
      else
      {
         fold_sliding = 1;
         ofs = fmod(t * FOLD_SPEED, period);
      }
   }

   if (!fold_tex.ok || fold_tex.pan != C_PANEL || fold_tex.dim != C_DIM || fold_tex.faint != C_FAINT)
      fold_render(lines, scrolling);
   if (!fold_tex.ok)
      return;

   iy = (int)ofs;
   fr = (int)((ofs - iy) * 256);
   for (r = 0; r < rows; r++)
   {
      const uint32_t *a = fold_tex.px + (size_t)(iy + r) * 260, *b = a + 260;
      uint32_t *d = c->px + (size_t)(ty + r) * c->stride + 567;
      if (!fr || !scrolling)
         memcpy(d, a, 260 * sizeof(uint32_t));
      else
         for (i = 0; i < 260; i++)
         {
            uint32_t pa = a[i], pb = b[i];
            uint32_t rb = (((pa & 0x00ff00ffu) * (256 - fr) + (pb & 0x00ff00ffu) * fr) >> 8) & 0x00ff00ffu;
            uint32_t g = (((pa & 0x0000ff00u) * (256 - fr) + (pb & 0x0000ff00u) * fr) >> 8) & 0x0000ff00u;
            d[i] = 0xff000000u | rb | g;
         }
   }
   if (scrolling)
      for (i = 0; i < FOLD_FADE; i++)
      {
         /* the lines sliding in and out fade into the panel instead of being cut off */
         uint32_t a = 255 - (uint32_t)(i * 255 / FOLD_FADE), col = (C_PANEL & 0x00ffffff) | (a << 24);
         gfx_fill_rect(c, 567, ty + i, 260, 1, col);
         gfx_fill_rect(c, 567, ty + rows - 1 - i, 260, 1, col);
      }
}

static void draw_list(canvas_t *c, double now)
{
   fold_sliding = 0;
   fold_wake = 0;
   system_t *s = cur_system();
   char title[700], name[512];
   int i;

   draw_background(c);

   /* header */
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   if (ui.list_all && ui.coll == COLL_SEARCH)
      snprintf(title, sizeof(title), "Search: %s", search_q);
   else if (ui.list_all)
      snprintf(title, sizeof(title), "%s", colls[ui.coll < ncolls ? ui.coll : 0].name);
   else if (ui.folder[0])
      snprintf(title, sizeof(title), "%s  /  %s", s->name, ui.folder);
   else
      snprintf(title, sizeof(title), "%s", s->name);
   gfx_draw_text(c, f_big, title, 20, (50 - font_height(f_big)) / 2, C_TEXT, 600);
   snprintf(name, sizeof(name), "%d / %d", ui.list.n ? ui.sel + 1 : 0, ui.list.n);
   {
      int sw = draw_status(c, c->w - 16, 25, 0);
      gfx_draw_text(c, f_small, name, c->w - 20 - sw - 14 - font_text_width(f_small, name),
            (50 - font_height(f_small)) / 2, C_DIM, 0);
   }

   /* list */
   if (ui.sel < ui.top)
      ui.top = ui.sel;
   if (ui.sel >= ui.top + LIST_ROWS)
      ui.top = ui.sel - LIST_ROWS + 1;
   for (i = 0; i < LIST_ROWS && ui.top + i < ui.list.n; i++)
   {
      const entry_t *e = &ui.list.e[ui.top + i];
      int y = LIST_Y + i * ROW_H, sel = ui.top + i == ui.sel;
      if (sel)
      {
         gfx_fill_rect(c, 12, y, 518, ROW_H, C_SEL);
         gfx_fill_rect(c, 12, y, 4, ROW_H, C_ACCENT);
      }
      display_name(e, name, sizeof(name));   /* (a folder has its small folder picture; no brackets round its name) */
      if (!e->is_dir)
      {
         char key[1300];
         entry_key(e, key, sizeof(key));
         if (is_fav(key))
            draw_star(c, 508, y + (ROW_H - 12) / 2, C_GOLD);
      }
      if (e->is_dir)
      {
         /* a small folder where a game's thumbnail would be */
         int fw = 31, fh = 25;
         draw_folder_shape(c, 23, y + (ROW_H - fh) / 2, fw, fh, TH(TH_PICBOX));
      }
      if (!e->is_dir)
      {
         /* the thumbnail square: the game's picture, or gray */
         const image_t *th = thumb_for(e);
         int ty = y + (ROW_H - THUMB_PX) / 2;
         if (th)
            gfx_draw_image(c, th, 26, ty);
         else if ((th = sysart_get(e->sys, 1)))
            gfx_draw_image(c, th, 26, ty);
         else
            gfx_fill_rect(c, 26, ty, THUMB_PX, THUMB_PX, C_THUMB);
      }
      if (ui.list_all)
      {
         /* mixed list: which console each game is on, small and dim on the right */
         const char *sys = systems[e->sys].name;
         int sw = font_text_width(f_small, sys);
         gfx_draw_text(c, f_small, sys, 500 - sw, y + (ROW_H - font_height(f_small)) / 2, C_FAINT, 0);
         gfx_draw_text(c, f_mid, name, 60, y + (ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 430 - sw - 16);
      }
      else
         gfx_draw_text(c, f_mid, name, 60, y + (ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 436);
   }
   /* scroll bar */
   if (ui.list.n > LIST_ROWS)
   {
      int track = LIST_ROWS * ROW_H, bar = track * LIST_ROWS / ui.list.n;
      gfx_fill_rect(c, 534, LIST_Y, 3, track, C_FAINT);
      gfx_fill_rect(c, 534, LIST_Y + (track - bar) * ui.top / (ui.list.n - LIST_ROWS), 3, bar, C_ACCENT);
   }

   /* details panel */
   gfx_fill_rect(c, 552, LIST_Y, 290, 360, C_PANEL);
   gfx_fill_rect(c, 567, LIST_Y + 15, 260, 195, TH(TH_PICBOX));
   if (ui.list.n)
   {
      const entry_t *e = &ui.list.e[ui.sel];
      const char *dot = strrchr(e->name, '.');
      const image_t *big = e->is_dir ? NULL : large_for(e);
      if (big)
      {
         /* fitted into the box, the right shape */
         double sc = 260.0 / big->w < 195.0 / big->h ? 260.0 / big->w : 195.0 / big->h;
         int bw = (int)(big->w * sc), bh = (int)(big->h * sc);
         gfx_draw_image_smooth(c, big, 697 - bw / 2, LIST_Y + 15 + (195 - bh) / 2, bw, bh);
      }
      else
      {
         if (e->is_dir)
            draw_folder_icon(c);
         else
            draw_no_art_hint(c, e->sys);
      }
      display_name(e, name, sizeof(name));
      gfx_draw_text(c, f_mid, name, 567, LIST_Y + 225, C_TEXT, 260);
      if (e->is_dir)
         draw_folder_contents(c, e, now);
      if (!e->is_dir && dot)
      {
         /* mixed lists name the console; console lists the file type */
         char info[96];
         if (ui.list_all)
            snprintf(info, sizeof(info), "%s", systems[e->sys].name);
         else
            snprintf(info, sizeof(info), "%s file", dot + 1);
         gfx_draw_text(c, f_small, info, 567, LIST_Y + 262, C_DIM, 260);
         draw_emulator(c, e, 567, LIST_Y + 290);
         draw_playtime(c, e, 567, LIST_Y + 318);
      }
   }

   draw_hints(c, ui.list.n && ui.list.e[ui.sel].is_dir
         ? "A  Open folder     B  Back     Dpad  Navigation     L / R  Letter"
         : ui.list_all && ui.coll == COLL_RECENT ? "A  Play     B  Back     Y  Favorite     Start  Remove     X  Search     Dpad  Navigation     L / R  Letter"
         : ui.list_all ? "A  Play     B  Back     Y  Favorite     X  Search     Dpad  Navigation     L / R  Letter"
         : "A  Play     B  Back     Y  Favorite     X  Search     Dpad  Navigation     L / R  Letter");
   draw_toast(c, now);
   draw_volume(c, now);
}


/* ---- the games carousel: its cards and its drawing (the data and the buttons are above, by reload_list).
 * Each game's card is drawn once at full size (the card colour, the game's picture on it, or its console's logo when it has none)
 * and then only scaled, like the home screen's; the last few are kept. */
#define GC_CACHE 48
static struct { cardimg_t ci; uint64_t key; unsigned stamp; } gcard[GC_CACHE];
static unsigned gcard_clock;

static void gcard_clear(void)
{
   int i;
   for (i = 0; i < GC_CACHE; i++)
      gcard[i].key = 0;   /* (each is drawn again when next shown) */
}

static uint64_t gcard_key(const entry_t *e)
{
   uint64_t h = 1469598103934665603ull;
   const char *p;
   for (p = systems[e->sys].id; *p; p++)
      h = (h ^ (uint8_t)*p) * 1099511628211ull;
   h = (h ^ '/') * 1099511628211ull;
   for (p = e->name; *p; p++)
      h = (h ^ (uint8_t)*p) * 1099511628211ull;
   h = (h ^ TH(TH_CARDL_TOP)) * 1099511628211ull;   /* (the colours it is drawn in) */
   h = (h ^ TH(TH_CARDL_BOT)) * 1099511628211ull;
   h = (h ^ TH(TH_TEXT)) * 1099511628211ull;
   return h | 1;
}

/* A picture's corners just barely rounded: the pixels of the card under each corner are kept before the picture goes down
 * and put back (blended along the curve) outside radius r afterwards. r is at most 8. */
static void art_corners_keep(const canvas_t *c, int x, int y, int w, int h, int r, uint32_t keep[4][64])
{
   int k, i, j;
   for (k = 0; k < 4; k++)
      for (j = 0; j < r; j++)
         for (i = 0; i < r; i++)
            keep[k][j * r + i] = c->px[(size_t)((k & 2 ? y + h - r : y) + j) * c->stride + (k & 1 ? x + w - r : x) + i];
}

static void art_corners_round(canvas_t *c, int x, int y, int w, int h, int r, uint32_t keep[4][64])
{
   int k, i, j;
   double r1 = r - 1.0;
   for (k = 0; k < 4; k++)
   {
      int x0 = k & 1 ? x + w - r : x, y0 = k & 2 ? y + h - r : y;
      double cx = k & 1 ? x + w - r : x + r, cy = k & 2 ? y + h - r : y + r;
      for (j = 0; j < r; j++)
         for (i = 0; i < r; i++)
         {
            double dx = x0 + i + 0.5 - cx, dy = y0 + j + 0.5 - cy, d2 = dx * dx + dy * dy, d;
            uint32_t *dst;
            if (((k & 1) ? dx < 0 : dx > 0) || ((k & 2) ? dy < 0 : dy > 0) || d2 <= r1 * r1)
               continue;   /* (inside the curve, or on the straight part of the edge) */
            d = sqrt(d2);
            dst = &c->px[(size_t)(y0 + j) * c->stride + x0 + i];
            *dst = blend256(*dst, keep[k][j * r + i], d >= r ? 256 : (unsigned)((d - r1) * 256));
         }
   }
}

static void gcard_make(cardimg_t *ci, const entry_t *e)
{
   canvas_t cv;
   const image_t *big;
   uint32_t top = TH(TH_CARDL_TOP), bot = TH(TH_CARDL_BOT);
   if (!ci->img.px)
   {
      ci->img.w = ci->img.h = GS;
      ci->img.px = malloc((size_t)GS * GS * 4);
      if (!ci->img.px)
         return;
   }
   cv.w = cv.h = cv.stride = GS;
   cv.px = ci->img.px;
   {
      /* the card colour with its shading is the same for every game: made once (again when the colours change) and copied */
      static uint32_t *base, base_top, base_bot;
      if (!base || base_top != top || base_bot != bot)
      {
         canvas_t bc = cv;
         free(base);
         base = malloc((size_t)GS * GS * 4);
         if (base)
         {
            bc.px = base;
            fill_vgradient(&bc, 0, 0, GS, GS, top, bot);
            card_depth_sq(&bc, bot, 256);
            base_top = top;
            base_bot = bot;
         }
      }
      if (base)
         memcpy(cv.px, base, (size_t)GS * GS * 4);
      else
      {
         fill_vgradient(&cv, 0, 0, GS, GS, top, bot);
         card_depth_sq(&cv, bot, 256);
      }
   }
   big = large_for(e);
   if (big)
   {
      /* the picture, as large as fits inside the card with a margin, in its own shape, on a soft shadow */
      int box = GS - 28, bw, bh, x, y, r = 6;
      uint32_t keep[4][64];
      double sc = (double)box / (big->w > big->h ? big->w : big->h);
      bw = (int)(big->w * sc + 0.5);
      bh = (int)(big->h * sc + 0.5);
      x = (GS - bw) / 2;
      y = (GS - bh) / 2;
      fill_round_rect_alpha(&cv, x - 2, y + 2, bw + 4, bh + 4, r + 2, 0xff000000, 60);
      fill_round_rect_alpha(&cv, x, y + 4, bw, bh, r, 0xff000000, 90);
      if (bw >= 2 * r && bh >= 2 * r)
         art_corners_keep(&cv, x, y, bw, bh, r, keep);
      gfx_draw_image_smooth(&cv, big, x, y, bw, bh);
      if (bw >= 2 * r && bh >= 2 * r)
         art_corners_round(&cv, x, y, bw, bh, r, keep);
   }
   else
   {
      /* no picture: the console's logo, as on the cards of the Consoles row */
      char path[300];
      image_t *logo = NULL;
      snprintf(path, sizeof(path), "%s/%s.png", LOGO_DIR, systems[e->sys].id);
      if (access(path, F_OK) == 0)
         logo = image_load(path);
      if (logo)
      {
         int lw = logo->w > GS - 44 ? GS - 44 : logo->w, lh = logo->h * lw / logo->w, x, y;
         if (lh > 100)
         {
            lw = lw * 100 / lh;
            lh = 100;
         }
         x = GS / 2 - lw / 2;
         y = (GS - lh) / 2 - 6;
         cast_logo_shadow(&cv, logo, x, y, lw, lh, 5, 8, 4, mix_colour(bot, 0xff000000, 200), 190);
         gfx_draw_image_scaled(&cv, logo, x, y, lw, lh);
         image_free(logo);
      }
      else
         draw_centered(&cv, f_mid, systems[e->sys].name, GS / 2, GS / 2 - font_height(f_mid) / 2, C_TEXT);
   }
   if (ci->half)
      image_free(ci->half);
   if (ci->scaled)
      image_free(ci->scaled);
   ci->half = image_half(&ci->img);
   ci->scaled = NULL;
   ci->last_w = ci->last_h = 0;
}

/* a game's card picture; with make = 0 only one that is already made. NULL when there is none yet (making one that
 * would overrun the frame's time is left for the next frame). */
static cardimg_t *gcard_get(const entry_t *e, int make)
{
   uint64_t key = gcard_key(e);
   int i, victim = 0;
   for (i = 0; i < GC_CACHE; i++)
      if (gcard[i].key == key && gcard[i].ci.img.px)
      {
         gcard[i].stamp = ++gcard_clock;
         return &gcard[i].ci;
      }
   if (!make)
      return NULL;
   if (card_spent > CARD_BUDGET)
   {
      card_pending = 1;
      return NULL;
   }
   for (i = 1; i < GC_CACHE; i++)
      if (gcard[i].stamp < gcard[victim].stamp)
         victim = i;
   {
      double t0 = now_s();
      cardimg_t *ci = &gcard[victim].ci;
      gcard_make(ci, e);
      gcard[victim].key = ci->img.px ? key : 0;
      gcard[victim].stamp = ++gcard_clock;
      card_spent += now_s() - t0;
      return ci->img.px ? ci : NULL;
   }
}

/* While the menu sits still, the cards that could come into view next are made (a few a frame, the nearest first: along the row
 * you are on, then the rows above and below), so moving never has to make one in the middle of a slide. 1 if one was made. */
static int gcard_prewarm(void)
{
   int dr, sgn, d, side, made = 0;
   if (!gc.on)
      return 0;
   for (dr = 0; dr <= 2; dr++)
      for (sgn = -1; sgn <= 1; sgn += 2)
      {
         int rr, span = dr ? 3 : 4;
         const grow_t *g;
         if (dr == 0 && sgn > 0)
            continue;
         rr = wrap(gc.row_pos + sgn * dr, gc.nrow);
         g = &gc.row[rr];
         for (d = 0; d <= span; d++)
            for (side = -1; side <= 1; side += 2)
            {
               const entry_t *e;
               if (d == 0 && side > 0)
                  continue;
               e = &ui.list.e[g->first + wrap(gc.pos[rr] + side * d, g->n)];
               if (gcard_get(e, 0))
                  continue;
               gcard_get(e, 1);
               made = 1;
               if (card_spent > 0.012)
                  return 1;
            }
      }
   return made;
}

/* a card at some size on the screen, as draw_card does for the home screen's */
static void draw_gcard(canvas_t *c, cardimg_t *ci, int cx, int cy, double scale, int focus)
{
   int w = (int)(GS * scale), h = w, x = cx - w / 2, y = cy - h / 2, slab_t = (int)(CARD_SLAB * scale + 0.5), r = (int)(28 * scale);
   uint32_t bot = TH(TH_CARDL_BOT);
   if (w < 8)
      return;
   card_shadow(c, x, y + slab_t, w, h, r, focus);
   if (w == GS)
      gfx_draw_image_smooth(c, &ci->img, x, y, w, h);              /* full size: a copy */
   else if (ci->scaled && ci->scaled->w == w && ci->scaled->h == h)
      gfx_draw_image_smooth(c, ci->scaled, x, y, w, h);            /* resting: the kept copy */
   else if (w == ci->last_w && h == ci->last_h && smooth_budget > 0)
   {
      smooth_budget--;
      if (ci->scaled)
         image_free(ci->scaled);
      ci->scaled = image_scaled(&ci->img, w, h);
      gfx_draw_image_smooth(c, ci->scaled ? ci->scaled : &ci->img, x, y, w, h);
   }
   else
      gfx_draw_image_fast(c, ci->half && w <= ci->half->w ? ci->half : &ci->img, x, y, w, h);
   ci->last_w = w;
   ci->last_h = h;
   round_corners(c, x, y, w, h, r);
   stroke_round_rect(c, x, y, w, h, r, TH(TH_CARD_LINE));
   card_slab(c, x, y, w, h, r, slab_t, mix_colour(bot, 0xff000000, 110), mix_colour(bot, 0xff000000, 200));
}

/* a card whose picture isn't made (the rows above and below only peek in, so they don't make theirs): the plain card */
static void draw_gplain(canvas_t *c, int cx, int cy, double scale)
{
   int w = (int)(GS * scale), x = cx - w / 2, y = cy - w / 2, r = (int)(28 * scale);
   if (w < 8)
      return;
   fill_round_rect_vgrad(c, x, y, w, w, r, TH(TH_CARDL_TOP), TH(TH_CARDL_BOT));
   stroke_round_rect(c, x, y, w, w, r, TH(TH_CARD_LINE));
}

#define GC_ROW_GAP 262     /* between rows: the ones above and below show only a sliver at the screen's edges */

/* how far from the middle the cards sit: the two beside the chosen one, then the outer two coming in at the screen's edges */
static double gc_x(double a)
{
   return a <= 1 ? 240 * a : a <= 2 ? 240 + 214 * (a - 1) : 454 + 196 * (a - 2);
}

static double gc_scale_y(double dy)
{
   return 1 - 0.38 * (dy < 1 ? dy : 1);
}

typedef struct
{
   int r, i, cx, cy;
   double scale, depth, d;
   cardimg_t *ci;
} gcdraw_t;

static int gc_cmp(const void *a, const void *b)
{
   const gcdraw_t *x = a, *y = b;
   return x->depth < y->depth ? 1 : x->depth > y->depth ? -1 : 0;
}

/* the text cut to a width, with "..." in the middle... at the end */
static void fit_text(font_t *f, const char *in, int maxw, char *out, size_t sz)
{
   size_t len;
   snprintf(out, sz, "%s", in);
   len = strlen(out);
   while (len > 1 && font_text_width(f, out) > maxw)
   {
      len--;
      while (len > 1 && ((unsigned char)out[len] & 0xc0) == 0x80)
         len--;
      memcpy(out + len, "\xe2\x80\xa6", 4);
   }
}

static void draw_games_carousel(canvas_t *c, double now)
{
   static gcdraw_t cards[64];
   static struct { char key[220]; image_t img; } pill;
   int n = 0, rr, k, i, nrow = gc.nrow, frow = wrap(gc.row_pos, nrow), fcy = HOME_CY;
   const entry_t *fe = &ui.list.e[ui.sel];
   double fd_min = 9;
   char text[260];

   fold_sliding = 0;
   fold_wake = 0;
   card_pending = 0;
   smooth_budget = 2;
   bg_stage_on = 1;
   draw_background(c);
   bg_stage_on = 0;

   for (rr = 0; rr < nrow; rr++)
   {
      grow_t *g = &gc.row[rr];
      double d = fmod(rr - gc.row_anim, nrow), sy;
      int cy, cnt = g->n, m = 0, idx[9];
      double off[9];
      if (d < -nrow / 2.0)
         d += nrow;
      if (d >= nrow / 2.0)
         d -= nrow;
      if (fabs(d) >= 1.4)
         continue;
      if (fabs(d) < fd_min)
      {
         fd_min = fabs(d);
         if (rr == frow)
            fcy = HOME_CY + (int)(d * GC_ROW_GAP);
      }
      sy = gc_scale_y(fabs(d));
      cy = HOME_CY + (int)(d * GC_ROW_GAP);
      if (cnt <= 9)
         for (i = 0; i < cnt; i++)
         {
            double o = fmod(i - gc.anim[rr], cnt);
            if (o < -cnt / 2.0)
               o += cnt;
            if (o >= cnt / 2.0)
               o -= cnt;
            idx[m] = i;
            off[m++] = o;
         }
      else
      {
         int base = (int)floor(gc.anim[rr]);
         for (k = -4; k <= 4; k++)
         {
            idx[m] = wrap(base + k, cnt);
            off[m++] = base + k - gc.anim[rr];
         }
      }
      for (k = 0; k < m && n < 64; k++)
      {
         double a = fabs(off[k]), sc = card_scale_x(a) * sy;
         int cx = c->w / 2 + (int)(gc_x(a) * sy * (off[k] < 0 ? -1 : 1) + (off[k] < 0 ? -0.5 : 0.5));
         if (cx + GS * sc / 2 < 0 || cx - GS * sc / 2 > c->w)
            continue;
         cards[n].r = rr;
         cards[n].i = g->first + idx[k];
         cards[n].cx = cx;
         cards[n].cy = cy;
         cards[n].scale = sc;
         cards[n].depth = fabs(d) * 10 + a;
         cards[n].d = d;
         cards[n].ci = NULL;
         n++;
      }
   }
   qsort(cards, n, sizeof(gcdraw_t), gc_cmp);   /* far ones first, the chosen card on top */

   /* the pictures, the nearest cards first (the row you are on makes its own; the others only use what is there) */
   for (i = n - 1; i >= 0; i--)
      cards[i].ci = gcard_get(&ui.list.e[cards[i].i], fabs(cards[i].d) < 0.5);

   /* the chosen card's reflection goes down first (the cards in front cover it where they meet) */
   for (i = 0; i < n; i++)
      if (cards[i].depth < 0.5 && cards[i].ci)
      {
         int w = (int)(GS * cards[i].scale);
         card_reflection(c, cards[i].ci, cards[i].cx - w / 2, cards[i].cy - w / 2, w, w, (int)(28 * cards[i].scale), (int)(CARD_SLAB * cards[i].scale + 0.5));
      }

   /* the glint on the card that has just been chosen */
   if (gc.glint_sel != ui.sel)
   {
      gc.glint_sel = ui.sel;
      gc.glint_t0 = now;
      gc.glint_until = theme_glint ? now + 0.7 : 0;
   }
   for (i = 0; i < n; i++)
   {
      const gcdraw_t *k2 = &cards[i];
      int focus = k2->depth < 0.5, w = (int)(GS * k2->scale);
      if (k2->ci)
         draw_gcard(c, k2->ci, k2->cx, k2->cy, k2->scale, focus);
      else
         draw_gplain(c, k2->cx, k2->cy, k2->scale);
      if (focus && theme_glint && now >= gc.glint_t0 && now < gc.glint_t0 + 0.6)
         card_glint(c, k2->cx - w / 2, k2->cy - w / 2, w, w, (int)(28 * k2->scale), (now - gc.glint_t0) / 0.6);
   }

   {
      /* the twinkling stars, as on the home screen: kept off the chosen row's cards and the writing drawn over them afterwards
       * (a quiet frame puts back what a star covered, which would take those pixels back too) */
      rect_t *ex = star_ex;
      int nex = 0, th = font_height(f_label ? f_label : f_mid);
      for (i = 0; i < n && nex < 24; i++)
         if (cards[i].depth < 0.5 || fabs(cards[i].d) < 0.5)
         {
            int w = (int)(GS * cards[i].scale);
            ex[nex].x = cards[i].cx - w / 2;
            ex[nex].y = cards[i].cy - w / 2;
            ex[nex].w = w + CARD_SLAB;
            ex[nex].h = w + CARD_SLAB;
            nex++;
         }
      if (fd_min < 0.5)
      {
         ex[nex].x = c->w / 2 - 330;
         ex[nex].w = 660;
         ex[nex].y = fcy - GS / 2 - font_height(f_label ? f_label : f_title ? f_title : f_small) - 20;
         ex[nex].h = 40;
         nex++;
         ex[nex].x = c->w / 2 - 300;
         ex[nex].w = 600;
         ex[nex].y = fcy + GS / 2 + CARD_SLAB;
         ex[nex].h = 2 * th + font_height(f_small) + 32;
         nex++;
      }
      ex[nex].x = 560;
      ex[nex].y = 0;
      ex[nex].w = c->w - 560;
      ex[nex].h = 56;
      nex++;
      star_nex = nex;
      stars_draw(c, now, ex, nex);
   }

   if (fd_min < 0.5)
   {
      /* the row's name above the chosen card, and under it the game's name in bold and where in the row it is */
      font_t *tf = f_label ? f_label : f_title ? f_title : f_small, *nf = f_label ? f_label : f_mid;
      int ph = font_height(tf) + 6, pw, py0, ty, line_h = font_height(nf), cx = c->w / 2;
      titlepill_t tp;
      fit_text(tf, gc.row[frow].title, 560, text, sizeof(text));
      pw = font_text_width(tf, text) + 48;
      py0 = fcy - GS / 2 - ph - 5;
      snprintf(pill.key, sizeof(pill.key), "%s,%d,%d,%06x", text, pw, ph, C_ACCENT & 0xffffff);
      tp.tf = tf;
      tp.title = text;
      tp.pw = pw;
      tp.ph = ph;
      {
         static char last[220];
         if (strcmp(last, pill.key) || !pill.img.px)
         {
            sprite_from_draw(&pill.img, pw + 16, ph + 18, row_title_pill, &tp);
            snprintf(last, sizeof(last), "%s", pill.key);
         }
      }
      if (pill.img.px)
         sprite_blit(c, &pill.img, cx - pw / 2 - 8, py0 - 4);

      ty = fcy + GS / 2 + CARD_SLAB + 12;
      {
         char name[512];
         int tw, maxw = 560, fav;
         {
            /* a favorite has a small gold star to the left of its name */
            char key[1300];
            entry_key(fe, key, sizeof(key));
            fav = is_fav(key);
         }
         display_name(fe, name, sizeof(name));
         tw = font_text_width(nf, name);
         if (tw <= maxw)
         {
            gfx_draw_text(c, nf, name, cx - tw / 2, ty, C_TEXT, maxw);
            if (fav)
               draw_star_big(c, cx - tw / 2 - 20, ty + line_h / 2, C_GOLD);
            ty += line_h;
         }
         else
         {
            /* two lines, split at the space nearest the middle */
            int len = (int)strlen(name), best = -1, w1, w2;
            char l1[256], l2[256];
            for (k = 0; k < len; k++)
               if (name[k] == ' ' && (best < 0 || abs(k - len / 2) < abs(best - len / 2)))
                  best = k;
            if (best < 0)
               best = len;
            snprintf(l1, sizeof(l1), "%.*s", best, name);
            snprintf(l2, sizeof(l2), "%s", best < len ? name + best + 1 : "");
            w1 = font_text_width(nf, l1);
            w2 = font_text_width(nf, l2);
            gfx_draw_text(c, nf, l1, cx - (w1 > maxw ? maxw : w1) / 2, ty, C_TEXT, maxw);
            if (fav)
               draw_star_big(c, cx - (w1 > maxw ? maxw : w1) / 2 - 20, ty + line_h / 2, C_GOLD);
            gfx_draw_text(c, nf, l2, cx - (w2 > maxw ? maxw : w2) / 2, ty + line_h, C_TEXT, maxw);
            ty += 2 * line_h;
         }
         snprintf(name, sizeof(name), "%d / %d", wrap(gc.pos[frow], gc.row[frow].n) + 1, gc.row[frow].n);
         draw_centered(c, f_small, name, cx, ty + 2, C_DIM);
      }
   }
   draw_status(c, c->w - 16, 25, 0);   /* (the corner, as on the other screens: a long row name takes the middle) */
   draw_toast(c, now);
   draw_volume(c, now);
   draw_hints(c, ui.list_all && ui.coll == COLL_RECENT
         ? "A  Play     B  Back     Y  Favorite     Start  Remove     X  Search     Dpad  Navigation     L / R  Letter"
         : ui.list_all && ui.coll == COLL_SEARCH
         ? "A  Play     B  Back     Y  Favorite     X  Search     Dpad  Navigation     L / R  Letter"
         : "A  Play     B  Back     Y  Favorite     X  Search     Dpad  Navigation     L / R  Letter");
}

/* ------------------------------------------------------------- settings screen */
#define G36_VERSION "1.1"
#define BACKLIGHT   BOARD_BACKLIGHT

/* write "key = value" into g36/settings.cfg, keeping everything else as it is */
static void set_setting(const char *key, int value)
{
   char **lines = NULL, line[512], tmp[300];
   int n = 0, cap = 0, i, done = 0;
   size_t kl = strlen(key);
   FILE *f = fopen(SETTINGS, "r");
   if (f)
   {
      while (fgets(line, sizeof(line), f))
      {
         char *p = line;
         while (*p == ' ' || *p == '\t')
            p++;
         if (!strncmp(p, key, kl) && (p[kl] == ' ' || p[kl] == '\t' || p[kl] == '='))
         {
            snprintf(line, sizeof(line), "%s = %d\n", key, value);
            done = 1;
         }
         if (n == cap)
         {
            cap = cap ? cap * 2 : 32;
            lines = realloc(lines, cap * sizeof(char *));
         }
         lines[n++] = strdup(line);
      }
      fclose(f);
   }
   snprintf(tmp, sizeof(tmp), "%s.tmp", SETTINGS);
   if ((f = fopen(tmp, "w")))
   {
      for (i = 0; i < n; i++)
         fputs(lines[i], f);
      if (!done)
         fprintf(f, "%s = %d\n", key, value);
      if (commit_close(f) == 0)
         rename(tmp, SETTINGS);
      else
         unlink(tmp);
   }
   for (i = 0; i < n; i++)
      free(lines[i]);
   free(lines);
}

static void apply_brightness(int pct)
{
   FILE *f;
   if (pct < 10 || pct > 100 || !(f = fopen(BACKLIGHT, "w")))
      return;
   fprintf(f, "%d\n", pct * 255 / 100);
   fclose(f);
}

/* the volume through the sound system, saved where the factory helper keeps it */
static void set_volume(int pct)
{
   long lo, hi;
   char buf[2048], out[2100], *p, *q;
   FILE *f;
   if (!mixer)
      mixer_open();
   if (master)
   {
      snd_mixer_selem_get_playback_volume_range(master, &lo, &hi);
      snd_mixer_selem_set_playback_volume_all(master, lo + (hi - lo) * pct / 100);
   }
   if (!(f = fopen(GAMEBOX_JSON, "r")))
      return;
   buf[fread(buf, 1, sizeof(buf) - 1, f)] = 0;
   fclose(f);
   if (!(p = strstr(buf, "\"volume\"")) || !(q = strchr(p, ':')))
      return;
   for (q++; *q == ' '; q++)
      ;
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

enum { S_HEAD, S_CHOICE, S_ACTION, S_INFO };
enum { A_NONE, A_CLOCK, A_RESCAN, A_RESTART, A_POWEROFF, A_ABOUT, A_DEFAULTS, A_SLEEP };

static const int v_bright[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static const int v_vol[]    = { 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
static const int v_scr[]    = { 0, 30, 60, 120, 180, 300 };
static const int v_pow[]    = { 0, 5, 10, 15, 30, 60 };
static const int v_warn[]   = { 10, 15, 20, 25, 30 };
static const int v_off[]    = { 5, 10, 15, 20 };
static const int v_clk[]    = { 0, 1 };
static const int v_onoff[]  = { 0, 1 };
static const int v_sbright[] = { -1, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };   /* -1 = last used */
static const int v_svol[]    = { -1, 0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };

typedef struct
{
   int kind;
   const char *label, *key;   /* key: in settings.cfg (NULL: handled specially) */
   const int *vals;
   int nvals, def, action;
} srow_t;

static const srow_t srows[] = {
   { S_ACTION, "About This Device", NULL, NULL, 0, 0, A_ABOUT },
   { S_HEAD,   "DISPLAY", NULL, NULL, 0, 0, 0 },
   { S_CHOICE, "Brightness", "brightness_percent", v_bright, 10, 50, 0 },
   { S_CHOICE, "Brightness at start-up", "startup_brightness", v_sbright, 11, -1, 0 },
   { S_CHOICE, "Borders around games", "game_borders", v_onoff, 2, 1, 0 },
   { S_CHOICE, "FPS counter in games", "fps_show", v_onoff, 2, 1, 0 },
   { S_HEAD,   "SOUND", NULL, NULL, 0, 0, 0 },
   { S_CHOICE, "Volume", NULL, v_vol, 11, 50, 0 },
   { S_CHOICE, "Volume at start-up", "startup_volume", v_svol, 12, -1, 0 },
   { S_CHOICE, "Menu click volume", "click_volume", v_vol, 11, 50, 0 },
   { S_HEAD,   "POWER", NULL, NULL, 0, 0, 0 },
   { S_CHOICE, "Screen off after", "screen_off_seconds", v_scr, 6, 180, 0 },
   { S_CHOICE, "Power off after", "auto_power_off_minutes", v_pow, 6, 10, 0 },
   { S_HEAD,   "BATTERY", NULL, NULL, 0, 0, 0 },
   { S_CHOICE, "Low battery warning at", "battery_warn_percent", v_warn, 5, 20, 0 },
   { S_CHOICE, "Save and power off at", "battery_off_percent", v_off, 4, 10, 0 },
   { S_HEAD,   "CLOCK", NULL, NULL, 0, 0, 0 },
   { S_ACTION, "Set date and time", NULL, NULL, 0, 0, A_CLOCK },
   { S_CHOICE, "Clock format", "clock_24h", v_clk, 2, 0, 0 },
   { S_HEAD,   "LIBRARY", NULL, NULL, 0, 0, 0 },
   { S_ACTION, "Rescan the library", NULL, NULL, 0, 0, A_RESCAN },
   { S_HEAD,   "SYSTEM", NULL, NULL, 0, 0, 0 },
   { S_CHOICE, "USB debugging (ADB)", "usb_debugging", v_onoff, 2, 0, 0 },
   { S_ACTION, "Restore default settings", NULL, NULL, 0, 0, A_DEFAULTS },
   { S_ACTION, "Sleep", NULL, NULL, 0, 0, A_SLEEP },
   { S_ACTION, "Restart", NULL, NULL, 0, 0, A_RESTART },
   { S_ACTION, "Power off", NULL, NULL, 0, 0, A_POWEROFF },
};
#define NSROWS ((int)(sizeof(srows) / sizeof(*srows)))

static struct
{
   int sel, top;
   int val[NSROWS];        /* current choice values */
   int editing_clock;
   int about;              /* the About This Device page is open */
   int field;              /* clock editor: 0 year .. 4 minute */
   struct tm tm;
} st;

/* Restart only shows with outside power (on battery a restart can only power off) */
static int srow_shown(int i)
{
   return srows[i].action != A_RESTART || st_power;
}

static void switch_prewarm(void);
static void settings_open(void)
{
   int i;
   for (i = 0; i < NSROWS; i++)
      if (srows[i].kind == S_CHOICE)
         st.val[i] = srows[i].key ? setting_int(srows[i].key, srows[i].def) : read_volume();
   if (st.sel < 0 || st.sel >= NSROWS || srows[st.sel].kind == S_HEAD || !srow_shown(st.sel))
      st.sel = 0;
   st.about = 0;
   st.top = 0;
   st.editing_clock = 0;
   switch_prewarm();
}

static void settings_move(int dir)
{
   int i = st.sel, k;
   for (k = 0; k < NSROWS; k++)
   {
      i = (i + dir + NSROWS) % NSROWS;
      if (srows[i].kind != S_HEAD && srows[i].kind != S_INFO && srow_shown(i))
         break;
   }
   st.sel = i;
}

/* left/right on a choice: the next value (no wrapping) */
/* what a setting's new value does at once (the rest are read when needed) */
static void setting_apply(const char *key, int v)
{
   if (!strcmp(key, "brightness_percent"))
      apply_brightness(v);
   else if (!strcmp(key, "screen_off_seconds"))
      screen_off_s = v;
   else if (!strcmp(key, "auto_power_off_minutes"))
      power_off_min = v;
   else if (!strcmp(key, "clock_24h"))
      clock_24h = v;
   else if (!strcmp(key, "click_volume"))
      kc.vol = v;
   else if (!strcmp(key, "usb_debugging"))
      filetransfer("refresh");   /* USB is set up again, with or without ADB (a couple of seconds) */
}

/* every choice back to its default (the date and time, and everything outside Settings,
 * is left alone) */
static void settings_defaults(void)
{
   int i;
   for (i = 0; i < NSROWS; i++)
   {
      const srow_t *r = &srows[i];
      if (r->kind != S_CHOICE)
         continue;
      if (r->key && !strcmp(r->key, "usb_debugging"))
         continue;   /* a developer switch: Restore defaults doesn't cut your connection */
      st.val[i] = r->def;
      if (r->key)
      {
         set_setting(r->key, r->def);
         setting_apply(r->key, r->def);
      }
      else
         set_volume(r->def);
   }
   mlog("settings: back to the defaults");
}

static void settings_change(int dir)
{
   const srow_t *r = &srows[st.sel];
   int i, k = 0, v;
   if (r->kind != S_CHOICE)
      return;
   for (i = 0; i < r->nvals; i++)
      if (r->vals[i] <= st.val[st.sel])
         k = i;
   k += dir;
   if (k < 0 || k >= r->nvals)
      return;
   v = st.val[st.sel] = r->vals[k];
   if (!r->key)
   {
      set_volume(v);
      return;
   }
   set_setting(r->key, v);
   setting_apply(r->key, v);
   if (!strcmp(r->key, "click_volume"))
      key_click();   /* hear what it sounds like */
   else if (!strcmp(r->key, "battery_off_percent") || !strcmp(r->key, "battery_warn_percent"))
   {
      /* the warning always comes before the power-off */
      int wi = 0, oi = 0, k;
      for (k = 0; k < NSROWS; k++)
         if (srows[k].key && !strcmp(srows[k].key, "battery_warn_percent"))
            wi = k;
         else if (srows[k].key && !strcmp(srows[k].key, "battery_off_percent"))
            oi = k;
      if (st.val[oi] >= st.val[wi])
      {
         if (st.sel == oi)
         {
            st.val[wi] = st.val[oi] + 5 > 30 ? 30 : st.val[oi] + 5;
            set_setting("battery_warn_percent", st.val[wi]);
         }
         else
         {
            st.val[oi] = st.val[wi] - 5 < 5 ? 5 : st.val[wi] - 5;
            set_setting("battery_off_percent", st.val[oi]);
         }
      }
   }
}

/* Left / right on the Menu click volume row: the setting plays its own click (at the new
 * level), so the usual click for the button is left out. */
static int click_volume_adjust(int code)
{
   return ui.view == 2 && !st.about && !st.editing_clock && (code == KEY_LEFT || code == KEY_RIGHT) &&
         srows[st.sel].key && !strcmp(srows[st.sel].key, "click_volume");
}

/* The same with a colour for every row (the other one steps in a few bands, which shows on a shape this small) */
static void fill_round_rect_vgrad_smooth(canvas_t *c, int x, int y, int w, int h, int r, uint32_t top_col, uint32_t bot_col)
{
   canvas_t sub = *c;
   int j;
   for (j = 0; j < h; j++)
   {
      if (y + j < 0 || y + j >= c->h)
         continue;
      sub.px = c->px + (size_t)(y + j) * c->stride;
      sub.h = 1;
      fill_round_rect(&sub, x, -j, w, h, r, mix_colour(top_col, bot_col, h > 1 ? 256 * j / (h - 1) : 0));
   }
}

/* A phone-style switch for the On/Off settings: a pill (the accent colour with the knob at the right when on, a dull grey
 * with the knob at the left when off) whose knob slides across when the setting changes. `id` picks the remembered state
 * for each switch on screen (0 Glint, 1 Translucent pill, 2 Translucent bar, 3 USB debugging); `right` is its right edge. */
#define SW_W 52
#define SW_H 28
#define SW_SLIDE 0.16
static double switch_until;   /* the main loop keeps drawing at full rate until then */
/* One switch drawn live with its track's top left at (x, y), the knob `p` of the way across (0 off, 1 on) */
static void switch_live(canvas_t *c, int x, int y, double p, int sel)
{
   /* lit like the cards: the track is sunk into the panel (dark at the top where the rim shadows it, lighter below, a
    * light lip on its lower edge), glowing in the accent when on; the knob is a dome (white at the top, a soft grey
    * below, a gloss over its upper half) standing on a layered shadow */
   int pk = (int)(p * 256), r = SW_H / 2, d = SW_H - 6, kx;
   uint32_t acc = C_ACCENT;
   uint32_t t_top = mix_colour(mix_colour(0xff25222c, 0xff000000, 40), mix_colour(acc, 0xff000000, 150), pk);
   uint32_t t_bot = mix_colour(0xff55505f, mix_colour(acc, 0xffffffff, 60), pk);
   if (p > 0.02)
      fill_round_rect_alpha(c, x - 3, y - 3, SW_W + 6, SW_H + 6, r + 3, acc, (int)(p * 55));   /* the glow */
   fill_round_rect_alpha(c, x, y + 1, SW_W, SW_H, r, 0xffffffff, sel ? 120 : 55);                 /* the lip below the rim */
   fill_round_rect(c, x - 1, y - 1, SW_W + 2, SW_H + 1, r + 1, sel ? 0xffe6e2ec : 0xff14121a);    /* the rim */
   fill_round_rect_vgrad_smooth(c, x, y, SW_W, SW_H - 1, r, t_top, t_bot);
   kx = x + 3 + (int)(p * (SW_W - SW_H) + 0.5);
   fill_round_rect_alpha(c, kx - 2, y + 4, d + 4, d + 4, d / 2 + 2, 0xff000000, 40);              /* its shadow, wide and soft */
   fill_round_rect_alpha(c, kx - 1, y + 4, d + 2, d + 2, d / 2 + 1, 0xff000000, 70);              /* and close and darker */
   fill_round_rect(c, kx, y + 3, d, d, d / 2, 0xff8f8a9c);                                        /* the knob's edge */
   fill_round_rect_vgrad_smooth(c, kx + 1, y + 4, d - 2, d - 3, d / 2 - 1, 0xffffffff, 0xffc4c0cf);
   fill_round_rect_alpha(c, kx + 4, y + 5, d - 8, d / 2 - 2, d / 4, 0xffffffff, 150);             /* the gloss */
}

/* Every position of a switch is a cached picture with see-through edges (the glow and shadows fade out into whatever is
 * behind): SW_STEPS + 1 knob positions, for a chosen switch (the one being changed, so the slide is a run of pictures) and a
 * plain one (only off and on). Each is made when first needed, and again if the accent colour changes. Drawing one live
 * costs about 15 shaded shapes (0.85 ms). A picture's alpha comes from drawing the switch twice, on black and on white:
 * what's behind shows through by (white - black), and the colour is what's left of the black one. */
#define SW_PAD 6
#define SW_STEPS 12
static image_t sw_spr[2][SW_STEPS + 1];
static uint32_t sw_spr_acc;

static void switch_sprite(int sel, int k)
{
   static uint32_t *bk, *wh;
   int sw = SW_W + 2 * SW_PAD, sh = SW_H + 2 * SW_PAD, i, n = sw * sh;
   image_t *im = &sw_spr[sel][k];
   canvas_t cb = { sw, sh, sw, NULL }, cw = { sw, sh, sw, NULL };
   if (!bk)
   {
      bk = malloc((size_t)n * 4);
      wh = malloc((size_t)n * 4);
   }
   if (!bk || !wh)
      return;
   if (!im->px)
   {
      im->px = malloc((size_t)n * 4);
      im->w = sw;
      im->h = sh;
   }
   if (!im->px)
      return;
   cb.px = bk;
   cw.px = wh;
   for (i = 0; i < n; i++)
   {
      bk[i] = 0xff000000;
      wh[i] = 0xffffffff;
   }
   switch_live(&cb, SW_PAD, SW_PAD, (double)k / SW_STEPS, sel);
   switch_live(&cw, SW_PAD, SW_PAD, (double)k / SW_STEPS, sel);
   for (i = 0; i < n; i++)
   {
      int q, diff = 0, a;
      uint32_t out = 0;
      for (q = 0; q <= 16; q += 8)
         diff += 255 - (int)((wh[i] >> q & 255) - (bk[i] >> q & 255));
      a = diff / 3;   /* how much of what's behind is hidden */
      a = a < 0 ? 0 : a > 255 ? 255 : a;
      if (a > 0)
         for (q = 0; q <= 16; q += 8)
         {
            int cc = (int)(bk[i] >> q & 255) * 255 / a;
            out |= (uint32_t)(cc > 255 ? 255 : cc) << q;
         }
      im->px[i] = out | (uint32_t)a << 24;
   }
}


/* the pictures are made with the accent colour: when it changes they're all dropped (and made again as needed) */
static void switch_check_accent(void)
{
   int q, v;
   if (sw_spr_acc == C_ACCENT)
      return;
   for (v = 0; v < 2; v++)
      for (q = 0; q <= SW_STEPS; q++)
      {
         free(sw_spr[v][q].px);
         sw_spr[v][q].px = NULL;
      }
   sw_spr_acc = C_ACCENT;
}

/* Made ahead, when a screen with switches opens (about 25 ms, once): every position of a chosen switch, off and on of the
 * others, so no slide or first draw has to make one */
static void switch_prewarm(void)
{
   int q, v, made = 0;
   double t0 = now_s();
   switch_check_accent();
   for (v = 0; v < 2; v++)
      for (q = 0; q <= SW_STEPS; q++)
         if (!sw_spr[v][q].px && (v || q == 0 || q == SW_STEPS))
         {
            switch_sprite(v, q);
            made++;
         }
   if (made)
      mlog("switch pictures: %d made in %d ms", made, (int)((now_s() - t0) * 1000));
}

static void draw_switch(canvas_t *c, int id, int right, int cy, int on, int sel, double now)
{
   static struct { int on; double t, seen; } sw[10];
   int x = right - SW_W, y = cy - SW_H / 2, k;
   double p = on ? 1 : 0, dt;
   if (now - sw[id].seen > 0.3 || now < sw[id].seen)
   {
      sw[id].on = on;   /* just came on screen: no slide */
      sw[id].t = -10;
   }
   sw[id].seen = now;
   if (sw[id].on != on)
   {
      sw[id].on = on;
      sw[id].t = now;
   }
   dt = (now - sw[id].t) / SW_SLIDE;
   if (dt < 1)
   {
      /* sliding: the knob part of the way across */
      dt = dt < 0 ? 0 : dt;
      p = dt * dt * (3 - 2 * dt);
      p = on ? p : 1 - p;
      switch_until = now + 0.1;
   }
   switch_check_accent();
   k = (int)(p * SW_STEPS + 0.5);
   if (!sel && k != 0 && k != SW_STEPS)
   {
      switch_live(c, x, y, p, sel);   /* (a switch moved by something else, e.g. Reset: not worth a set of pictures) */
      return;
   }
   if (!sw_spr[sel ? 1 : 0][k].px)
      switch_sprite(sel ? 1 : 0, k);
   else if (sel && dt >= 1)
   {
      /* at rest on the chosen switch: make the knob's other positions, one a frame, so a slide never has to */
      int q;
      for (q = 0; q <= SW_STEPS; q++)
         if (!sw_spr[1][q].px)
         {
            switch_sprite(1, q);
            break;
         }
   }
   if (sw_spr[sel ? 1 : 0][k].px)
      sprite_blit(c, &sw_spr[sel ? 1 : 0][k], x - SW_PAD, y - SW_PAD);
   else
      switch_live(c, x, y, p, sel);
}

static void choice_text(int i, char *out, size_t sz)
{
   int v = st.val[i];
   const char *key = srows[i].key ? srows[i].key : "";
   if (!strcmp(key, "screen_off_seconds"))
   {
      if (!v)
         snprintf(out, sz, "Never");
      else if (v < 60)
         snprintf(out, sz, "%d seconds", v);
      else
         snprintf(out, sz, "%d minute%s", v / 60, v == 60 ? "" : "s");
   }
   else if (!strcmp(key, "auto_power_off_minutes"))
      snprintf(out, sz, v ? "%d minutes" : "Never", v);
   else if (!strcmp(key, "clock_24h"))
      snprintf(out, sz, "%s", v ? "24-hour" : "12-hour");
   else if (!strcmp(key, "usb_debugging") || !strcmp(key, "game_borders"))
      snprintf(out, sz, "%s", v ? "On" : "Off");
   else if (!strcmp(key, "click_volume"))
   {
      if (v)
         snprintf(out, sz, "%d%%", v);
      else
         snprintf(out, sz, "Off");
   }
   else if ((!strcmp(key, "startup_brightness") || !strcmp(key, "startup_volume")) && v < 0)
      snprintf(out, sz, "Last used");
   else
      snprintf(out, sz, "%d%%", v);
}

#define SET_ROW_H 38
#define SET_Y     58
#define SET_ROWS  ((480 - SET_Y - 48) / SET_ROW_H)

/* ---- About This Device */
static void du_text(const char *path, int used, char *out, size_t sz)
{
   struct statvfs v;
   if (statvfs(path, &v) != 0)
   {
      snprintf(out, sz, "?");
      return;
   }
   if (used)
      snprintf(out, sz, "%.0f MB used of %.0f MB", (double)(v.f_blocks - v.f_bfree) * v.f_frsize / 1e6,
            (double)v.f_blocks * v.f_frsize / 1e6);
   else
      snprintf(out, sz, "%.1f GB free of %.1f GB", (double)v.f_bavail * v.f_frsize / 1e9,
            (double)v.f_blocks * v.f_frsize / 1e9);
}

/* the card and its partitions' sizes, read when About opens */
static struct
{
   int measured;
   long long card, g_total, g_free, e_total, e_free;
} du;

static void about_measure(void)
{
   struct statvfs v;
   memset(&du, 0, sizeof(du));
   if (statvfs(SDCARD, &v) == 0)
   {
      du.g_total = (long long)v.f_blocks * v.f_frsize;
      du.g_free = (long long)v.f_bavail * v.f_frsize;
   }
   if (statvfs(BOARD_EMU_MOUNT, &v) == 0)
   {
      du.e_total = (long long)v.f_blocks * v.f_frsize;
      du.e_free = (long long)v.f_bavail * v.f_frsize;
   }
   {
      /* the whole card: /sys gives its size in 512-byte sectors */
      char buf[32];
      if (read_file(BOARD_CARD_SIZE, buf, sizeof(buf)) == 0)
         du.card = atoll(buf) * 512;
   }
   du.measured = 1;
}

static void size_text(long long b, char *out, size_t sz)
{
   if (b >= 1000000000LL)
      snprintf(out, sz, "%.1f GB", b / 1e9);
   else if (b >= 1000000)
      snprintf(out, sz, "%.0f MB", b / 1e6);
   else if (b > 0)
      snprintf(out, sz, "%.0f KB", b / 1e3 < 1 ? 1 : b / 1e3);
   else
      snprintf(out, sz, "0");
}

/* the card as one bar, split into its partitions and what fills them, with a legend
 * (which wraps onto a second line if it has to) */
static int draw_card_bar(canvas_t *c, int y, const char *const *labels, const long long *parts,
      const uint32_t *cols, int nparts, long long total, long long free_b)
{
   int x = 44, w = c->w - 88, bh = 16, i, px = x, lx = x;
   char head[160], fs[32], ts[32];

   size_text(free_b, fs, sizeof(fs));
   size_text(total, ts, sizeof(ts));
   gfx_draw_text(c, f_mid, "Memory card", x, y, C_TEXT, 0);
   snprintf(head, sizeof(head), "%s free of %s", fs, ts);
   gfx_draw_text(c, f_small, head, c->w - 44 - font_text_width(f_small, head), y + 3, C_DIM, 0);
   y += font_height(f_mid) + 6;

   gfx_fill_rect(c, x, y, w, bh, 0xff3a2a48);   /* the free space is the empty track */
   for (i = 0; i < nparts && total > 0; i++)
   {
      int pw = (int)((double)parts[i] * w / total + 0.5);
      if (parts[i] > 0 && pw < 2)
         pw = 2;   /* something small still shows */
      if (px + pw > x + w)
         pw = x + w - px;
      gfx_fill_rect(c, px, y, pw, bh, cols[i]);
      px += pw;
   }
   round_corners(c, x, y, w, bh, 6);
   y += bh + 10;

   for (i = 0; i <= nparts; i++)
   {
      char t[64], sz[32];
      int tw;
      if (i < nparts && !labels[i])
         continue;   /* a part with no legend of its own (the emulators' spare room) */
      size_text(i == nparts ? free_b : parts[i], sz, sizeof(sz));
      snprintf(t, sizeof(t), "%s %s", i == nparts ? "Free" : labels[i], sz);
      tw = 16 + font_text_width(f_small, t);
      if (lx + tw > x + w)
      {
         lx = x;
         y += font_height(f_small) + 6;
      }
      gfx_fill_rect(c, lx, y + font_height(f_small) / 2 - 5, 10, 10, i == nparts ? 0xff3a2a48 : cols[i]);
      gfx_draw_text(c, f_small, t, lx + 16, y, C_DIM, 0);
      lx += tw + 22;
   }
   return y + font_height(f_small) + 18;
}

static void draw_about(canvas_t *c)
{
   /* the card's partitions: system, emulators, games (what's used of it), then free */
   static const char *const labels_[] = { "System", "Emulators", "Games" };
   static const uint32_t cols[] = { 0xff8a7f99, 0xff5ad1c6, 0xffcd5cff };
   char vals[5][96];
   const char *labels[5] = { "Device", "Firmware", "Games", "Battery", "Memory" };
   int i, y;
   long pages = sysconf(_SC_PHYS_PAGES), psize = sysconf(_SC_PAGESIZE);

   if (!du.measured)
      about_measure();

   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   gfx_draw_text(c, f_big, "About This Device", 20, (50 - font_height(f_big)) / 2, C_TEXT, 600);
   draw_status(c, c->w - 16, 25, 0);

   /* storage first */
   gfx_draw_text(c, f_small, "STORAGE", 40, 60, C_ACCENT, 0);
   {
      long long card = du.card > 0 ? du.card : du.g_total + du.e_total;
      long long parts[3] = {
         card - du.g_total - du.e_total,   /* the system partitions (and the gaps between them) */
         du.e_total,
         du.g_total - du.g_free,
      };
      if (parts[0] < 0)
         parts[0] = 0;
      y = draw_card_bar(c, 86, labels_, parts, cols, 3, card, du.g_free);
   }

   snprintf(vals[0], sizeof(vals[0]), "G36 handheld (Rockchip RK3506B)");
   snprintf(vals[1], sizeof(vals[1]), "%s", G36_VERSION);
   snprintf(vals[2], sizeof(vals[2]), "%d on %d console%s", total_games(), nvisible, nvisible == 1 ? "" : "s");
   if (st_battery >= 0)
      snprintf(vals[3], sizeof(vals[3]), "%d%%%s", st_battery, st_power ? ", charging" : "");
   else
      snprintf(vals[3], sizeof(vals[3]), "?");
   {
      /* the chip, rounded up to a power of two, and what Linux has (the kernel and the
       * screen's reserve take the rest) */
      long usable = pages > 0 ? pages * (psize / 1024) / 1024 : 0, chip = 64;
      while (chip < usable)
         chip *= 2;
      snprintf(vals[4], sizeof(vals[4]), "%ld MB (%ld MB usable)", chip, usable);
   }
   for (i = 0; i < 5; i++)
   {
      int ry = y + i * 30;
      gfx_draw_text(c, f_small, labels[i], 44, ry, C_DIM, 300);
      gfx_draw_text(c, f_small, vals[i], c->w - 44 - font_text_width(f_small, vals[i]), ry, C_TEXT, 0);
   }
   draw_hints(c, "B  Back");
}


static void draw_settings(canvas_t *c, double now)
{
   int i, n = 0, vis[NSROWS], selpos = 0, y;
   char val[96];

   for (i = 0; i < NSROWS; i++)
      if (srow_shown(i))
      {
         if (i == st.sel)
            selpos = n;
         vis[n++] = i;
      }
   if (selpos < st.top + 1)
      st.top = selpos > 0 ? selpos - 1 : 0;   /* keep the section title above in view */
   if (selpos >= st.top + SET_ROWS)
      st.top = selpos - SET_ROWS + 1;
   {
      /* on the last thing that can be selected: show the end (the info rows) too */
      int k, last = 1;
      for (k = st.sel + 1; k < NSROWS; k++)
         if (srows[k].kind != S_HEAD && srows[k].kind != S_INFO && srow_shown(k))
            last = 0;
      if (last && n > SET_ROWS)
         st.top = n - SET_ROWS;
   }

   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   gfx_draw_text(c, f_big, "Settings", 20, (50 - font_height(f_big)) / 2, C_TEXT, 600);
   draw_status(c, c->w - 16, 25, 0);
   for (i = 0; i < SET_ROWS && st.top + i < n; i++)
   {
      int r = vis[st.top + i], sel = r == st.sel;
      const srow_t *row = &srows[r];
      y = SET_Y + i * SET_ROW_H;
      if (row->kind == S_HEAD)
      {
         gfx_draw_text(c, f_small, row->label, 40, y + SET_ROW_H - font_height(f_small) - 4, C_ACCENT, 0);
         continue;
      }
      if (sel)
      {
         gfx_fill_rect(c, 24, y + 2, c->w - 48, SET_ROW_H - 4, C_SEL);
         gfx_fill_rect(c, 24, y + 2, 4, SET_ROW_H - 4, C_ACCENT);
      }
      gfx_draw_text(c, f_mid, row->label, 44, y + (SET_ROW_H - font_height(f_mid)) / 2,
            row->kind == S_INFO ? C_DIM : sel ? C_TEXT : C_DIM, 400);
      val[0] = 0;
      if (row->kind == S_CHOICE && row->key && (!strcmp(row->key, "usb_debugging") || !strcmp(row->key, "game_borders") || !strcmp(row->key, "fps_show")))
         draw_switch(c, !strcmp(row->key, "usb_debugging") ? 3 : !strcmp(row->key, "fps_show") ? 8 : 4, c->w - 50, y + SET_ROW_H / 2, st.val[r], sel, now);
      else if (row->kind == S_CHOICE)
         choice_text(r, val, sizeof(val));

      if (val[0])
      {
         int vw = font_text_width(f_mid, val), vx = c->w - 50 - vw;
         uint32_t col = row->kind == S_INFO ? C_FAINT : sel ? C_TEXT : C_DIM;
         if (sel && row->kind == S_CHOICE)
         {
            gfx_draw_text(c, f_mid, "<", vx - 26, y + (SET_ROW_H - font_height(f_mid)) / 2, C_ACCENT, 0);
            gfx_draw_text(c, f_mid, ">", c->w - 42, y + (SET_ROW_H - font_height(f_mid)) / 2, C_ACCENT, 0);
         }
         gfx_draw_text(c, f_mid, val, vx, y + (SET_ROW_H - font_height(f_mid)) / 2, col, 0);
      }
   }
   draw_hints(c, srows[st.sel].kind == S_CHOICE && srows[st.sel].nvals == 2 ? "Dpad:UD  Move     A  Switch     B  Back"
         : srows[st.sel].kind == S_CHOICE ? "Dpad:UD  Move     Dpad:LR  Change     B  Back"
         : "Dpad:UD  Move     A  Select     B  Back");
   draw_toast(c, now);
   draw_volume(c, now);
}

/* ---- the date and time editor */
static void clock_edit_open(void)
{
   time_t t = time(NULL);
   localtime_r(&t, &st.tm);
   st.tm.tm_sec = 0;
   st.field = 3;   /* start on the hour: that's usually what needs setting */
   st.editing_clock = 1;
}

static void clock_edit_change(int dir)
{
   struct tm *m = &st.tm;
   static const int mdays[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
   switch (st.field)
   {
      case 0: m->tm_year = 124 + (m->tm_year - 124 + dir + 76) % 76; break;   /* 2024 .. 2099 */
      case 1: m->tm_mon = (m->tm_mon + dir + 12) % 12; break;
      case 2: m->tm_mday = (m->tm_mday - 1 + dir + mdays[m->tm_mon]) % mdays[m->tm_mon] + 1; break;
      case 3: m->tm_hour = (m->tm_hour + dir + 24) % 24; break;
      case 4: m->tm_min = (m->tm_min + dir + 60) % 60; break;
   }
   if (m->tm_mday > mdays[m->tm_mon])
      m->tm_mday = mdays[m->tm_mon];
}

static void clock_edit_save(double now)
{
   struct tm m = st.tm;
   struct timespec ts;
   FILE *f;
   m.tm_isdst = 0;
   ts.tv_sec = mktime(&m);   /* the handheld runs on UTC, so this is the time as entered */
   ts.tv_nsec = 0;
   if (clock_settime(CLOCK_REALTIME, &ts) != 0)
   {
      toast("Couldn't set the clock", now);
      return;
   }
   /* save it now (g36d also saves every 10 minutes) and turn on the status bar clock */
   if ((f = fopen("/etc/g36-clock", "w")))
   {
      fprintf(f, "%ld\n", (long)ts.tv_sec);
      fclose(f);
   }
   touch(CLOCK_SET);
   sync();
   mlog("clock set to %04d-%02d-%02d %02d:%02d", m.tm_year + 1900, m.tm_mon + 1, m.tm_mday, m.tm_hour, m.tm_min);
   st.editing_clock = 0;
   toast("Clock set", now);
}

static void draw_clock_edit(canvas_t *c)
{
   static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
   char f[5][16];
   int w = 600, h = 230, x = c->w / 2 - w / 2, y = (c->h - h) / 2 - 10, i, fx[5], fwid[5], total = 0, gap = 18, cx;
   const struct tm *m = &st.tm;

   snprintf(f[0], sizeof(f[0]), "%d", m->tm_year + 1900);
   snprintf(f[1], sizeof(f[1]), "%s", months[m->tm_mon]);
   snprintf(f[2], sizeof(f[2]), "%d", m->tm_mday);
   if (clock_24h)
      snprintf(f[3], sizeof(f[3]), "%02d", m->tm_hour);
   else
      snprintf(f[3], sizeof(f[3]), "%d %s", m->tm_hour % 12 ? m->tm_hour % 12 : 12, m->tm_hour < 12 ? "AM" : "PM");
   snprintf(f[4], sizeof(f[4]), "%02d", m->tm_min);

   gfx_fill_rect(c, 0, 0, c->w, c->h, 0xb0000000);
   gfx_fill_rect(c, x, y, w, h, C_PANEL);
   gfx_fill_rect(c, x, y, w, 4, C_ACCENT);
   draw_centered(c, f_big, "Set date and time", c->w / 2, y + 22, C_TEXT);
   for (i = 0; i < 5; i++)
   {
      fwid[i] = font_text_width(f_big, f[i]) + 24;
      total += fwid[i] + (i ? gap : 0);
   }
   total += 30;   /* a wider gap between the date and the time */
   cx = c->w / 2 - total / 2;
   for (i = 0; i < 5; i++)
   {
      if (i == 3)
         cx += 30;
      fx[i] = cx;
      gfx_fill_rect(c, cx, y + 90, fwid[i], 56, i == st.field ? C_ACCENT : C_SEL);
      draw_centered(c, f_big, f[i], cx + fwid[i] / 2, y + 90 + (56 - font_height(f_big)) / 2, C_TEXT);
      cx += fwid[i] + gap;
   }
   (void)fx;
   draw_centered(c, f_small, "Dpad  Field     Up / Down  Change     A  Save     B  Cancel", c->w / 2, y + h - 40, C_DIM);
}

/* a rounded rectangle with a vertical gradient and see-through (alpha 0-255), drawn in bands like the opaque one */
static void fill_round_rect_vgrad_alpha(canvas_t *c, int x, int y, int w, int h, int r, uint32_t top_col, uint32_t bot_col, int alpha)
{
   int bands = h < 24 ? 4 : h / 4, b;
   canvas_t sub = *c;
   if (bands < 2)
      bands = 2;
   for (b = 0; b < bands; b++)
   {
      int y0 = y + h * b / bands, y1 = y + h * (b + 1) / bands, cy0 = y0 < 0 ? 0 : y0;
      if (y1 <= y0 || y0 >= c->h || y1 <= 0)
         continue;
      sub.px = c->px + (size_t)cy0 * c->stride;
      sub.h = y1 - cy0;
      fill_round_rect_alpha(&sub, x, y - cy0, w, h, r, mix_colour(top_col, bot_col, 256 * b / (bands - 1)), alpha);
   }
}

/* the card behind a prompt: a soft shadow, a see-through body that goes from a lighter top to a deeper bottom
 * (the dimmed screen shows faintly through), a bright edge, and the accent rim along the top */
static void draw_bubble_live(canvas_t *c, int x, int y, int w, int h)
{
   uint32_t top = mix_colour(C_PANEL, C_ACCENT, 52), bot = mix_colour(C_PANEL, 0xff000000, 60);
   canvas_t sub = *c;
   fill_round_rect_alpha(c, x - 10, y + 6, w + 20, h + 10, 26, 0xff000000, 40);
   fill_round_rect_alpha(c, x - 5, y + 8, w + 10, h + 2, 21, 0xff000000, 70);
   fill_round_rect_alpha(c, x - 1, y - 1, w + 2, h + 2, 17, 0xffffffff, 38);          /* the bright edge */
   fill_round_rect_vgrad_alpha(c, x, y, w, h, 16, top, bot, 185);
   fill_round_rect_alpha(c, x + 12, y + 5, w - 24, 1, 0, 0xffffffff, 28);                /* a light line under the rim */
   sub.h = y + 4 < c->h ? y + 4 : c->h;                                                  /* the rim: the top four rows */
   fill_round_rect(&sub, x, y, w, h, 16, C_ACCENT);
}

/* ...but drawn live that cost 77 ms a frame, so it is drawn once into a picture (with see-through edges and body) and that
 * is put on the screen each frame; it is drawn again only when the size or the theme colours change */
static void bubble_sprite_fn(canvas_t *cv, const void *arg)
{
   const int *wh = arg;
   draw_bubble_live(cv, 14, 4, wh[0], wh[1]);
}

static void draw_bubble(canvas_t *c, int x, int y, int w, int h)
{
   static image_t pic;
   static int kw, kh;
   static uint32_t kpanel, kaccent;
   int wh[2] = { w, h };
   if (!pic.px || kw != w || kh != h || kpanel != C_PANEL || kaccent != C_ACCENT)
   {
      sprite_from_draw(&pic, w + 28, h + 28, bubble_sprite_fn, wh);
      kw = w;
      kh = h;
      kpanel = C_PANEL;
      kaccent = C_ACCENT;
   }
   if (!pic.px)
   {
      draw_bubble_live(c, x, y, w, h);
      return;
   }
   {
      /* Nothing under a prompt moves, so the blend of the card over the screen is done once and kept: each frame samples
       * about every 37th pixel under the card, and if they are all as they were, the kept result is copied back. */
      static uint32_t *kept, *seen;
      static size_t kept_n;
      static int kx, ky, kcw, kch, kvalid;
      static uint32_t kpic_panel, kpic_accent;
      int rx = x - 14, ry = y - 4, rw = pic.w, rh = pic.h, same = 0, r;
      size_t n = (size_t)rw * rh, nseen = (n + 36) / 37, i;
      if (rx < 0 || ry < 0 || rx + rw > c->w || ry + rh > c->h)
      {
         sprite_blit(c, &pic, rx, ry);
         kvalid = 0;
         return;
      }
      if (kept_n != n)
      {
         free(kept);
         free(seen);
         kept = malloc(n * 4);
         seen = malloc(nseen * 4);
         kept_n = n;
         kvalid = 0;
      }
      if (!kept || !seen)
      {
         kept_n = 0;
         sprite_blit(c, &pic, rx, ry);
         return;
      }
      if (kvalid && kx == rx && ky == ry && kcw == rw && kch == rh && kpic_panel == C_PANEL && kpic_accent == C_ACCENT)
      {
         same = 1;
         for (i = 0; i < nseen && same; i++)
         {
            size_t at = i * 37;
            if (c->px[(size_t)(ry + at / rw) * c->stride + rx + at % rw] != seen[i])
               same = 0;
         }
      }
      if (same)
      {
         for (r = 0; r < rh; r++)
            memcpy(c->px + (size_t)(ry + r) * c->stride + rx, kept + (size_t)r * rw, (size_t)rw * 4);
         return;
      }
      for (i = 0; i < nseen; i++)
      {
         size_t at = i * 37;
         seen[i] = c->px[(size_t)(ry + at / rw) * c->stride + rx + at % rw];
      }
      sprite_blit(c, &pic, rx, ry);
      for (r = 0; r < rh; r++)
         memcpy(kept + (size_t)r * rw, c->px + (size_t)(ry + r) * c->stride + rx, (size_t)rw * 4);
      kx = rx;
      ky = ry;
      kcw = rw;
      kch = rh;
      kpic_panel = C_PANEL;
      kpic_accent = C_ACCENT;
      kvalid = 1;
   }
}

/* a button on a prompt: the chosen one in the accent colour, the others in the selection colour, both a touch
 * lighter at the top */
static void draw_prompt_button(canvas_t *c, int x, int y, int w, int h, int chosen)
{
   uint32_t base = chosen ? C_ACCENT : C_SEL;
   fill_round_rect_alpha(c, x, y + 3, w, h, 14, 0xff000000, 60);
   fill_round_rect_vgrad(c, x, y, w, h, 14, mix_colour(base, 0xffffffff, chosen ? 45 : 22), mix_colour(base, 0xff000000, chosen ? 25 : 15));
}

/* A prompt is drawn in two parts: the still part (the dim, the card, the words), and the part that moves (the buttons, the
 * switch). 0 draws both; 1 only the still part; 2 only the moving part, over a kept copy of the still part (see the main loop). */
static int prompt_phase;

/* a centred box with a title, up to two lines of notes and up to three buttons */
/* a prompt with up to four lines of notes (note3 and note4 may be NULL) */
static void draw_prompt4(canvas_t *c, const char *title, const char *note1, const char *note2, const char *note3,
      const char *note4, const char *const *opts, int nopts, int choice)
{
   const char *const notes_txt[4] = { note1, note2, note3, note4 };
   int notes = (note1 != NULL) + (note2 != NULL) + (note3 != NULL) + (note4 != NULL);
   int w = nopts > 3 ? 660 : notes || nopts > 2 ? 560 : 400, h = 170 + notes * 28 + (notes ? 12 : 0);
   int x = c->w / 2 - w / 2, y = (c->h - h) / 2 - 10, by = y + h - 70;
   int bw = nopts > 3 ? 140 : 160, gap = nopts > 3 ? 16 : 20, bx, i;
   /* a button grows when its words need more than the usual room (20 px each side), as far as the box allows */
   for (i = 0; i < nopts; i++)
   {
      int need = font_text_width(f_mid, opts[i]) + 40, most = (w - 40 - (nopts - 1) * gap) / nopts;
      if (need > bw)
         bw = need < most ? need : most;
   }
   bx = c->w / 2 - (nopts * bw + (nopts - 1) * gap) / 2;
   if (prompt_phase != 2)
   {
      gfx_fill_rect(c, 0, 0, c->w, c->h, 0xb0000000);
      draw_bubble(c, x, y, w, h);
      {
         int tw = font_text_width(f_big, title);
         if (tw > w - 40)
            tw = w - 40;
         gfx_draw_text(c, f_big, title, c->w / 2 - tw / 2, y + 30, C_TEXT, w - 40);
      }
      for (i = 0; i < 4; i++)
         if (notes_txt[i])
         {
            int tw = font_text_width(f_small, notes_txt[i]);
            if (tw > w - 40)
               tw = w - 40;
            gfx_draw_text(c, f_small, notes_txt[i], c->w / 2 - tw / 2, y + 86 + i * 28, C_DIM, w - 40);
         }
   }
   if (prompt_phase != 1)
   {
      for (i = 0; i < nopts; i++)
      {
         int cx = bx + i * (bw + gap);
         draw_prompt_button(c, cx, by, bw, 44, choice == i);
         draw_centered(c, f_mid, opts[i], cx + bw / 2, by + 8, C_TEXT);
      }
   }
}

/* a prompt with a switch ("Don't show again", as in the settings) above one button; choice 0 is the switch, 1 the button */
static void draw_prompt_toggle(canvas_t *c, const char *title, const char *note1, const char *note2, const char *note3,
      const char *note4, const char *toggle_text, const char *button, int choice, double now)
{
   const char *const notes_txt[4] = { note1, note2, note3, note4 };
   int notes = (note1 != NULL) + (note2 != NULL) + (note3 != NULL) + (note4 != NULL), i;
   int w = 560, y, x = c->w / 2 - w / 2, h, ty, cy, by, bw = 200, rw;
   int tw = font_text_width(f_small, toggle_text), row = SW_W + 14 + tw;
   ty = 86 + notes * 28 + (notes ? 12 : 0);   /* from the top of the bubble: where the switch row starts */
   cy = ty + 22;
   by = cy + 38;
   h = by + 44 + 26;
   y = (c->h - h) / 2 - 10;
   if (prompt_phase != 2)
   {
      gfx_fill_rect(c, 0, 0, c->w, c->h, 0xb0000000);
      draw_bubble(c, x, y, w, h);
      {
         int ttw = font_text_width(f_big, title);
         if (ttw > w - 40)
            ttw = w - 40;
         gfx_draw_text(c, f_big, title, c->w / 2 - ttw / 2, y + 30, C_TEXT, w - 40);
      }
      for (i = 0; i < 4; i++)
         if (notes_txt[i])
         {
            int nw = font_text_width(f_small, notes_txt[i]);
            if (nw > w - 40)
               nw = w - 40;
            gfx_draw_text(c, f_small, notes_txt[i], c->w / 2 - nw / 2, y + 86 + i * 28, C_DIM, w - 40);
         }
   }
   if (prompt_phase == 1)
      return;
   rw = row + 40;
   if (choice == 0)
      fill_round_rect(c, c->w / 2 - rw / 2, y + cy - 20, rw, 40, 14, C_SEL);
   draw_switch(c, 5, c->w / 2 - row / 2 + SW_W, y + cy, prompt_toggle, choice == 0, now);
   gfx_draw_text(c, f_small, toggle_text, c->w / 2 - row / 2 + SW_W + 14, y + cy - font_height(f_small) / 2,
         choice == 0 ? C_TEXT : C_DIM, 0);
   draw_prompt_button(c, c->w / 2 - bw / 2, y + by, bw, 44, choice == 1);
   draw_centered(c, f_mid, button, c->w / 2, y + by + 8, C_TEXT);
}

static void draw_prompt(canvas_t *c, const char *title, const char *note1, const char *note2,
      const char *const *opts, int nopts, int choice)
{
   draw_prompt4(c, title, note1, note2, NULL, NULL, opts, nopts, choice);
}

/* Nothing under a prompt moves, so its still part (the screen below, the dim, the card, the words) is drawn once and kept;
 * after that a frame is that copy plus the buttons and the switch. It is drawn again about once a second (the clock, the
 * battery), and whenever the prompt changes. */
static uint32_t *frozen_px;
static int frozen_ok, frozen_w, frozen_h;
static uint32_t frozen_key;
static double frozen_t;

static uint32_t frozen_hash(uint32_t h, const char *str)
{
   for (; *str; str++)
      h = (h ^ (uint8_t)*str) * 16777619u;
   return h;
}

static int frozen_usable(const canvas_t *c, uint32_t key, double now)
{
   return frozen_ok && frozen_key == key && frozen_w == c->w && frozen_h == c->h && now - frozen_t < 1.0;
}

static void frozen_restore(canvas_t *c)
{
   int y;
   for (y = 0; y < c->h; y++)
      memcpy(c->px + (size_t)y * c->stride, frozen_px + (size_t)y * c->w, (size_t)c->w * 4);
}

static void frozen_save(const canvas_t *c, uint32_t key, double now)
{
   int y;
   if (!frozen_px || frozen_w != c->w || frozen_h != c->h)
   {
      free(frozen_px);
      frozen_px = malloc((size_t)c->w * c->h * 4);
      frozen_w = c->w;
      frozen_h = c->h;
   }
   if (!frozen_px)
   {
      frozen_ok = 0;
      return;
   }
   for (y = 0; y < c->h; y++)
      memcpy(frozen_px + (size_t)y * c->w, c->px + (size_t)y * c->stride, (size_t)c->w * 4);
   frozen_key = key;
   frozen_t = now;
   frozen_ok = 1;
}

/* A screen that is the same as the one before: the last frame is kept, and each new one is compared with it (and the kept
 * copy brought up to date). A still screen is then shown once, not 10 times a second, and drawn less often (see the main loop). */
static uint32_t *last_px;
static int last_w, last_h;

static int frame_changed(const canvas_t *c)
{
   int y, changed = 0;
   if (!last_px || last_w != c->w || last_h != c->h)
   {
      free(last_px);
      last_px = malloc((size_t)c->w * c->h * 4);
      last_w = c->w;
      last_h = c->h;
      changed = 1;
      if (!last_px)
         return 1;
   }
   for (y = 0; y < c->h; y++)
   {
      const uint32_t *row = c->px + (size_t)y * c->stride;
      uint32_t *kept = last_px + (size_t)y * c->w;
      if (changed || memcmp(kept, row, (size_t)c->w * 4))
      {
         memcpy(kept, row, (size_t)c->w * 4);
         changed = 1;
      }
   }
   return changed;
}

/* what is on the display is not the kept frame any more (video, the screen off): the next frame is always shown */
static void frame_forget(void)
{
   free(last_px);
   last_px = NULL;
}

#define PROMPT_POWER   1
#define PROMPT_USB     2
#define PROMPT_BATTERY 3
#define PROMPT_CORE    4      /* emulator picker: which emulator */
#define PROMPT_CORE_HOW 5     /* ...and for what: once, this game, the console */
#define PROMPT_ERROR   6      /* a game that didn't start */
#define PROMPT_BIOS    7      /* a warning before launching (missing BIOS, zip too big): play anyway? */
#define PROMPT_RESET   8      /* the Video Player: forget where every video was stopped? */
#define PROMPT_THEME_RESET 9  /* the theme editor: back to the default colours? */
#define PROMPT_PIC_DELETE 10  /* the gallery: delete this picture? */
#define PROMPT_SET_DEFAULTS 11 /* Settings: put every setting back to its default? */
#define PROMPT_SLEEP 12       /* sleep, with the note about what it does */
#define PROMPT_LAUNCH_TIP 13  /* the first few launches: how to sleep and power off from inside a game */

/* Restart needs outside power: a restart resets the pin (gpio0-5, "system_power")
 * that keeps the handheld switched on, so on battery it would just power off. */
static const char *const power_opts[] = { "Power off", "Restart", "Sleep", "Cancel" };
static const char *const power_opts_battery[] = { "Power off", "Sleep", "Cancel" };
static const char *const sleep_opts[] = { "Sleep", "Cancel" };
static const char *const tip_opts[] = { "Play", "Hide tips" };
static int power_restart;   /* the open power prompt offers Restart */
static const char *const usb_opts[]   = { "Allow", "Not now" };
static const char *const ok_opts[]    = { "OK" };
static const char *const how_opts[]   = { "Play once", "This game", "This console" };
static const char *const bios_opts[]  = { "Play anyway", "Cancel" };
static const char *const psx_opts[]   = { "Don't show again", "Play" };   /* (the prompt opens on the second) */
static const char *const sleep_state_opts[] = { "Start normally", "Load state" };   /* (the prompt opens on the second) */
static const char *const reset_opts[] = { "Reset", "Cancel" };
static const char *const delete_opts[] = { "Delete", "Cancel" };
static const char *const restore_opts[] = { "Restore", "Cancel" };

static int prompt_opts(int prompt)
{
   return prompt == PROMPT_POWER ? (power_restart ? 4 : 3) : prompt == PROMPT_USB ? 2
        : prompt == PROMPT_CORE_HOW ? 3 : prompt == PROMPT_BIOS || prompt == PROMPT_RESET || prompt == PROMPT_THEME_RESET || prompt == PROMPT_PIC_DELETE || prompt == PROMPT_SET_DEFAULTS || prompt == PROMPT_SLEEP || prompt == PROMPT_LAUNCH_TIP ? 2 : 1;
}

/* Sleep: the screen goes off at once, then g36-sleep flushes the card, locks it read-only,
 * suspends, and unlocks it on waking. The Power button wakes it (and, with the screen off,
 * that same tap switches the screen back on). */
static void go_to_sleep(int *screen_on)
{
   save_state();
   mlog("sleep");
   *screen_on = 0;
   gfx_screen_power(0);
   if (fork() == 0)
   {
      setsid();
      execl("/usr/sbin/g36-sleep", "g36-sleep", (char *)NULL);
      _exit(1);
   }
}

/* Power taps right after waking. The wake-up press can arrive as a tap of its own and then again as the
 * finger's press, which switched the screen straight back off. The monotonic clock stops while the
 * handheld sleeps and the boot-time clock doesn't, so a jump in their difference is a wake-up; for
 * the next WAKE_GUARD_S seconds (the screen can take a moment to show) only the first Power tap counts. */
#define WAKE_GUARD_S 5.0

static double slept_s(void)
{
   struct timespec a, b;
   clock_gettime(CLOCK_MONOTONIC, &a);
   clock_gettime(CLOCK_BOOTTIME, &b);
   return (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
}

/* what the panel is doing, for the log: "dpms=On bl=50 blp=0 kernel=952.1" */
static const char *screen_dbg(void)
{
   static char out[96];
   char a[16] = "?", b[16] = "?", c[16] = "?", up[32] = "?";
   const char *files[] = { "/sys/class/drm/card0-DSI-1/dpms", "/sys/class/backlight/backlight/actual_brightness",
                           "/sys/class/backlight/backlight/bl_power", "/proc/uptime" };
   char *dst[] = { a, b, c, up };
   size_t sz[] = { sizeof(a), sizeof(b), sizeof(c), sizeof(up) };
   for (int i = 0; i < 4; i++)
   {
      FILE *f = fopen(files[i], "r");
      if (f)
      {
         if (fscanf(f, i == 3 ? "%31s" : "%15s", dst[i]) != 1)
            dst[i][0] = '?';
         fclose(f);
      }
      (void)sz;
   }
   snprintf(out, sizeof(out), "dpms=%s bl=%s blp=%s kernel=%s", a, b, c, up);
   return out;
}

static double wake_seen_at = -100, wake_slept;
static int wake_tap_used;

/* true while inside the guard window after a wake-up (call before handling a Power event) */
static int wake_guard(double now)
{
   double s = slept_s();
   if (s > wake_slept + 0.5)
   {
      wake_seen_at = now;
      wake_tap_used = 0;
      mlog("woke from sleep");
   }
   wake_slept = s;
   return now - wake_seen_at < WAKE_GUARD_S;
}

/* ------------------------------------------------------------- main loop */

static volatile int quit, want_shot;
static void on_signal(int s) { (void)s; quit = 1; }
static void on_usr1(int s) { (void)s; want_shot = 1; }
static void on_usr2(int s) { (void)s; timing = !timing; }

/* kill -USR1 <menu>: save the screen to /tmp/g36menu-shot.ppm (for checking layouts over ADB) */
static void save_shot(const canvas_t *c)
{
   FILE *f = fopen("/tmp/g36menu-shot.ppm", "wb");
   int x, y;
   if (!f)
      return;
   fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
   for (y = 0; y < c->h; y++)
      for (x = 0; x < c->w; x++)
      {
         uint32_t p = c->px[y * c->stride + x];
         unsigned char rgb[3] = { p >> 16 & 0xff, p >> 8 & 0xff, p & 0xff };
         fwrite(rgb, 1, 3, f);
      }
   fclose(f);
}

/* ------------------------------------------------------------- search screen */
/* an on-screen keyboard: QWERTY letters on the left, a numpad on the right. Each row is
 * 13 cells; a key a few cells wide repeats in each (Show results, Space along the
 * bottom, the 0). */
#define KB_SPACE 1
#define KB_DEL   2
#define KB_GO    3
#define KB_SYM   4        /* switches between the letters and the symbols */
#define KB_COLS  13
#define KB_PAD   10       /* the numpad starts at this column */
static const char kb_rows[4][KB_COLS + 1] = {
   "QWERTYUIOP789", "ASDFGHJKL\002456", "ZXCVBNM\003\003\003123", "\001\001\001\001\001\001\001\001\004\004000" };
static const char kb_sym_rows[4][KB_COLS + 1] = {
   "!?.,'\":;()789", "-_=+*/&@#\002456", "$%<>[]~\003\003\003123", "\001\001\001\001\001\001\001\001\004\004000" };
static int kb_sym;        /* the symbols page is showing */
#define SUGGEST_MAX 4
static struct
{
   int r, c;              /* the key the cursor is on; r = -1: the suggestion bar, -2: the results above */
   int p;                 /* the result it's on, when r = -2 */
   int s;                 /* the suggestion it's on, in the bar */
   list_t res;            /* what matches what's typed */
   char res_q[sizeof(search_q)];
   int fresh;
   char sugg[SUGGEST_MAX][40];   /* words that finish (or follow) what's typed */
   int next[SUGGEST_MAX];        /* 1: it follows the typed word, rather than finishing it */
   int nsugg;
} sk;

/* Autocomplete. Two kinds of word, from the names of the games that match what's
 * finished so far: words that finish the one being typed ("MAR" -> MARIO), and words
 * that come next once a word is whole ("MARIO" -> KART, BROS). Finishing words come
 * first; within each kind, the most common. Upper case, like the keyboard. */
static struct { char w[40]; int n, next; } cand[512];
static int ncand;

/* add the words of matching names that start with part (next: any word, for after
 * the typed one) */
static void suggest_gather(const char *done, const char *part, int next)
{
   static const char *const filler = " the of and in to on at an for vs with from ";
   char name[600];
   size_t pl = strlen(part);
   int i, j, k;
   for (i = 0; i < nvisible; i++)
   {
      system_t *sy = &systems[visible[i]];
      for (j = 0; j < sy->ngames; j++)
      {
         const char *w;
         if (done[0] && !search_match(done, sy->games[j].label))
            continue;
         search_norm(sy->games[j].label, name, sizeof(name));
         for (w = name; *w; )
         {
            size_t wl = strcspn(w, " "), dg = 0;
            char word[40], probe[44];
            int skip = wl < 2 || wl >= sizeof(word) || strncmp(w, part, pl) || wl <= pl;
            while (dg < wl && isdigit((unsigned char)w[dg]))
               dg++;
            if (dg == wl)
               skip = 1;   /* nor only digits ("001") */
            if (!skip)
            {
               memcpy(word, w, wl);
               word[wl] = 0;
               /* small joining words aren't worth a slot as a next word */
               snprintf(probe, sizeof(probe), " %s ", word);
               if (!pl && strstr(filler, probe))
                  skip = 1;
               /* nor a word that's already typed */
               if (!skip && done[0])
               {
                  char pad[sizeof(search_q) * 2 + 2];
                  snprintf(pad, sizeof(pad), " %s ", done);
                  skip = strstr(pad, probe) != NULL;
               }
            }
            for (k = 0; !skip && k < ncand; k++)
               if (!strcmp(cand[k].w, word))
               {
                  if (cand[k].next == next)
                     cand[k].n++;
                  skip = 1;
               }
            if (!skip && ncand < (int)(sizeof(cand) / sizeof(cand[0])))
            {
               snprintf(cand[ncand].w, sizeof(cand[0].w), "%s", word);
               cand[ncand].n = 1;
               cand[ncand++].next = next;
            }
            w += wl;
            while (*w == ' ')
               w++;
         }
      }
   }
}

static void search_suggest(void)
{
   char done[sizeof(search_q) * 2], part[48], tmp[sizeof(search_q)];
   const char *last = strrchr(search_q, ' ');
   int i, k;

   sk.nsugg = 0;
   ncand = 0;
   if (!search_q[0])
      return;
   /* the finished words, and the one being typed ("" after a space) */
   snprintf(tmp, sizeof(tmp), "%.*s", last ? (int)(last - search_q) : 0, search_q);
   search_norm(tmp, done, sizeof(done));
   search_norm(last ? last + 1 : search_q, part, sizeof(part));
   suggest_gather(done, part, !part[0]);
   if (part[0])
   {
      /* the typed word as it stands, then what follows it */
      char whole[sizeof(done) + sizeof(part) + 2];
      snprintf(whole, sizeof(whole), "%s%s%s", done, done[0] ? " " : "", part);
      suggest_gather(whole, "", 1);
   }
   for (k = 0; k < SUGGEST_MAX && k < ncand; k++)
   {
      int best = -1;
      for (i = 0; i < ncand; i++)
      {
         if (cand[i].n < 0)
            continue;
         if (best < 0 || cand[i].next < cand[best].next ||
               (cand[i].next == cand[best].next && (cand[i].n > cand[best].n ||
               (cand[i].n == cand[best].n && (strlen(cand[i].w) < strlen(cand[best].w) ||
               (strlen(cand[i].w) == strlen(cand[best].w) && strcmp(cand[i].w, cand[best].w) < 0))))))
            best = i;
      }
      if (best < 0)
         break;
      for (i = 0; cand[best].w[i]; i++)
         sk.sugg[sk.nsugg][i] = (char)toupper((unsigned char)cand[best].w[i]);
      sk.sugg[sk.nsugg][i] = 0;
      sk.next[sk.nsugg] = cand[best].next && part[0];
      sk.nsugg++;
      cand[best].n = -1;
   }
}

/* a suggestion: in place of the word being typed, or after it; then a space */
static void search_accept(int i)
{
   char *last;
   if (i < 0 || i >= sk.nsugg)
      return;
   if (sk.next[i])
   {
      /* after the typed word */
      size_t n = strlen(search_q);
      if (n + 1 < sizeof(search_q) && n && search_q[n - 1] != ' ')
         strcat(search_q, " ");
   }
   else
   {
      last = strrchr(search_q, ' ');
      if (last)
         last[1] = 0;
      else
         search_q[0] = 0;
   }
   if (strlen(search_q) + strlen(sk.sugg[i]) + 2 < sizeof(search_q))
   {
      strcat(search_q, sk.sugg[i]);
      strcat(search_q, " ");
   }
}

/* what's in a keyboard cell: a letter or digit, KB_SPACE, KB_DEL or KB_GO */
static int kb_key(int r, int c)
{
   return (unsigned char)(kb_sym && r < 3 ? kb_sym_rows : kb_rows)[r][c];
}

/* a key's first cell and how many it spans (wide keys are runs of the same key; the
 * numpad's 0 is a run of '0', which no other key in its row repeats) */
static int kb_first(int r, int c)
{
   int k = kb_key(r, c);
   while (c > 0 && kb_key(r, c - 1) == k && (k < '0' || r == 3) && (c - 1 >= KB_PAD) == (c >= KB_PAD))
      c--;
   return c;
}

static int kb_span(int r, int c)
{
   int k = kb_key(r, c), f = kb_first(r, c), n = 1;
   while (f + n < KB_COLS && kb_key(r, f + n) == k && (k < '0' || r == 3) && (f + n >= KB_PAD) == (f >= KB_PAD))
      n++;
   return n;
}

static void hit_text(int i, char *name, size_t nsz, char *kind, size_t ksz);
static int hits_build(void);   /* everything that matches: cards, games, media (below) */
static int nhits_cached(void);
#define SEARCH_SHOWN 6
static int search_shown(void)
{
   int n = nhits_cached();
   return n < SEARCH_SHOWN ? n : SEARCH_SHOWN;
}

static void search_refresh(void)
{
   double t0;
   if (sk.fresh && !strcmp(sk.res_q, search_q))
      return;
   t0 = now_s();
   list_search(&sk.res);
   sk.fresh = 1;
   snprintf(sk.res_q, sizeof(sk.res_q), "%s", search_q);
   hits_build();
   search_suggest();
   snprintf(sk.res_q, sizeof(sk.res_q), "%s", search_q);
   sk.fresh = 1;
   if (sk.s >= sk.nsugg)
      sk.s = 0;
   if (sk.r == -1 && !sk.nsugg)
      sk.r = 0;   /* the bar emptied: back to the keys */
   if (sk.r == -2 && !search_shown())
      sk.r = 0;   /* nothing above any more */
   if (sk.r == -2 && sk.p >= search_shown())
      sk.p = search_shown() - 1;
   if (timing)   /* (kill -USR2: the same switch as the frame timing) */
      mlog("search \"%s\": %.0f ms, %d games, %d hits, %d suggestions", search_q, (now_s() - t0) * 1000, sk.res.n, nhits_cached(), sk.nsugg);
}

static int mfiles_done;

static void search_open(void)
{
   ui.view = 6;
   sk.fresh = 0;
   mfiles_done = 0;   /* the videos and songs are looked at again */
}

static void search_type(int key)
{
   size_t n = strlen(search_q);
   if (key == KB_DEL)
   {
      if (n)
         search_q[n - 1] = 0;
   }
   else if (n + 1 < sizeof(search_q) && !(key == KB_SPACE && (!n || search_q[n - 1] == ' ')))
   {
      search_q[n] = key == KB_SPACE ? ' ' : (char)key;
      search_q[n + 1] = 0;
   }
}

/* the matches, as an ordinary game list (B comes back here) */

static void search_results(double t)
{
   search_refresh();
   if (!search_q[0])
   {
      toast("Type part of a name first", t);
      return;
   }
   if (!hits_build())
   {
      toast("Nothing matches", t);
      return;
   }
   ui.view = 7;
}

static void search_move(int code)
{
   search_refresh();
   if (sk.r == -2)
   {
      /* the results above the keyboard: up and down through them, then back down */
      if (code == KEY_UP && sk.p > 0)
         sk.p--;
      else if (code == KEY_DOWN && sk.p < search_shown() - 1)
         sk.p++;
      else if (code == KEY_DOWN)
         sk.r = sk.nsugg ? -1 : 0;
      return;
   }
   if (sk.r == -1)
   {
      /* the suggestion bar */
      if (code == KEY_LEFT || code == KEY_RIGHT)
         sk.s = (sk.s + (code == KEY_LEFT ? sk.nsugg - 1 : 1)) % (sk.nsugg ? sk.nsugg : 1);
      else if (code == KEY_UP && search_shown())
      {
         sk.r = -2;
         sk.p = search_shown() - 1;
      }
      else
         sk.r = code == KEY_DOWN ? 0 : 3;
      return;
   }
   if (code == KEY_UP && sk.r == 0 && sk.nsugg)
   {
      sk.r = -1;
      return;
   }
   if (code == KEY_UP && sk.r == 0 && search_shown())
   {
      sk.r = -2;
      sk.p = search_shown() - 1;
      return;
   }
   if (code == KEY_UP || code == KEY_DOWN)
   {
      sk.r = (sk.r + (code == KEY_UP ? 3 : 1)) % 4;
      if (code == KEY_DOWN && sk.r == 0 && sk.nsugg)
         sk.r = -1;   /* down from the bottom row wraps to the bar */
      return;
   }
   /* sideways: to the next key, past every cell of a wide one */
   if (code == KEY_LEFT)
      sk.c = (kb_first(sk.r, sk.c) + KB_COLS - 1) % KB_COLS;
   else
      sk.c = (kb_first(sk.r, sk.c) + kb_span(sk.r, sk.c)) % KB_COLS;
}

static void search_key(int code, double t)
{
   if (code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT)
      search_move(code);
   else if (code == BTN_SOUTH && sk.r == -2)
      ;   /* opened in the main loop (it may start a game) */
   else if (code == BTN_SOUTH && sk.r == -1)
      search_accept(sk.s);
   else if (code == BTN_SOUTH && kb_key(sk.r, sk.c) == KB_GO)
      search_results(t);
   else if (code == BTN_SOUTH && kb_key(sk.r, sk.c) == KB_SYM)
      kb_sym ^= 1;
   else if (code == BTN_SOUTH)
      search_type(kb_key(sk.r, sk.c));
   else if (code == BTN_TR)
   {
      search_refresh();
      search_accept(0);   /* R: the first suggestion */
   }
   else if (code == BTN_WEST)
      search_type(KB_SPACE);
   else if (code == BTN_NORTH)
      search_type(KB_DEL);
   else if (code == BTN_EAST)
   {
      if (search_q[0])
         search_type(KB_DEL);
      else
         ui.view = 0;
   }
   else if (code == BTN_START)
      search_results(t);
}

static void draw_search(canvas_t *c, double now)
{
   int i, y, r, k, kw = 55, kh = 24, gap = 4, pad = 14, kx0, ky0 = 356;
   char line[96];

   search_refresh();
   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   gfx_draw_text(c, f_big, "Search", 20, (50 - font_height(f_big)) / 2, C_TEXT, 400);
   {
      int sw = draw_status(c, c->w - 16, 25, 0), x = c->w - 20 - sw - 14;
      if (search_q[0])
      {
         snprintf(line, sizeof(line), "%d found", nhits_cached());
         x -= font_text_width(f_small, line);
         gfx_draw_text(c, f_small, line, x, (50 - font_height(f_small)) / 2, C_DIM, 0);
         x -= 28;
      }
      x -= font_text_width(f_small, "R = Autocomplete");
      gfx_draw_text(c, f_small, "R = Autocomplete", x, (50 - font_height(f_small)) / 2, C_ACCENT, 0);
   }

   /* what's typed, in a box, with a blinking cursor */
   fill_round_rect(c, 23, 61, c->w - 46, 50, 12, C_ACCENT);
   fill_round_rect(c, 25, 63, c->w - 50, 46, 10, C_PANEL);
   y = 63 + (46 - font_height(f_big)) / 2;
   if (search_q[0])
   {
      int w = gfx_draw_text(c, f_big, search_q, 42, y, C_TEXT, c->w - 100);
      if (fmod(now, 1.0) < 0.6)
         gfx_fill_rect(c, 44 + w, y + 6, 3, font_height(f_big) - 12, C_ACCENT);
   }
   else
      gfx_draw_text(c, f_mid, "Type part of a game's name", 42, 63 + (46 - font_height(f_mid)) / 2, C_FAINT, 0);

   /* the first few matches */
   for (i = 0; i < SEARCH_SHOWN && i < nhits_cached(); i++)
   {
      char nm[300], kind[64];
      int sw, ry = 114 + i * 27, on = sk.r == -2 && sk.p == i;
      hit_text(i, nm, sizeof(nm), kind, sizeof(kind));
      if (on)
      {
         gfx_fill_rect(c, 25, ry, c->w - 50, 27, C_SEL);
         gfx_fill_rect(c, 25, ry, 4, 27, C_ACCENT);
      }
      sw = font_text_width(f_small, kind);
      gfx_draw_text(c, f_small, kind, c->w - 40 - sw, ry + (27 - font_height(f_small)) / 2, C_FAINT, 0);
      gfx_draw_text(c, f_mid, nm, 42, ry + (27 - font_height(f_mid)) / 2, on ? C_TEXT : C_DIM, c->w - 110 - sw);
   }
   if (search_q[0] && !nhits_cached())
      gfx_draw_text(c, f_mid, "Nothing matches", 42, 114 + (27 - font_height(f_mid)) / 2, C_FAINT, 0);
   if (nhits_cached())
   {
      /* Start opens the full list: a small drop-down arrow and the word */
      int ax = 44, ay = 287;
      for (i = 0; i < 6; i++)
         gfx_fill_rect(c, ax + i, ay + i, 12 - 2 * i, 1, C_ACCENT);
      gfx_draw_text(c, f_small, "Start", ax + 18, ay + 3 - font_height(f_small) / 2, C_ACCENT, 0);
   }

   /* the keyboard sits on a card of its own, like the menu's (suggestions and keys) */
   {
      int cw = KB_COLS * kw + (KB_COLS - 1) * gap + pad + 16, cx = (c->w - cw) / 2, cy = 348, ch = ky0 + 4 * (kh + gap) - gap + 8 - cy;
      canvas_t sub = { cw, ch, c->stride, c->px + (size_t)cy * c->stride + cx };
      fill_vgradient(&sub, 0, 0, cw, ch, TH(TH_CARD_TOP), TH(TH_CARD_BOT));
      round_corners(c, cx, cy, cw, ch, 18);
      /* and the suggestions' own card above it: a long pill */
      {
         canvas_t bar = { cw, 36, c->stride, c->px + (size_t)306 * c->stride + cx };
         fill_vgradient(&bar, 0, 0, cw, 36, TH(TH_CARD_TOP), TH(TH_BAR_BOX));
         round_corners(c, cx, 306, cw, 36, 18);
      }
   }

   /* the suggestion bar: words to finish with, the first one is R's */
   {
      int fw = KB_COLS * kw + (KB_COLS - 1) * gap + pad;
      int bx = (c->w - fw) / 2, by = 311, bw = (fw - 3 * gap) / SUGGEST_MAX;
      for (i = 0; i < SUGGEST_MAX; i++)
      {
         int x = bx + i * (bw + gap), sel = sk.r == -1 && sk.s == i;
         if (i >= sk.nsugg)
         {
            fill_round_rect(c, x, by, bw, 26, 13, TH(TH_BAR_BOX));
            continue;
         }
         fill_round_rect(c, x, by, bw, 26, 13, sel ? C_ACCENT : TH(TH_SUGGEST));
         gfx_draw_text(c, f_small, sk.sugg[i], x + bw / 2 - font_text_width(f_small, sk.sugg[i]) / 2 > x + 34
               ? x + bw / 2 - font_text_width(f_small, sk.sugg[i]) / 2 : x + 34,
               by + (26 - font_height(f_small)) / 2, sel ? 0xffffffff : C_TEXT, bw - 44);
         if (i == 0)
         {
            /* R takes this one: a small R badge at its left end */
            fill_round_rect(c, x + 4, by + 3, 20, 20, 10, sel ? 0xffffffff : C_ACCENT);
            draw_centered(c, f_title ? f_title : f_small, "R", x + 14, by + 13 - font_height(f_small) / 2,
                  sel ? C_ACCENT : 0xffffffff);
         }
      }
   }

   /* the keyboard: letters, then the numpad a little apart */
   kx0 = (c->w - (KB_COLS * kw + (KB_COLS - 1) * gap + pad)) / 2;
   for (r = 0; r < 4; r++)
      for (k = 0; k < KB_COLS; k++)
      {
         int key = kb_key(r, k), span, x, ky = ky0 + r * (kh + gap), w, sel;
         const char *label;
         char ch[2] = { (char)key, 0 };
         if (kb_first(r, k) != k)
            continue;   /* drawn with its first cell */
         span = kb_span(r, k);
         x = kx0 + k * (kw + gap) + (k >= KB_PAD ? pad : 0);
         w = span * kw + (span - 1) * gap;
         sel = r == sk.r && kb_first(sk.r, sk.c) == k;
         label = key == KB_SPACE ? "Space" : key == KB_DEL ? "Del" : key == KB_GO ? "Show results"
               : key == KB_SYM ? (kb_sym ? "ABC" : "Symbols") : ch;
         /* a mild outline, then the key */
         fill_round_rect(c, x - 1, ky - 1, w + 2, kh + 2, 8, sel ? 0xffe2a8ff : TH(TH_KEY_LINE));
         fill_round_rect(c, x, ky, w, kh, 7, sel ? C_ACCENT : TH(TH_KEY));
         draw_centered(c, f_small, label, x + w / 2, ky + (kh - font_height(f_small)) / 2, sel ? 0xffffffff : C_TEXT);
      }

   draw_toast(c, now);
   draw_volume(c, now);
}

/* ------------------------------------------------------------- media player */
/* videos/ and music/ on G36-GAMES: videos play full screen, music plays through its folder */
#define VIDEOS        SDCARD "/videos"
#define MUSIC         SDCARD "/music"
#define MEDIA_RESUME  SDCARD "/g36/watch-progress.txt"

enum { MK_DIR, MK_VIDEO, MK_MUSIC };

typedef struct
{
   char name[256];
   int kind;
} mentry_t;

static struct
{
   int mode;                    /* MK_VIDEO (the Video Player) or MK_MUSIC */
   char folder[1024];           /* inside videos/ or music/, "" = the top */
   char base[256];              /* the card it was opened from ("" = the loose files) */
   mentry_t *e;
   int n, cap, sel, top;
   char want[256];              /* select this one after the next reload */
   unsigned sig;                /* the folder's names, to notice new files */
   double check;
   char res_key[1100];          /* the folder whose watch progress is below, looked up once */
   char res_file[256];          /* the video you're on there, and where in it */
   double res_val;
} mb;

static struct
{
   int kind, index;
   char key[1300];              /* path inside videos/ */
   char name[256];
   double overlay_until, last_draw, stats_at;
   int got_frame, on_top_last, redraw, screen_was_on;
   int vx, vy, vw, vh;          /* where the picture goes */
   int sub_shown;               /* a subtitle is on screen (it's cleared when it goes) */
   image_t *cover;              /* album art, square, screen size */
   image_t *poster;             /* the video's picture, shown until its first frame */
} pl;

static int media_kind(const char *name)
{
   static const char *const video[] = { "mp4", "m4v", "mkv", "webm", "avi", "mov", "mpg", "mpeg",
      "ts", "m2ts", "flv", "3gp", "ogv", NULL };
   static const char *const music[] = { "mp3", "flac", "ogg", "oga", "opus", "m4a", "aac", "wav",
      "wma", NULL };
   const char *dot = strrchr(name, '.');
   int i;
   if (!dot)
      return -1;
   for (i = 0; video[i]; i++)
      if (!strcasecmp(dot + 1, video[i]))
         return MK_VIDEO;
   for (i = 0; music[i]; i++)
      if (!strcasecmp(dot + 1, music[i]))
         return MK_MUSIC;
   return -1;
}

/* folders first, then by name (the same order as the game lists) */
static int mentry_cmp(const void *x, const void *y)
{
   const mentry_t *a = x, *b = y;
   if ((a->kind == MK_DIR) != (b->kind == MK_DIR))
      return a->kind == MK_DIR ? -1 : 1;
   return natcmp(a->name, b->name);
}

static void media_dir(char *out, size_t sz)
{
   snprintf(out, sz, "%s%s%s", mb.mode == MK_VIDEO ? VIDEOS : MUSIC, mb.folder[0] ? "/" : "", mb.folder);
}

/* read the folder; returns a signature of what's in it */
static unsigned media_scan(int keep)
{
   char dir[1100];
   DIR *d;
   struct dirent *de;
   unsigned sig = 2166136261u;
   char cur[256] = "";

   if (keep && mb.n)
      snprintf(cur, sizeof(cur), "%s", mb.e[mb.sel].name);
   if (mb.want[0])
   {
      snprintf(cur, sizeof(cur), "%s", mb.want);
      mb.want[0] = 0;
   }
   mb.n = 0;
   media_dir(dir, sizeof(dir));
   if ((d = opendir(dir)))
   {
      while ((de = readdir(d)))
      {
         char p[1400];
         struct stat sb;
         int kind;
         const char *s;
         if (de->d_name[0] == '.')
            continue;
         snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
         if (stat(p, &sb) < 0)
            continue;
         kind = S_ISDIR(sb.st_mode) ? MK_DIR : media_kind(de->d_name);
         if (kind != MK_DIR && kind != mb.mode)
            continue;   /* the other player's files (and anything else) */
         if (kind == MK_DIR && !mb.folder[0])
            continue;   /* the top's folders are the cards; only its loose files are listed */
         if (mb.n == mb.cap)
         {
            int nc = mb.cap ? mb.cap * 2 : 64;
            mentry_t *ne = realloc(mb.e, (size_t)nc * sizeof(*ne));
            if (!ne)
               break;
            mb.e = ne;
            mb.cap = nc;
         }
         snprintf(mb.e[mb.n].name, sizeof(mb.e[0].name), "%s", de->d_name);
         mb.e[mb.n].kind = kind;
         mb.n++;
         for (s = de->d_name; *s; s++)
            sig = (sig ^ (unsigned char)*s) * 16777619u;
         sig = (sig ^ (unsigned)kind) * 16777619u;
      }
      closedir(d);
   }
   qsort(mb.e, mb.n, sizeof(*mb.e), mentry_cmp);
   if (cur[0])
   {
      int i;
      for (i = 0; i < mb.n; i++)
         if (!strcmp(mb.e[i].name, cur))
            mb.sel = i;
   }
   if (mb.sel >= mb.n)
      mb.sel = mb.n ? mb.n - 1 : 0;
   mb.res_key[0] = 0;
   return sig + (unsigned)mb.n;
}

/* playable files in a folder and the folders inside it (seasons, albums) */
static int media_count(const char *dir, int mode, int depth)
{
   DIR *d = opendir(dir);
   struct dirent *de;
   int n = 0;
   if (!d)
      return 0;
   while ((de = readdir(d)))
   {
      char p[1400];
      struct stat sb;
      if (de->d_name[0] == '.')
         continue;
      snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
      if (stat(p, &sb) < 0)
         continue;
      if (S_ISDIR(sb.st_mode))
      {
         if (depth < 4)
            n += media_count(p, mode, depth + 1);
      }
      else if (media_kind(de->d_name) == mode)
         n++;
   }
   closedir(d);
   return n;
}

static int mf_cmp(const void *a, const void *b)
{
   return natcmp(a, b);
}

/* what the watch progress file holds for a folder that has been watched to the end (in place of a video's name) */
#define FIN_MARK "(finished)"

/* How many videos come before a place in a folder, counted in the order the lists show them (the folders first,
 * each one whole, then the files). tf is the folder the place is in (relative to videos/, "" or "." = the top)
 * and tfile the video there; FIN_MARK counts the whole of tf as well. Returns 1 when the place was found; *n is
 * what was counted. flat = don't look inside folders (the card of loose files at the top). */
static int media_walk(const char *root, const char *rel, const char *tf, const char *tfile, int *n, int flat, int depth)
{
   char dir[1400];
   DIR *d;
   struct dirent *de;
   char (*dirs)[256], (*files)[256];
   int nd = 0, nf = 0, i, found = 0, here = !strcmp(rel, !strcmp(tf, ".") ? "" : tf);

   snprintf(dir, sizeof(dir), "%s%s%s", root, rel[0] ? "/" : "", rel);
   if (!(d = opendir(dir)))
      return 0;
   dirs = malloc(256 * sizeof(*dirs));
   files = malloc(256 * sizeof(*files));
   if (!dirs || !files)
   {
      free(dirs);
      free(files);
      closedir(d);
      return 0;
   }
   while ((de = readdir(d)))
   {
      char p[1500];
      struct stat sb;
      if (de->d_name[0] == '.')
         continue;
      snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
      if (stat(p, &sb) < 0)
         continue;
      if (S_ISDIR(sb.st_mode))
      {
         if (nd < 256)
            snprintf(dirs[nd++], 256, "%s", de->d_name);
      }
      else if (media_kind(de->d_name) == MK_VIDEO && nf < 256)
         snprintf(files[nf++], 256, "%s", de->d_name);
   }
   closedir(d);
   qsort(dirs, nd, sizeof(*dirs), mf_cmp);
   qsort(files, nf, sizeof(*files), mf_cmp);
   for (i = 0; i < nd && !found && !flat && depth < 4; i++)
   {
      char sub[1100];
      snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", dirs[i]);
      found = media_walk(root, sub, tf, tfile, n, 0, depth + 1);
   }
   for (i = 0; i < nf && !found; i++)
   {
      if (here && !strcmp(files[i], tfile))
         found = 1;
      else
         (*n)++;
   }
   if (!found && here && !strcmp(tfile, FIN_MARK))
      found = 1;
   free(dirs);
   free(files);
   return found;
}

/* the cards: the folders at the top of videos/ or music/ */
static void media_folders(void)
{
   const char *root = mb.mode == MK_VIDEO ? VIDEOS : MUSIC;
   DIR *d = opendir(root);
   struct dirent *de;
   int i;
   mf.video = mb.mode == MK_VIDEO;
   mf.n = mf.loose = 0;
   if (!d)
      return;
   while ((de = readdir(d)))
   {
      char p[1400];
      struct stat sb;
      if (de->d_name[0] == '.')
         continue;
      snprintf(p, sizeof(p), "%s/%s", root, de->d_name);
      if (stat(p, &sb) < 0)
         continue;
      if (S_ISDIR(sb.st_mode) && mf.n < MAX_SYSTEMS - 1)
         snprintf(mf.name[mf.n++], sizeof(mf.name[0]), "%s", de->d_name);
      else if (!S_ISDIR(sb.st_mode) && media_kind(de->d_name) == mb.mode)
         mf.loose++;
   }
   closedir(d);
   qsort(mf.name, mf.n, sizeof(mf.name[0]), mf_cmp);
   for (i = 0; i < mf.n; i++)
   {
      char p[1400];
      snprintf(p, sizeof(p), "%s/%s", root, mf.name[i]);
      mf.count[i] = media_count(p, mb.mode, 1);
   }
}

static unsigned media_folders_sig(void)
{
   unsigned h = 2166136261u;
   int i;
   const char *c;
   for (i = 0; i < mf.n; i++)
   {
      for (c = mf.name[i]; *c; c++)
         h = (h ^ (unsigned char)*c) * 16777619u;
      h = (h ^ (unsigned)mf.count[i]) * 16777619u;
   }
   return (h ^ (unsigned)mf.loose) * 16777619u + (unsigned)mf.n;
}

/* the Video Player or Music Player card: its folders, as a carousel */
static void vt_prune(void);
static void media_open(int mode, double t)
{
   mb.mode = mode;
   if (mode == MK_VIDEO)
      vt_prune();
   mkdir(mode == MK_VIDEO ? VIDEOS : MUSIC, 0755);
   media_folders();
   mf.sig = media_folders_sig();
   mf.check = t;
   row_set(ROW_MEDIA, 0);
   ui.anim_[ROW_MEDIA] = ui.pos_[ROW_MEDIA];
   ui.view = 5;
}

static void media_enter(double t);
static void theme_open(void);
static void gal_open(void);

/* a card: its folder's list */
static void media_open_card(int i, double t)
{
   if (i < mf.n)
      snprintf(mb.base, sizeof(mb.base), "%s", mf.name[i]);
   else
      mb.base[0] = 0;
   snprintf(mb.folder, sizeof(mb.folder), "%s", mb.base);
   media_enter(t);
   ui.view = 3;
}

/* ---- watch progress, one line per folder: "<folder inside videos/> TAB <video> TAB
 * <seconds>" in g36/watch-progress.txt ("." is the top of videos/). A folder plays
 * through, so only the video you're on in each folder is kept, and where in it. */
static void progress_folder(char *out, size_t sz)
{
   snprintf(out, sz, "%s", mb.folder[0] ? mb.folder : ".");
}

/* one line: its folder, video and seconds (an older "path = seconds" line too) */
static int progress_parse(char *line, char **folder, char **file, double *secs)
{
   char *t1 = strchr(line, '\t'), *t2, *eq;
   line[strcspn(line, "\n")] = 0;
   if (t1 && (t2 = strchr(t1 + 1, '\t')))
   {
      *t1 = *t2 = 0;
      *folder = line;
      *file = t1 + 1;
      *secs = atof(t2 + 1);
      return 1;
   }
   if ((eq = strstr(line, " = ")))
   {
      char *slash;
      *eq = 0;
      *secs = atof(eq + 3);
      if ((slash = strrchr(line, '/')))
      {
         *slash = 0;
         *folder = line;
         *file = slash + 1;
      }
      else
      {
         *folder = ".";
         *file = line;
      }
      return 1;
   }
   return 0;
}

static int progress_get(const char *folder, char *file, size_t fsz, double *secs)
{
   char line[1500], *fo, *fi;
   double v;
   int found = 0;
   FILE *f = fopen(MEDIA_RESUME, "r");
   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
      if (progress_parse(line, &fo, &fi, &v) && !strcmp(fo, folder))
      {
         snprintf(file, fsz, "%s", fi);
         *secs = v;
         found = 1;
      }
   fclose(f);
   return found;
}

/* the video you're on in a folder (FIN_MARK instead of a video: the folder has been watched to the end; NULL forgets it) */
static void progress_set(const char *folder, const char *file, double secs)
{
   char line[1500], copy[1500], *fo, *fi;
   double v;
   FILE *in = fopen(MEDIA_RESUME, "r"), *out = fopen(MEDIA_RESUME ".tmp", "w");
   if (!out)
   {
      if (in)
         fclose(in);
      return;
   }
   while (in && fgets(line, sizeof(line), in))
   {
      snprintf(copy, sizeof(copy), "%s", line);
      if (!progress_parse(copy, &fo, &fi, &v) || strcmp(fo, folder))
         fputs(line, out);
   }
   if (in)
      fclose(in);
   if (file)
      fprintf(out, "%s\t%s\t%d\n", folder, file, secs > 0 ? (int)secs : 0);
   fclose(out);
   rename(MEDIA_RESUME ".tmp", MEDIA_RESUME);
   mb.res_key[0] = 0;
}

static int resume_count(void)
{
   char line[1500], *fo, *fi;
   double v;
   int n = 0;
   FILE *f = fopen(MEDIA_RESUME, "r");
   if (!f)
      return 0;
   while (fgets(line, sizeof(line), f))
      n += progress_parse(line, &fo, &fi, &v);
   fclose(f);
   return n;
}

/* the list of a folder, starting on the video you're on in it */
static void media_enter(double t)
{
   char fk[1100], file[256];
   double secs;
   mb.want[0] = 0;
   progress_folder(fk, sizeof(fk));
   if (mb.mode == MK_VIDEO && progress_get(fk, file, sizeof(file), &secs))
      snprintf(mb.want, sizeof(mb.want), "%s", file);
   mb.sel = mb.top = 0;
   mb.sig = media_scan(0);
   mb.check = t;
}

static void resume_reset(double t)
{
   int n = resume_count();
   unlink(MEDIA_RESUME);
   mb.res_key[0] = 0;
   mlog("media: watch progress reset (%d videos)", n);
   toast(n == 1 ? "Watch progress reset: 1 video" : "Watch progress reset", t);
}

static void fmt_time(double s, char *out, size_t sz)
{
   int t = s > 0 ? (int)s : 0;
   if (t >= 3600)
      snprintf(out, sz, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
   else
      snprintf(out, sz, "%d:%02d", t / 60, t % 60);
}

/* the name without its extension */
static void media_title(const char *name, char *out, size_t sz)
{
   const char *dot = strrchr(name, '.');
   snprintf(out, sz, "%.*s", dot ? (int)(dot - name) : (int)strlen(name), name);
}

/* centre-cropped square, scaled to size */
static image_t *square_image(const image_t *src, int size)
{
   int side = src->w < src->h ? src->w : src->h, y;
   image_t tmp, *out;
   tmp.w = tmp.h = side;
   if (!(tmp.px = malloc((size_t)side * side * 4)))
      return NULL;
   for (y = 0; y < side; y++)
      memcpy(tmp.px + (size_t)y * side,
            src->px + (size_t)(y + (src->h - side) / 2) * src->w + (src->w - side) / 2, (size_t)side * 4);
   out = image_scaled(&tmp, size, size);
   free(tmp.px);
   return out;
}

#define COVER_PX 250

/* ---- the video player's quick menu (the Menu button), subtitles and the picture.
 * Subtitles, their size, the picture and playing on are kept in g36/player.cfg; the
 * speed and the sleep timer last until the menu restarts. */
#define PLAYER_FILE SDCARD "/g36/player.cfg"
static const double speeds[] = { 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2 };
#define NSPEEDS 7
#define SPEED_1 2
static const int sleeps[] = { 0, 15, 30, 45, 60, 90, 120 };   /* minutes */
#define NSLEEPS 7
static const char *const sub_sizes[] = { "Small", "Medium", "Large" };
static const int sub_px[] = { 20, 25, 31 };
static const char *const pictures[] = { "Fit", "Zoom to fill", "Stretch" };
enum { QM_SUBS, QM_SUBTRACK, QM_SUBSIZE, QM_AUDIO, QM_SPEED, QM_PICTURE, QM_AUTOPLAY, QM_SLEEP, QM_SOUND, QM_RESTART };
static struct
{
   int subs, sub_size, picture, autoplay;   /* kept */
   char sub_name[64];           /* the subtitle track last chosen, picked again next time */
   int speed, sleep;            /* until the menu restarts */
   double sleep_at;             /* when the sleep timer stops the video, 0 = off */
   int open, sel;               /* the quick menu */
   font_t *sf[3];
} vq = { 0, 1, 0, 1, "", SPEED_1, 0, 0, 0, 0, { NULL, NULL, NULL } };

static void vq_save(void)
{
   FILE *f = fopen(PLAYER_FILE ".tmp", "w");
   if (!f)
      return;
   fprintf(f, "# G36 video player settings (the Menu button while a video plays)\n"
         "subtitles = %s\nsubtitle_track = %s\nsubtitle_size = %s\npicture = %s\nplay_next = %s\n",
         vq.subs ? "on" : "off", vq.sub_name, sub_sizes[vq.sub_size], pictures[vq.picture],
         vq.autoplay ? "on" : "off");
   if (commit_close(f) == 0)
      rename(PLAYER_FILE ".tmp", PLAYER_FILE);
   else
      unlink(PLAYER_FILE ".tmp");
}

static void vq_load(void)
{
   char line[200];
   FILE *f = fopen(PLAYER_FILE, "r");
   int i;
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *eq = strchr(line, '='), *k = line, *v, *e;
      if (line[0] == '#' || !eq)
         continue;
      *eq = 0;
      v = eq + 1;
      while (*k == ' ')
         k++;
      for (e = eq - 1; e >= k && *e == ' '; e--)
         *e = 0;
      while (*v == ' ')
         v++;
      v[strcspn(v, "\r\n")] = 0;
      if (!strcmp(k, "subtitles"))
         vq.subs = !strcmp(v, "on");
      else if (!strcmp(k, "subtitle_track"))
         snprintf(vq.sub_name, sizeof(vq.sub_name), "%s", v);
      else if (!strcmp(k, "play_next"))
         vq.autoplay = strcmp(v, "off") != 0;
      else if (!strcmp(k, "subtitle_size"))
      {
         for (i = 0; i < 3; i++)
            if (!strcmp(v, sub_sizes[i]))
               vq.sub_size = i;
      }
      else if (!strcmp(k, "picture"))
      {
         for (i = 0; i < 3; i++)
            if (!strcmp(v, pictures[i]))
               vq.picture = i;
      }
   }
   fclose(f);
}

/* subtitles on with the track chosen before (by name), or the first; or off */
static void vq_apply_subs(void)
{
   int n = player_sub_count(), i, pick = 0;
   if (!vq.subs || !n)
   {
      player_set_sub(-1);
      return;
   }
   for (i = 0; i < n; i++)
      if (!strcmp(player_sub_name(i), vq.sub_name))
         pick = i;
   player_set_sub(pick);
}

/* where the picture goes: fitted, zoomed to fill (the edges cut off), or stretched */
static void video_layout(void)
{
   const player_info_t *in = player_info();
   canvas_t *c = gfx_canvas();
   gfx_video_t *v = player_video();
   double sx, sy, s;
   if (!in->has_video || in->video_w <= 0 || in->video_h <= 0)
      return;
   sx = (double)c->w / in->video_w;
   sy = (double)c->h / in->video_h;
   gfx_video_crop(v, 0, 0, 0, 0);
   if (vq.picture == 2)
   {
      pl.vw = c->w & ~1;
      pl.vh = c->h & ~1;
   }
   else if (vq.picture == 1)
   {
      /* the part of the frame that fills the screen, in the frame's own pixels */
      int cw, ch;
      s = sx > sy ? sx : sy;
      cw = (int)(c->w / s * in->src_w / in->video_w);
      ch = (int)(c->h / s * in->src_h / in->video_h);
      if (cw > in->src_w)
         cw = in->src_w;
      if (ch > in->src_h)
         ch = in->src_h;
      gfx_video_crop(v, (in->src_w - cw) / 2, (in->src_h - ch) / 2, cw, ch);
      pl.vw = c->w & ~1;
      pl.vh = c->h & ~1;
   }
   else
   {
      /* as big as fits, the right shape; even sizes for the scaler */
      s = sx < sy ? sx : sy;
      pl.vw = (int)(in->video_w * s) & ~1;
      pl.vh = (int)(in->video_h * s) & ~1;
   }
   pl.vx = (c->w - pl.vw) / 2;
   pl.vy = (c->h - pl.vh) / 2;
   pl.redraw = 1;
}

/* text with a dark outline, centred on cx */
static void outlined(canvas_t *c, font_t *f, const char *t, int cx, int y)
{
   static const signed char off[][2] = { { -2, 0 }, { 2, 0 }, { 0, -2 }, { 0, 2 },
      { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 }, { 2, 2 } };
   int x = cx - font_text_width(f, t) / 2;
   unsigned k;
   for (k = 0; k < sizeof(off) / sizeof(off[0]); k++)
      gfx_draw_text(c, f, t, x + off[k][0], y + off[k][1], 0xff000000, 0);
   gfx_draw_text(c, f, t, x, y, 0xffffffff, 0);
}

/* the subtitle showing now, at the bottom of the picture (above the controls) */
static int sub_render(canvas_t *screen, font_t *f, const char *text, int maxw, image_t **out);
static int draw_subtitles(canvas_t *c, int lift)
{
   player_sub_t sb;
   font_t *f;
   int lh, bottom;
   if (!player_subtitle(&sb))
      return 0;
   if (sb.img && sb.canvas_w > 0 && sb.canvas_h > 0)
   {
      /* a picture subtitle, placed as on the original frame */
      double kx = (double)pl.vw / sb.canvas_w, ky = (double)pl.vh / sb.canvas_h;
      int y = pl.vy + (int)(sb.y * ky), h = (int)(sb.h * ky);
      if (lift && y + h > c->h - lift)
         y = c->h - lift - h;
      gfx_draw_image_scaled(c, sb.img, pl.vx + (int)(sb.x * kx), y, (int)(sb.w * kx), h);
      return 1;
   }
   if (!vq.sf[vq.sub_size])
      vq.sf[vq.sub_size] = font_load_bold(FONT, sub_px[vq.sub_size]);
   if (!(f = vq.sf[vq.sub_size]))
      return 0;
   lh = font_height(f) + 2;
   bottom = pl.vy + pl.vh < c->h ? pl.vy + pl.vh : c->h;
   if (bottom > c->h - lift - 14)
      bottom = c->h - lift - 14;
   {
      /* The same subtitle stays up for seconds while the video redraws under it 24 times
       * a second: it's drawn once into a picture (see-through around the letters) and
       * that is laid over each frame. */
      static image_t *pic;
      static unsigned pic_id;
      static int pic_size = -1, pic_w, pic_n;
      if (!pic || pic_id != sb.id || pic_size != vq.sub_size || pic_w != pl.vw)
      {
         image_free(pic);
         pic = NULL;
         pic_n = sub_render(c, f, sb.text, (pl.vw < c->w ? pl.vw : c->w) - 60, &pic);
         pic_id = sb.id;
         pic_size = vq.sub_size;
         pic_w = pl.vw;
      }
      if (!pic || !pic_n)
         return 0;
      gfx_draw_image(c, pic, (c->w - pic->w) / 2, bottom - pic_n * lh - 3);
      return 1;
   }
}

/* A subtitle's text as a picture, wrapped to maxw: drawn on black and again on white,
 * which tells each pixel's colour and how see-through it is (so the edges stay smooth).
 * Returns the number of lines (0 for none). */
static int sub_render(canvas_t *screen, font_t *f, const char *text, int maxw, image_t **out)
{
   char lines[6][300];
   int nl = 0, i, lh = font_height(f) + 2, w, h;
   const char *p;
   uint32_t *bk, *wh;
   image_t *img;
   (void)screen;
   /* its lines, wrapped to the picture's width */
   for (p = text; *p && nl < 6;)
   {
      char line[300];
      size_t n = strcspn(p, "\n"), at = 0;
      if (n >= sizeof(line))
         n = sizeof(line) - 1;
      memcpy(line, p, n);
      line[n] = 0;
      p += n;
      if (*p == '\n')
         p++;
      while (line[at] && nl < 6)
      {
         /* as many words as fit */
         size_t end = strlen(line + at), best = 0, k;
         char tmp[300];
         for (k = 0; k <= end; k++)
            if (k == end || line[at + k] == ' ')
            {
               memcpy(tmp, line + at, k);
               tmp[k] = 0;
               if (best && font_text_width(f, tmp) > maxw)
                  break;
               best = k;
            }
         if (!best)
            best = end;
         memcpy(lines[nl], line + at, best);
         lines[nl++][best] = 0;
         at += best;
         while (line[at] == ' ')
            at++;
      }
   }
   if (!nl)
      return 0;
   w = maxw + 60;
   h = nl * lh + 6;
   bk = calloc((size_t)w * h, 4);
   wh = malloc((size_t)w * h * 4);
   img = malloc(sizeof(*img));
   if (!bk || !wh || !img)
   {
      free(bk);
      free(wh);
      free(img);
      return 0;
   }
   for (i = 0; i < w * h; i++)
   {
      bk[i] = 0xff000000u;
      wh[i] = 0xffffffffu;
   }
   {
      canvas_t cb = { w, h, w, bk }, cw = { w, h, w, wh };
      for (i = 0; i < nl; i++)
      {
         outlined(&cb, f, lines[i], w / 2, 3 + i * lh);
         outlined(&cw, f, lines[i], w / 2, 3 + i * lh);
      }
   }
   /* on black a pixel is colour x alpha; on white it's that plus (1 - alpha) */
   for (i = 0; i < w * h; i++)
   {
      unsigned b = bk[i], wv = wh[i], a = 255 - (((wv >> 8) & 0xff) - ((b >> 8) & 0xff)), k;
      uint32_t px = 0;
      if (a > 255)
         a = 0;
      if (a)
         for (k = 0; k < 24; k += 8)
         {
            unsigned ch = ((b >> k) & 0xff) * 255 / a;
            px |= (ch > 255 ? 255 : ch) << k;
         }
      bk[i] = px | a << 24;
   }
   free(wh);
   img->w = w;
   img->h = h;
   img->px = bk;
   *out = img;
   return nl;
}

/* the rows the quick menu has for this video */
static int qm_rows(int *rows)
{
   int n = 0, subs = player_sub_count();
   rows[n++] = QM_SUBS;
   if (subs > 1)
      rows[n++] = QM_SUBTRACK;
   if (subs)
      rows[n++] = QM_SUBSIZE;
   if (player_audio_count() > 1)
      rows[n++] = QM_AUDIO;
   rows[n++] = QM_SPEED;
   rows[n++] = QM_PICTURE;
   rows[n++] = QM_AUTOPLAY;
   rows[n++] = QM_SLEEP;
   if (player_info()->has_audio)
      rows[n++] = QM_SOUND;
   rows[n++] = QM_RESTART;
   return n;
}

static void qm_paint(canvas_t *c, double now)
{
   int rows[12], n = qm_rows(rows), i, rh = 34, x = c->w - 356, y = 14, w = 342, h = 48 + n * rh + 70;
   const player_info_t *in = player_info();
   char val[96], info[160];
   if (vq.sel >= n)
      vq.sel = n - 1;
   fill_round_rect(c, x - 1, y - 1, w + 2, h + 2, 15, C_ACCENT);
   fill_round_rect(c, x, y, w, h, 14, C_PANEL);   /* solid: blending it over every frame costs frames */
   gfx_draw_text(c, f_mid, "Video options", x + 20, y + 12, C_TEXT, 0);
   for (i = 0; i < n; i++)
   {
      int ry = y + 48 + i * rh, sel = i == vq.sel, r = rows[i], cur;
      const char *label = "";
      val[0] = 0;
      switch (r)
      {
      case QM_SUBS:
         label = "Subtitles";
         snprintf(val, sizeof(val), "%s", !player_sub_count() ? "None found" : vq.subs ? "On" : "Off");
         break;
      case QM_SUBTRACK:
         label = "Subtitle track";
         cur = player_sub_current();
         snprintf(val, sizeof(val), "%s", cur >= 0 ? player_sub_name(cur) : "Off");
         break;
      case QM_SUBSIZE:
         label = "Subtitle size";
         snprintf(val, sizeof(val), "%s", sub_sizes[vq.sub_size]);
         break;
      case QM_AUDIO:
         label = "Sound track";
         snprintf(val, sizeof(val), "%s", player_audio_name(player_audio_current()));
         break;
      case QM_SPEED:
         label = "Speed";
         snprintf(val, sizeof(val), "%gx", speeds[vq.speed]);
         break;
      case QM_PICTURE:
         label = "Picture";
         snprintf(val, sizeof(val), "%s", pictures[vq.picture]);
         break;
      case QM_AUTOPLAY:
         label = "Play next";
         snprintf(val, sizeof(val), "%s", vq.autoplay ? "On" : "Off");
         break;
      case QM_SLEEP:
         label = "Sleep timer";
         if (vq.sleep_at > now)
            snprintf(val, sizeof(val), "%d min (%d left)", sleeps[vq.sleep], (int)((vq.sleep_at - now) / 60) + 1);
         else
            snprintf(val, sizeof(val), "Off");
         break;
      case QM_SOUND:
         label = "Sound only";
         snprintf(val, sizeof(val), "Screen off");
         break;
      case QM_RESTART:
         label = "Start over";
         break;
      }
      if (sel)
      {
         gfx_fill_rect(c, x + 10, ry + 2, w - 20, rh - 4, C_SEL);
         gfx_fill_rect(c, x + 10, ry + 2, 4, rh - 4, C_ACCENT);
      }
      gfx_draw_text(c, f_small, label, x + 24, ry + (rh - font_height(f_small)) / 2, sel ? C_TEXT : C_DIM, 0);
      if (val[0])
      {
         /* the value at the right, cut short if it's long; < > around it when chosen */
         int ty = ry + (rh - font_height(f_small)) / 2, right = x + w - 22, room = w - 215, vw;
         if (sel)
         {
            right -= font_text_width(f_small, ">");
            gfx_draw_text(c, f_small, ">", right, ty, C_TEXT, 0);
            right -= 10;
         }
         vw = font_text_width(f_small, val);
         if (vw > room)
            vw = room;
         gfx_draw_text(c, f_small, val, right - vw, ty, sel ? C_TEXT : C_DIM, room);
         if (sel)
            gfx_draw_text(c, f_small, "<", right - vw - 10 - font_text_width(f_small, "<"), ty, C_TEXT, 0);
      }
   }
   /* what the file is */
   snprintf(info, sizeof(info), "%dx%d  %s%s%s", in->src_w, in->src_h, in->vcodec,
         in->acodec[0] ? " + " : "", in->acodec);
   if (in->fps > 0)
      snprintf(info + strlen(info), sizeof(info) - strlen(info), "  %.3g fps", in->fps);
   if (rows[vq.sel < n ? vq.sel : 0] == QM_SPEED)
   {
      /* faster than normal needs the frames decoded faster too */
      gfx_draw_text(c, f_small, "Smooth above 1x only for videos", x + 24, y + h - 2 * font_height(f_small) - 14, C_GOLD, w - 48);
      gfx_draw_text(c, f_small, "of 720p or less; bigger ones lag.", x + 24, y + h - font_height(f_small) - 12, C_GOLD, w - 48);
   }
   else
      gfx_draw_text(c, f_small, info, x + 24, y + h - font_height(f_small) - 12, C_FAINT, w - 48);
   draw_hints(c, "Dpad:UD  Choose     Dpad:LR  Change     A  Select     B  Close");
}

/* The panel and its bar change only when something in them does, but the video is
 * redrawn every frame: they're drawn once into a buffer of their own and copied over
 * each frame (drawing all that text every frame cost a third of the frames). */
static void draw_quick_menu(canvas_t *c, double now)
{
   static uint32_t *buf;
   static unsigned sig_last;
   static int bw, bh;
   int rows[12], n = qm_rows(rows), y, x0 = c->w - 358, y1 = 14 + 48 + n * 34 + 70 + 2;
   unsigned sig = 2166136261u;
   int v[] = { vq.sel, vq.subs, vq.sub_size, vq.picture, vq.autoplay, vq.speed, vq.sleep,
      vq.sleep_at > now ? (int)((vq.sleep_at - now) / 60) : -1, player_sub_current(),
      player_audio_current(), n, ft_on, c->w, c->h };
   unsigned k;
   for (k = 0; k < sizeof(v) / sizeof(v[0]); k++)
      sig = (sig ^ (unsigned)v[k]) * 16777619u;
   if (!buf || bw != c->w || bh != c->h)
   {
      free(buf);
      buf = calloc((size_t)c->w * c->h, 4);
      bw = c->w;
      bh = c->h;
      sig_last = ~sig;
      if (!buf)
      {
         qm_paint(c, now);
         return;
      }
   }
   if (sig != sig_last)
   {
      canvas_t b = { bw, bh, bw, buf };
      memset(buf, 0, (size_t)bw * bh * 4);
      qm_paint(&b, now);
      sig_last = sig;
   }
   /* copy what was drawn (the untouched parts are see-through) */
   for (y = 0; y < c->h; y++)
   {
      uint32_t *src = buf + (size_t)y * bw, *dst = c->px + (size_t)y * c->stride;
      int x, xa = 0;
      if (y >= y1 && y < c->h - 40)
         continue;
      if (y < y1 && y < c->h - 40)
         xa = x0 > 0 ? x0 : 0;
      for (x = xa; x < c->w; x++)
         if (src[x] >> 24)
            dst[x] = src[x];
   }
}

static int sound_only_req;   /* the quick menu asked for "Sound only": the main loop turns the screen off */

/* a button with the quick menu open */
static void qm_key(int code, double t)
{
   int rows[12], n = qm_rows(rows), r, dir = code == KEY_LEFT ? -1 : 1, k;
   if (code == KEY_MENU || code == BTN_EAST)
   {
      vq.open = 0;
      pl.redraw = 1;
      return;
   }
   if (code == KEY_UP || code == KEY_DOWN)
   {
      vq.sel = (vq.sel + (code == KEY_UP ? n - 1 : 1)) % n;
      return;
   }
   if (code != KEY_LEFT && code != KEY_RIGHT && code != BTN_SOUTH)
      return;
   r = rows[vq.sel < n ? vq.sel : 0];
   switch (r)
   {
   case QM_SUBS:
      if (!player_sub_count())
      {
         toast("This video has no subtitles", t);
         break;
      }
      vq.subs = !vq.subs;
      vq_apply_subs();
      vq_save();
      break;
   case QM_SUBTRACK:
      if ((n = player_sub_count()) > 0)
      {
         k = player_sub_current();
         k = k < 0 ? 0 : (k + dir + n) % n;
         player_set_sub(k);
         snprintf(vq.sub_name, sizeof(vq.sub_name), "%s", player_sub_name(k));
         vq.subs = 1;
         vq_save();
      }
      break;
   case QM_SUBSIZE:
      vq.sub_size = (vq.sub_size + dir + 3) % 3;
      vq_save();
      break;
   case QM_AUDIO:
      if ((n = player_audio_count()) > 1)
      {
         k = (player_audio_current() + dir + n) % n;
         player_set_audio(k);
         video_layout();
      }
      break;
   case QM_SPEED:
      vq.speed += code == KEY_LEFT ? -1 : 1;
      if (code == BTN_SOUTH && vq.speed >= NSPEEDS)
         vq.speed = 0;
      vq.speed = vq.speed < 0 ? 0 : vq.speed >= NSPEEDS ? NSPEEDS - 1 : vq.speed;
      player_set_speed(speeds[vq.speed]);
      break;
   case QM_PICTURE:
      vq.picture = (vq.picture + dir + 3) % 3;
      video_layout();
      vq_save();
      break;
   case QM_AUTOPLAY:
      vq.autoplay = !vq.autoplay;
      vq_save();
      break;
   case QM_SLEEP:
      vq.sleep = (vq.sleep + dir + NSLEEPS) % NSLEEPS;
      vq.sleep_at = vq.sleep ? t + sleeps[vq.sleep] * 60.0 : 0;
      break;
   case QM_SOUND:
      if (code == BTN_SOUTH)
      {
         sound_only_req = 1;
         vq.open = 0;
         pl.redraw = 1;
      }
      break;
   case QM_RESTART:
      if (code == BTN_SOUTH)
      {
         player_seek(0);
         toast("From the start", t);
         vq.open = 0;
         pl.redraw = 1;
      }
      break;
   }
}

static int vt_source(const char *path, char *jpg, size_t sz);
static void vt_prune(void);
static void menu_unload(void);

/* Playing a video or song works like a game: the menu hands its process to the
 * player (this same program, started with --play) and quits, so none of the menu's
 * memory or work is in the way. When playing stops, the player writes where it got to
 * in MEDIA_RETURN and quits, and the start-up loop starts the menu again on that list. */
#define MEDIA_RETURN "/tmp/g36-media-return"
static int play_req;             /* the menu: hand over to the player after this frame */
static char play_name[256];

static void media_launch(int index)
{
   if (index < 0 || index >= mb.n)
      return;
   snprintf(play_name, sizeof(play_name), "%s", mb.e[index].name);
   play_req = 1;
}

/* the player: where to come back to (the folder's list, on the video or song it got to) */
static void media_return_write(void)
{
   FILE *f = fopen(MEDIA_RETURN ".tmp", "w");
   if (!f)
      return;
   fprintf(f, "%d\n%s\n%s\n%s\n%d\n", mb.mode, mb.base, mb.folder,
         mb.n ? mb.e[mb.sel < mb.n ? mb.sel : 0].name : "", ui.sel_[ROW_MEDIA]);
   fclose(f);
   rename(MEDIA_RETURN ".tmp", MEDIA_RETURN);
}

/* the menu, starting: back on the list a player left (1), or not (0) */
static int media_return_read(double t)
{
   char line[5][1100];
   int i;
   FILE *f = fopen(MEDIA_RETURN, "r");
   if (!f)
      return 0;
   for (i = 0; i < 5; i++)
   {
      if (!fgets(line[i], sizeof(line[i]), f))
         line[i][0] = 0;
      line[i][strcspn(line[i], "\n")] = 0;
   }
   fclose(f);
   unlink(MEDIA_RETURN);
   mb.mode = atoi(line[0]) == MK_MUSIC ? MK_MUSIC : MK_VIDEO;
   media_folders();
   mf.sig = media_folders_sig();
   mf.check = t;
   row_set(ROW_MEDIA, atoi(line[4]));
   ui.anim_[ROW_MEDIA] = ui.pos_[ROW_MEDIA];
   snprintf(mb.base, sizeof(mb.base), "%s", line[1]);
   snprintf(mb.folder, sizeof(mb.folder), "%s", line[2]);
   snprintf(mb.want, sizeof(mb.want), "%s", line[3]);
   mb.sel = mb.top = 0;
   mb.sig = media_scan(0);
   mb.check = t;
   ui.view = 3;
   return 1;
}
static int media_play(int index, double t)
{
   char dir[1100], path[1400];
   const player_info_t *in;
   double start = 0;
   mentry_t *e = &mb.e[index];

   media_dir(dir, sizeof(dir));
   snprintf(path, sizeof(path), "%s/%s", dir, e->name);
   image_free(pl.poster);
   memset(&pl, 0, sizeof(pl));
   pl.kind = e->kind;
   pl.index = index;
   snprintf(pl.key, sizeof(pl.key), "%s%s%s", mb.folder, mb.folder[0] ? "/" : "", e->name);
   media_title(e->name, pl.name, sizeof(pl.name));
   if (pl.kind == MK_VIDEO)
   {
      char fk[1100], file[256];
      double secs;
      progress_folder(fk, sizeof(fk));
      if (progress_get(fk, file, sizeof(file), &secs) && !strcmp(file, e->name))
         start = secs;
      progress_set(fk, e->name, start);
   }
   mlog("media: play %s%s", pl.key, start > 0 ? " (resuming)" : "");
   if (player_open(path, start) < 0)
   {
      mlog("media: %s", player_error());
      toast(player_error(), t);
      return -1;
   }
   in = player_info();
   if (!in->has_video)
      pl.kind = MK_MUSIC;
   if (in->has_video)
   {
      char jpg[1400];
      video_layout();
      /* its picture from the list, to look at while the first frame is decoded */
      menu_unload();   /* the menu's pictures make room for the video while it plays */
      if (vt_source(path, jpg, sizeof(jpg)) && (pl.poster = image_load(jpg)))
      {
         /* scaled once, so redrawing it under the wheel is only a copy */
         image_t *big = image_scaled(pl.poster, pl.vw, pl.vh);
         if (big)
         {
            image_free(pl.poster);
            pl.poster = big;
         }
      }
      player_set_speed(speeds[vq.speed]);
      vq_apply_subs();
   }
   else
      player_set_speed(1);
   if (in->cover)
      pl.cover = square_image(in->cover, COVER_PX);
   pl.overlay_until = t + 3;
   pl.redraw = 1;
   if (start > 0)
   {
      char ts[32], msg[96];
      fmt_time(start, ts, sizeof(ts));
      snprintf(msg, sizeof(msg), "Resumed at %s. Select starts over.", ts);
      toast(msg, t);
      ui.toast_until = t + 3;
   }
   ui.view = 4;
   mb.sel = index;
   return 0;
}

/* the next (or previous) video or song in the folder, -1 if there isn't one */
static int media_neighbour(int from, int dir)
{
   int i;
   for (i = from + dir; i >= 0 && i < mb.n; i += dir)
      if (mb.e[i].kind == pl.kind)
         return i;
   return -1;
}

/* stop. A video keeps its place; one that was nearly over counts as watched, and the
 * folder moves on to the next (or, after the last, is forgotten). */
static void media_stop(int keep_place)
{
   if (!player_is_open())
      return;
   if (pl.kind == MK_VIDEO && keep_place)
   {
      char fk[1100];
      double pos = player_position(), dur = player_info()->duration;
      progress_folder(fk, sizeof(fk));
      if (dur > 0 && dur - pos <= 30)
      {
         int next = media_neighbour(pl.index, 1);
         progress_set(fk, next >= 0 ? mb.e[next].name : FIN_MARK, 0);
         if (next >= 0)
            mb.sel = next;
      }
      else
         progress_set(fk, mb.e[pl.index].name, pos > 30 ? pos : 0);
      mlog("media: stopped %s at %.0f s", pl.key, pos);
   }
   player_close();
   image_free(pl.cover);
   pl.cover = NULL;
   image_free(pl.poster);
   pl.poster = NULL;
   mb.res_key[0] = 0;
   vq.open = 0;
   ui.view = 3;
}

/* another video or song in the folder, from its start */
static void media_switch(int to, double t)
{
   player_close();
   image_free(pl.cover);
   pl.cover = NULL;
   image_free(pl.poster);
   pl.poster = NULL;
   if (media_play(to, t) < 0)
      ui.view = 3;
}

/* a song or video finished: the next one in the folder */
static void media_tick(double t)
{
   int next;
   if (ui.view == 4 && pl.kind == MK_VIDEO && vq.sleep_at && t >= vq.sleep_at)
   {
      vq.sleep_at = 0;
      vq.sleep = 0;
      vq.open = 0;
      mlog("media: sleep timer");
      media_stop(1);
      toast("Sleep timer: stopped", t);
      return;
   }
   if (ui.view != 4 || !player_ended())
      return;
   if (pl.kind == MK_VIDEO && !vq.autoplay)
   {
      vq.open = 0;
      media_stop(1);   /* the folder moves on to the next, for later */
      return;
   }
   if ((next = media_neighbour(pl.index, 1)) >= 0)
   {
      mlog("media: next in the folder");
      media_switch(next, t);
      return;
   }
   if (pl.kind == MK_VIDEO)
   {
      /* the last one: the folder has been watched */
      char fk[1100];
      progress_folder(fk, sizeof(fk));
      progress_set(fk, FIN_MARK, 0);
   }
   media_stop(0);
}

/* buttons on the media screens */
static void media_key(int code, double t)
{
   if (ui.view == 5)
   {
      int n = row_count(ROW_MEDIA);
      if ((code == KEY_LEFT || code == BTN_TL) && n)
         row_move(ROW_MEDIA, -1);
      else if ((code == KEY_RIGHT || code == BTN_TR) && n)
         row_move(ROW_MEDIA, 1);
      else if (code == BTN_SOUTH && n)
         media_open_card(ui.sel_[ROW_MEDIA], t);
      else if (code == BTN_EAST)
         ui.view = 0;
      return;
   }
   if (ui.view == 3)
   {
      if (code == KEY_UP && mb.n)
         mb.sel = (mb.sel + mb.n - 1) % mb.n;
      else if (code == KEY_DOWN && mb.n)
         mb.sel = (mb.sel + 1) % mb.n;
      else if (code == BTN_TL && mb.n)
         mb.sel = mb.sel > LIST_ROWS ? mb.sel - LIST_ROWS : 0;
      else if (code == BTN_TR && mb.n)
         mb.sel = mb.sel + LIST_ROWS < mb.n ? mb.sel + LIST_ROWS : mb.n - 1;
      else if (code == BTN_EAST)
      {
         char *slash = strrchr(mb.folder, '/');
         if (!strcmp(mb.folder, mb.base))
         {
            ui.view = 5;   /* back to the cards */
            media_folders();
            mf.sig = media_folders_sig();
            return;
         }
         snprintf(mb.want, sizeof(mb.want), "%s", slash ? slash + 1 : mb.folder);
         if (slash)
            *slash = 0;
         else
            mb.folder[0] = 0;
         mb.sig = media_scan(0);
      }
      else if (code == BTN_SOUTH && mb.n)
      {
         mentry_t *e = &mb.e[mb.sel];
         if (e->kind == MK_DIR)
         {
            size_t l = strlen(mb.folder);
            snprintf(mb.folder + l, sizeof(mb.folder) - l, "%s%s", l ? "/" : "", e->name);
            media_enter(t);
         }
         else
            media_launch(mb.sel);
      }
      return;
   }

   /* playing */
   if (vq.open)
   {
      qm_key(code, t);
      return;
   }
   if (code == KEY_MENU && pl.kind == MK_VIDEO)
   {
      vq.open = 1;
      vq.sel = 0;
      pl.redraw = 1;
      return;
   }
   pl.overlay_until = t + 3;
   if (code == BTN_SOUTH)
      player_set_paused(!player_paused());
   else if (code == BTN_EAST)
      media_stop(1);
   else if (code == KEY_LEFT || code == KEY_RIGHT)
      player_seek_by(code == KEY_LEFT ? -10 : 10);
   else if (code == BTN_SELECT && pl.kind == MK_VIDEO)
   {
      player_seek(0);
      toast("From the start", t);
   }
   else if ((code == KEY_UP || code == KEY_DOWN) && pl.kind == MK_VIDEO)
      player_seek_by(code == KEY_DOWN ? -60 : 60);
   else if ((code == BTN_TL || code == BTN_TR) && pl.kind == MK_VIDEO)
   {
      int to = media_neighbour(pl.index, code == BTN_TR ? 1 : -1);
      if (to < 0)
         toast(code == BTN_TR ? "That's the last video here" : "That's the first video here", t);
      else
         media_switch(to, t);
   }
   else if (code == BTN_TL || code == BTN_TR)
   {
      /* songs: back to the start of this one first (like most players), then the one before */
      int to = code == BTN_TR ? media_neighbour(pl.index, 1)
            : player_position() > 3 ? -2 : media_neighbour(pl.index, -1);
      if (to == -2)
         player_seek(0);
      else if (to >= 0)
         media_switch(to, t);
   }
}

/* ---- pictures for videos: a frame from each, made once on a background thread and
 * kept in thumbnails/videos/, in the same folders and with the same names as the
 * videos, so a picture of your own can simply replace one (.jpg or .png) */
#define VT_DIR  SDCARD "/thumbnails/videos"
#define VT_W    260
#define VT_H    195
#define VT_MAX  48
#define VS_W    43   /* the small picture beside a video in the lists: the frame fitted into this */
#define VS_H    24
enum { VT_FREE, VT_WANT, VT_BUSY, VT_READY, VT_LOADED, VT_NONE };

static struct
{
   char path[1400], jpg[1400];
   int state;
   image_t *img, *small;
   double used;              /* last asked for: the newest is made first */
} vt[VT_MAX];
static pthread_mutex_t vt_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vt_c = PTHREAD_COND_INITIALIZER;
static int vt_paused, vt_started;

/* the folders a picture goes in */
static void mkdirs_for(const char *file)
{
   char p[1400], *s;
   snprintf(p, sizeof(p), "%s", file);
   for (s = p + 1; *s; s++)
      if (*s == '/')
      {
         *s = 0;
         mkdir(p, 0755);
         *s = '/';
      }
}

static void *vt_main(void *arg)
{
   (void)arg;
   pthread_mutex_lock(&vt_m);
   for (;;)
   {
      char path[1400], jpg[1400];
      int i, best = -1, ok;
      double t0;
      for (i = 0; i < VT_MAX; i++)
         if (vt[i].state == VT_WANT && (best < 0 || vt[i].used > vt[best].used))
            best = i;
      if (best < 0 || vt_paused)
      {
         pthread_cond_wait(&vt_c, &vt_m);
         continue;
      }
      vt[best].state = VT_BUSY;
      snprintf(path, sizeof(path), "%s", vt[best].path);
      snprintf(jpg, sizeof(jpg), "%s", vt[best].jpg);
      pthread_mutex_unlock(&vt_m);
      t0 = now_s();
      mkdirs_for(jpg);
      ok = access(jpg, F_OK) == 0 || video_thumbnail(path, jpg, VT_W, VT_H) == 0;
      if (!ok)
         mlog("media: no picture from %s", path);
      else if (now_s() - t0 > 0.05)
         mlog("media: picture made in %.1f s", now_s() - t0);
      pthread_mutex_lock(&vt_m);
      if (vt[best].state == VT_BUSY && !strcmp(vt[best].path, path))
         vt[best].state = ok ? VT_READY : VT_NONE;
   }
   return NULL;
}

/* where a video's picture goes: thumbnails/videos/<path inside videos/>.png or .jpg;
 * 1 if one is already there */
static int vt_source(const char *path, char *jpg, size_t sz)
{
   char base[1400], *dot;
   size_t n = strlen(VIDEOS);
   const char *rel = !strncmp(path, VIDEOS "/", n + 1) ? path + n + 1 : path;
   snprintf(base, sizeof(base), "%s/%s", VT_DIR, rel);
   if ((dot = strrchr(base, '.')) && dot > strrchr(base, '/'))
      *dot = 0;
   snprintf(jpg, sz, "%s.png", base);   /* a PNG can only be one of yours */
   if (access(jpg, F_OK) == 0)
      return 1;
   snprintf(jpg, sz, "%s.jpg", base);
   return access(jpg, F_OK) == 0;
}

/* Pictures for videos that are gone: thumbnails/videos/<folder>/<name>.jpg is kept only
 * while videos/<folder>/<name>.<video type> exists. (Your own .png pictures are left.)
 * Run when the Video Player card is opened, so a video deleted over the USB cable or off
 * the card takes its picture with it; a folder left empty goes too. */
static int vt_has_video(const char *rel_dir, const char *base)
{
   char dir[1400];
   DIR *d;
   struct dirent *de;
   int found = 0;
   size_t bl = strlen(base);
   snprintf(dir, sizeof(dir), "%s%s%s", VIDEOS, rel_dir[0] ? "/" : "", rel_dir);
   if (!(d = opendir(dir)))
      return 0;
   while (!found && (de = readdir(d)))
      found = !strncmp(de->d_name, base, bl) && de->d_name[bl] == '.' && media_kind(de->d_name) == MK_VIDEO &&
            !strchr(de->d_name + bl + 1, '.');
   closedir(d);
   return found;
}

static int vt_prune_dir(const char *rel)
{
   char dir[1400], path[1500];
   DIR *d;
   struct dirent *de;
   int removed = 0, left = 0;
   snprintf(dir, sizeof(dir), "%s%s%s", VT_DIR, rel[0] ? "/" : "", rel);
   if (!(d = opendir(dir)))
      return 0;
   while ((de = readdir(d)))
   {
      char sub[1400], base[300];
      size_t l = strlen(de->d_name);
      struct stat st;
      if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
         continue;
      snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
      if (stat(path, &st) < 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         snprintf(sub, sizeof(sub), "%s%s%s", rel, rel[0] ? "/" : "", de->d_name);
         removed += vt_prune_dir(sub);
         if (rmdir(path) < 0)
            left++;   /* not empty: something stays in it */
      }
      else if (l > 4 && !strcasecmp(de->d_name + l - 4, ".jpg"))
      {
         snprintf(base, sizeof(base), "%.*s", (int)(l - 4), de->d_name);
         if (!vt_has_video(rel, base))
         {
            unlink(path);
            removed++;
         }
         else
            left++;
      }
      else
         left++;
   }
   closedir(d);
   (void)left;
   return removed;
}

static void vt_prune(void)
{
   int n;
   if (access(VIDEOS, R_OK) < 0)
      return;   /* no videos folder to judge by (the card isn't there): remove nothing */
   n = vt_prune_dir("");
   if (n)
      mlog("media: removed %d pictures of videos that are gone", n);
}

/* a video's picture, or NULL while it's being made (*state says which): made the first
 * time the video is selected; load = 0 only asks for it */
static image_t *vthumb_slot(const char *path, double used, int load, int *state, image_t **small)
{
   int i, slot = -1;
   image_t *img = NULL;
   pthread_mutex_lock(&vt_m);
   if (!vt_started)
   {
      pthread_t th;
      mkdir(SDCARD "/thumbnails", 0755);
      mkdir(VT_DIR, 0755);
      if (pthread_create(&th, NULL, vt_main, NULL) == 0)
         pthread_detach(th);
      vt_started = 1;
   }
   for (i = 0; i < VT_MAX && slot < 0; i++)
      if (vt[i].state != VT_FREE && !strcmp(vt[i].path, path))
         slot = i;
   if (slot < 0)
   {
      /* a free slot, or the one unused the longest (not one being made) */
      for (i = 0; i < VT_MAX; i++)
         if (vt[i].state != VT_BUSY && (slot < 0 || vt[i].state == VT_FREE ||
               (vt[slot].state != VT_FREE && vt[i].used < vt[slot].used)))
            slot = i;
      if (slot < 0)
      {
         pthread_mutex_unlock(&vt_m);
         *state = VT_WANT;
         return NULL;
      }
      image_free(vt[slot].img);
      image_free(vt[slot].small);
      vt[slot].img = vt[slot].small = NULL;
      snprintf(vt[slot].path, sizeof(vt[slot].path), "%s", path);
      vt[slot].state = vt_source(path, vt[slot].jpg, sizeof(vt[slot].jpg)) ? VT_READY : VT_WANT;
      pthread_cond_signal(&vt_c);
   }
   if (used > vt[slot].used || vt[slot].state == VT_WANT)
      vt[slot].used = used;
   if (load && vt[slot].state == VT_READY)
   {
      /* loaded here (quick: they're small), fitted into the panel's box */
      image_t *raw = image_load(vt[slot].jpg);
      vt[slot].state = VT_NONE;
      if (raw)
      {
         double sx = (double)VT_W / raw->w, sy = (double)VT_H / raw->h, sc = sx < sy ? sx : sy;
         if (sc < 1)
         {
            vt[slot].img = image_scaled(raw, (int)(raw->w * sc), (int)(raw->h * sc));
            image_free(raw);
         }
         else
            vt[slot].img = raw;
         if (vt[slot].img)
         {
            /* the list's small picture: the same frame, fitted */
            const image_t *f = vt[slot].img;
            double sx = (double)VS_W / f->w, sy = (double)VS_H / f->h, sc = sx < sy ? sx : sy;
            int w = (int)(f->w * sc + 0.5), h = (int)(f->h * sc + 0.5);
            vt[slot].small = image_scaled(f, w > 0 ? w : 1, h > 0 ? h : 1);
         }
         vt[slot].state = vt[slot].img ? VT_LOADED : VT_NONE;
      }
   }
   if (vt[slot].state == VT_LOADED)
   {
      img = vt[slot].img;
      if (small)
         *small = vt[slot].small;
   }
   *state = vt[slot].state;
   pthread_mutex_unlock(&vt_m);
   return img;
}

static image_t *vthumb_get(const char *path, double used, int load, int *state)
{
   return vthumb_slot(path, used, load, state, NULL);
}

/* while a video plays, the pictures wait (the player needs the processor) */
static void vthumb_pause(int paused)
{
   pthread_mutex_lock(&vt_m);
   if (paused != vt_paused)
   {
      vt_paused = paused;
      pthread_cond_signal(&vt_c);
   }
   pthread_mutex_unlock(&vt_m);
}

static image_t *mword_cache[3];   /* dropped when the theme changes */

/* While a video plays the menu isn't drawn: its pictures (cards, box art, the video
 * pictures, the background) are let go, leaving the memory to the video. Each is made
 * again when it's next needed. */
static void menu_unload(void)
{
   int r, k;
   thumbs_clear();
   for (r = 0; r <= NROWS; r++)
      for (k = 0; k < MAX_SYSTEMS; k++)
      {
         cardimg_t *ci = &card_cache[r][k];
         free(ci->img.px);
         ci->img.px = NULL;
         image_free(ci->half);
         image_free(ci->scaled);
         ci->half = ci->scaled = NULL;
         ci->key[0] = 0;
         ci->last_w = ci->last_h = 0;
      }
   for (k = 0; k < 3; k++)
   {
      image_free(mword_cache[k]);
      mword_cache[k] = NULL;
   }
   free(bg);
   bg = NULL;
   free(bg_stage);
   bg_stage = NULL;
   bg_cur = NULL;
   pthread_mutex_lock(&vt_m);
   for (k = 0; k < VT_MAX; k++)
      if (vt[k].state == VT_LOADED || vt[k].state == VT_READY)
      {
         image_free(vt[k].img);
         image_free(vt[k].small);
         vt[k].img = vt[k].small = NULL;
         vt[k].state = VT_READY;   /* loaded again from its file when it's next shown */
      }
   pthread_mutex_unlock(&vt_m);
}

/* the panel's big word, in the logos' lettering (made once) */
static image_t *media_word(int kind)
{
   image_t **cache = mword_cache;
   static const char *const words[] = { "Folder", "Video", "Music" };
   if (!cache[kind] && access(LOGO_FONT, F_OK) == 0)
      cache[kind] = gradient_mask(words[kind], 230, 80);
   return cache[kind];
}

static void draw_progress(canvas_t *c, int x, int y, int w, double pos, double dur)
{
   int fill = dur > 0 ? (int)(w * (pos / dur)) : 0;
   if (fill > w)
      fill = w;
   fill_round_rect(c, x, y, w, 8, 4, C_SEL);
   if (fill >= 8)
      fill_round_rect(c, x, y, fill, 8, 4, C_ACCENT);
}

/* the Video and Music Players' version: the contents of a folder inside the one being looked at (a season, say):
 * folders first (in brackets, like the games lists' sub-folders), then the videos or songs, named as the lists
 * name them */
static void draw_media_folder_contents(canvas_t *c, const mentry_t *e, double now)
{
   char dir[1100], sub[1400], key[1500];
   media_dir(dir, sizeof(dir));
   snprintf(sub, sizeof(sub), "%s/%s", dir, e->name);
   snprintf(key, sizeof(key), "m%d|%s", mb.mode, sub);
   if (fold_stale(key, (int)mb.sig, now))
   {
      char (*dirs)[256] = malloc(512 * 256), (*files)[256] = malloc(512 * 256);
      int nd = 0, nf = 0, i;
      DIR *d = opendir(sub);
      struct dirent *de;
      list_free(&fold_c.l);
      if (dirs && files && d)
      {
         while ((de = readdir(d)))
         {
            char p[1500];
            struct stat sb;
            if (de->d_name[0] == '.')
               continue;
            snprintf(p, sizeof(p), "%s/%s", sub, de->d_name);
            if (stat(p, &sb) < 0)
               continue;
            if (S_ISDIR(sb.st_mode))
            {
               if (nd < 512)
                  snprintf(dirs[nd++], 256, "%s", de->d_name);
            }
            else if (media_kind(de->d_name) == mb.mode && nf < 512)
               snprintf(files[nf++], 256, "%s", de->d_name);
         }
         qsort(dirs, nd, 256, mf_cmp);
         qsort(files, nf, 256, mf_cmp);
         for (i = 0; i < nd; i++)
            list_add(&fold_c.l, dirs[i], dirs[i], 1, 0);
         for (i = 0; i < nf; i++)
         {
            char title[256];
            media_title(files[i], title, sizeof(title));
            list_add(&fold_c.l, files[i], title, 0, 0);
         }
      }
      if (d)
         closedir(d);
      free(dirs);
      free(files);
   }
   fold_show(c, now);
}

/* A music note for the Music Player's list rows, drawn like the folder picture (same place, size and theme
 * colours, lighter at the top and deeper at the bottom): an eighth note, with a tilted oval head, a stem and
 * a curved flag. Everything is smoothed at the edges. The colours come from the theme's accent mixed into
 * bg, the colour behind it. */
typedef struct { uint32_t top, bot; float y0, h; } note_paint_t;

static void note_put(canvas_t *c, int x, int y, float cov, const note_paint_t *np)
{
   int t = (int)((y + 0.5f - np->y0) * 256.0f / np->h);
   if (cov > 0 && x >= 0 && y >= 0 && x < c->w && y < c->h)
      fill_px(c, x, y, mix_colour(np->top, np->bot, t < 0 ? 0 : t > 256 ? 256 : t), cov > 1 ? 1 : cov);
}

static void note_ellipse(canvas_t *c, float cx, float cy, float rx, float ry, float ang, const note_paint_t *np)
{
   float ca = cosf(ang), sa = sinf(ang), rmin = rx < ry ? rx : ry;
   int x, y, x0 = (int)(cx - rx - 2), x1 = (int)(cx + rx + 2), y0 = (int)(cy - rx - 2), y1 = (int)(cy + rx + 2);
   for (y = y0; y <= y1; y++)
      for (x = x0; x <= x1; x++)
      {
         float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
         float u = dx * ca + dy * sa, v = -dx * sa + dy * ca;
         note_put(c, x, y, 0.5f - (sqrtf(u * u / (rx * rx) + v * v / (ry * ry)) - 1.0f) * rmin, np);
      }
}

static void note_line(canvas_t *c, float ax, float ay, float bx, float by, float half, const note_paint_t *np)
{
   float vx = bx - ax, vy = by - ay, len2 = vx * vx + vy * vy;
   int x, y, x0 = (int)(ax < bx ? ax : bx) - (int)half - 2, x1 = (int)(ax > bx ? ax : bx) + (int)half + 2;
   int y0 = (int)(ay < by ? ay : by) - (int)half - 2, y1 = (int)(ay > by ? ay : by) + (int)half + 2;
   for (y = y0; y <= y1; y++)
      for (x = x0; x <= x1; x++)
      {
         float px = x + 0.5f - ax, py = y + 0.5f - ay;
         float t = len2 > 0 ? (px * vx + py * vy) / len2 : 0, dx, dy;
         t = t < 0 ? 0 : t > 1 ? 1 : t;
         dx = px - t * vx;
         dy = py - t * vy;
         note_put(c, x, y, 0.5f - (sqrtf(dx * dx + dy * dy) - half), np);
      }
}

static void draw_music_note(canvas_t *c, int x, int y, int w, int h, uint32_t bg)
{
   float k = (float)h / 25.0f, ox = x + (w - 31.0f * k) / 2 + 1.0f * k;      /* drawn in a 31 x 25 box, scaled to fit */
   note_paint_t np = { mix_colour(bg, C_ACCENT, 255), mix_colour(bg, C_ACCENT, 185), y + 2.0f * k, 23.0f * k };
   static const float flag[5][3] = { { 15.4f, 2.6f, 1.9f }, { 18.8f, 4.0f, 1.8f }, { 21.5f, 7.0f, 1.6f },
                                     { 22.3f, 10.8f, 1.4f }, { 20.4f, 14.4f, 1.1f } };
   int i;
   note_line(c, ox + 15.2f * k, y + 18.0f * k, ox + 15.2f * k, y + 2.6f * k, 1.45f * k, &np);   /* the stem */
   for (i = 0; i < 4; i++)                                                                    /* the flag */
      note_line(c, ox + flag[i][0] * k, y + flag[i][1] * k, ox + flag[i + 1][0] * k, y + flag[i + 1][1] * k,
            flag[i][2] * k, &np);
   note_ellipse(c, ox + 10.8f * k, y + 19.2f * k, 6.4f * k, 4.6f * k, -0.42f, &np);           /* the head */
}

static void draw_media_list(canvas_t *c, double now)
{
   int has_dirs = 0, has_vids = 0, icons, tx;
   char title[1200], name[300], vdir[1100];
   int i;

   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   if (mb.folder[0])
      snprintf(title, sizeof(title), "%s  /  %s", mb.mode == MK_VIDEO ? "Video Player" : "Music Player", mb.folder);
   else
      snprintf(title, sizeof(title), "%s", mb.mode == MK_VIDEO ? "Video Player" : "Music Player");
   gfx_draw_text(c, f_big, title, 20, (50 - font_height(f_big)) / 2, C_TEXT, 600);
   snprintf(name, sizeof(name), "%d / %d", mb.n ? mb.sel + 1 : 0, mb.n);
   {
      int sw = draw_status(c, c->w - 16, 25, 0);
      gfx_draw_text(c, f_small, name, c->w - 20 - sw - 14 - font_text_width(f_small, name),
            (50 - font_height(f_small)) / 2, C_DIM, 0);
   }

   if (!mb.n)
   {
      int y = 150;
      draw_centered(c, f_big, "Nothing here yet", c->w / 2, y, C_TEXT);
      draw_centered(c, f_mid, mb.mode == MK_VIDEO ? "Copy videos into the videos folder on"
            : "Copy music into the music folder on", c->w / 2, y + 60, C_DIM);
      draw_centered(c, f_mid, "G36-GAMES (file transfer from a computer works).", c->w / 2, y + 92, C_DIM);
      if (mb.mode == MK_MUSIC)
         draw_centered(c, f_small, "Album art: a cover.jpg in the folder, or the picture inside the file.", c->w / 2, y + 150, C_FAINT);
      draw_hints(c, mb.mode == MK_VIDEO ? "B  Back     X  Reset watch progress" : "B  Back");
      draw_toast(c, now);
      draw_volume(c, now);
      return;
   }

   if (mb.sel < mb.top)
      mb.top = mb.sel;
   if (mb.sel >= mb.top + LIST_ROWS)
      mb.top = mb.sel - LIST_ROWS + 1;
   /* with folders in the list every name starts after the folder pictures' column, so they line up */
   for (i = 0; i < mb.n && !has_dirs; i++)
      has_dirs = mb.e[i].kind == MK_DIR;
   for (i = 0; i < mb.n && !has_vids; i++)
      has_vids = mb.e[i].kind == MK_VIDEO;
   icons = has_dirs || has_vids || mb.mode == MK_MUSIC;   /* songs have their note, videos their picture */
   tx = has_vids ? 24 + VS_W + 10 : 60;
   media_dir(vdir, sizeof(vdir));
   for (i = 0; i < LIST_ROWS && mb.top + i < mb.n; i++)
   {
      const mentry_t *e = &mb.e[mb.top + i];
      int y = LIST_Y + i * ROW_H, sel = mb.top + i == mb.sel;
      if (sel)
      {
         gfx_fill_rect(c, 12, y, 518, ROW_H, C_SEL);
         gfx_fill_rect(c, 12, y, 4, ROW_H, C_ACCENT);
      }
      if (e->kind == MK_DIR)
      {
         /* a folder: its small picture, as in the game lists, and its name */
         draw_folder_shape(c, 23, y + (ROW_H - 25) / 2, 31, 25, TH(TH_PICBOX));
         snprintf(name, sizeof(name), "%s", e->name);
      }
      else
      {
         /* a song: the music note in the folder pictures' place */
         if (e->kind == MK_MUSIC)
            draw_music_note(c, 23, y + (ROW_H - 25) / 2, 31, 25, TH(TH_PICBOX));
         else if (e->kind == MK_VIDEO)
         {
            /* a video: a frame from it, small (made in the background, nearest the selection first) */
            char path[1400];
            image_t *sm = NULL;
            int st, ty = y + (ROW_H - VS_H) / 2;
            snprintf(path, sizeof(path), "%s/%s", vdir, e->name);
            vthumb_slot(path, now - 0.01 * abs(mb.top + i - mb.sel), 1, &st, &sm);
            gfx_fill_rect(c, 24, ty, VS_W, VS_H, sm ? 0xff140c18 : C_THUMB);
            if (sm)
               gfx_draw_image(c, sm, 24 + (VS_W - sm->w) / 2, ty + (VS_H - sm->h) / 2);
         }
         media_title(e->name, name, sizeof(name));
      }
      gfx_draw_text(c, f_mid, name, icons ? tx : 30, y + (ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, icons ? 520 - tx : 490);
   }
   if (mb.n > LIST_ROWS)
   {
      int track = LIST_ROWS * ROW_H, bar = track * LIST_ROWS / mb.n;
      gfx_fill_rect(c, 534, LIST_Y, 3, track, C_FAINT);
      gfx_fill_rect(c, 534, LIST_Y + (track - bar) * mb.top / (mb.n - LIST_ROWS), 3, bar, C_ACCENT);
   }

   /* details panel */
   gfx_fill_rect(c, 552, LIST_Y, 290, 360, C_PANEL);
   {
      const mentry_t *e = &mb.e[mb.sel];
      const char *dot = strrchr(e->name, '.');
      image_t *word = e->kind == MK_VIDEO || e->kind == MK_DIR ? NULL : media_word(e->kind);
      char info[96];
      if (e->kind == MK_VIDEO)
      {
         char dir[1100], path[1400];
         int st;
         image_t *pic;
         media_dir(dir, sizeof(dir));
         snprintf(path, sizeof(path), "%s/%s", dir, e->name);
         pic = vthumb_get(path, now, 1, &st);
         gfx_fill_rect(c, 567, LIST_Y + 15, VT_W, VT_H, 0xff140c18);
         if (pic)
            gfx_draw_image_smooth(c, pic, 567 + (VT_W - pic->w) / 2, LIST_Y + 15 + (VT_H - pic->h) / 2, pic->w, pic->h);
         else
            draw_centered(c, f_small, st == VT_NONE ? "No picture" : "Getting a picture...", 697,
                  LIST_Y + 15 + VT_H / 2 - font_height(f_small) / 2, C_FAINT);
      }
      else if (e->kind == MK_DIR)
      {
         /* a folder: the same picture as in the game lists */
         gfx_fill_rect(c, 567, LIST_Y + 15, 260, 195, TH(TH_PICBOX));
         draw_folder_icon(c);
      }
      else
         fill_vgradient(c, 567, LIST_Y + 15, 260, 195, TH(TH_CARD_TOP), TH(TH_CARD_BOT));
      if (word)
      {
         /* folders gray, like their cards */
         grad = e->kind == MK_DIR ? grad_gray : grad_logo;
         grad_edge = e->kind == MK_DIR ? 0xff3b3946 : C_LET_EDGE;
         draw_gradient_mask(c, word, 697, LIST_Y + 15 + (195 - gradient_height(word)) / 2, 0);
      }
      if (e->kind == MK_DIR)
         snprintf(name, sizeof(name), "%s", e->name);
      else
         media_title(e->name, name, sizeof(name));
      gfx_draw_text(c, f_mid, name, 567, LIST_Y + 225, C_TEXT, 260);
      if (e->kind == MK_DIR)
         draw_media_folder_contents(c, e, now);
      if (e->kind == MK_DIR)
         snprintf(info, sizeof(info), "Folder");
      else
      {
         int k;
         snprintf(info, sizeof(info), "%s %s", dot ? dot + 1 : "", e->kind == MK_VIDEO ? "video" : "music");
         for (k = 0; info[k] && info[k] != ' '; k++)
            info[k] = toupper((unsigned char)info[k]);
      }
      if (e->kind != MK_VIDEO && e->kind != MK_DIR)   /* (a folder shows its contents there instead) */
         gfx_draw_text(c, f_small, info, 567, LIST_Y + 262, C_DIM, 260);
      if (e->kind == MK_VIDEO)
      {
         char fk[1100];
         progress_folder(fk, sizeof(fk));
         if (strcmp(fk, mb.res_key))
         {
            /* looked up once per folder (and after anything changes it) */
            snprintf(mb.res_key, sizeof(mb.res_key), "%s", fk);
            if (!progress_get(fk, mb.res_file, sizeof(mb.res_file), &mb.res_val))
               mb.res_file[0] = 0;
         }
         if (strcmp(e->name, mb.res_file))
            gfx_draw_text(c, f_small, info, 567, LIST_Y + 262, C_DIM, 260);
         else
         {
            char ts[32], line[64];
            fmt_time(mb.res_val, ts, sizeof(ts));
            if (mb.res_val > 0)
               snprintf(line, sizeof(line), "Resumes at %s", ts);
            else
               snprintf(line, sizeof(line), "Up next");
            gfx_draw_text(c, f_small, line, 567, LIST_Y + 262, C_ACCENT, 260);
         }
      }
   }
   if (mb.mode == MK_VIDEO)
      draw_hints(c, mb.e[mb.sel].kind == MK_DIR ? "A  Open folder     B  Back     L / R  Page     X  Reset watch progress"
            : "A  Play     B  Back     L / R  Page     X  Reset watch progress");
   else
      draw_hints(c, mb.e[mb.sel].kind == MK_DIR ? "A  Open folder     B  Back     L / R  Page"
            : "A  Play     B  Back     L / R  Page");
   draw_toast(c, now);
   draw_volume(c, now);
}

/* Where you are in each of the Video Player's folders, for the bar on its card: the last video you were on in it
 * (a folder inside it, a season, counts too), how far into it you got, and how many of the folder's videos come
 * before that one (those count as watched). The file is read again when it changes. */
static struct { char ep[200]; double secs; int has, fin, watched, total; } fp_c[MAX_SYSTEMS + 1];
static unsigned fp_key;
static int fp_ready;
static double fp_checked;

/* the last part of a folder path */
static const char *last_part(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

static int folder_progress(int card, char *ep, size_t esz, double *secs, int *fin, int *watched, int *total)
{
   struct stat st;
   unsigned key;
   double t = now_s();
   if (card < 0 || card > mf.n || card > MAX_SYSTEMS)
      return 0;
   if (!fp_ready || t - fp_checked > 0.5)
   {
      fp_checked = t;
      if (stat(MEDIA_RESUME, &st))
      {
         st.st_mtime = 0;
         st.st_size = 0;
      }
      key = (unsigned)st.st_mtime * 31u + (unsigned)st.st_size * 7u + mf.sig * 17u + (unsigned)mf.n;
      if (!fp_ready || key != fp_key)
      {
         static char tf[MAX_SYSTEMS + 1][512], tfile[MAX_SYSTEMS + 1][256];
         char line[1500], *fo, *fi;
         double v;
         int j;
         FILE *f = fopen(MEDIA_RESUME, "r");
         memset(fp_c, 0, sizeof(fp_c));
         while (f && fgets(line, sizeof(line), f))
            if (progress_parse(line, &fo, &fi, &v))
               for (j = 0; j <= mf.n && j <= MAX_SYSTEMS; j++)
               {
                  const char *nm = j < mf.n ? mf.name[j] : ".";
                  size_t nl = strlen(nm);
                  if (!strcmp(fo, nm) || (j < mf.n && !strncmp(fo, nm, nl) && fo[nl] == '/'))
                  {
                     /* the last line is the most recent */
                     fp_c[j].fin = !strcmp(fi, FIN_MARK);
                     if (fp_c[j].fin)
                        snprintf(fp_c[j].ep, sizeof(fp_c[j].ep), "%s", !strcmp(fo, nm) ? "" : last_part(fo));
                     else if (!strcmp(fo, nm) || j >= mf.n)
                        media_title(fi, fp_c[j].ep, sizeof(fp_c[j].ep));
                     else
                     {
                        /* a folder inside the show (a season): say which */
                        char title[200];
                        media_title(fi, title, sizeof(title));
                        snprintf(fp_c[j].ep, sizeof(fp_c[j].ep), "%s - %s", last_part(fo), title);
                     }
                     fp_c[j].secs = v;
                     fp_c[j].has = 1;
                     snprintf(tf[j], sizeof(tf[j]), "%s", fo);
                     snprintf(tfile[j], sizeof(tfile[j]), "%s", fi);
                  }
               }
         if (f)
            fclose(f);
         for (j = 0; j <= mf.n && j <= MAX_SYSTEMS; j++)
         {
            int n = 0, ok;
            fp_c[j].total = j < mf.n ? mf.count[j] : mf.loose;
            if (!fp_c[j].has)
               continue;
            if (j < mf.n)
               ok = media_walk(VIDEOS, mf.name[j], tf[j], tfile[j], &n, 0, 0);
            else
               ok = media_walk(VIDEOS, "", tf[j], tfile[j], &n, 1, 0);
            fp_c[j].watched = ok ? n : 0;
            if (fp_c[j].watched > fp_c[j].total)
               fp_c[j].watched = fp_c[j].total;
         }
         fp_key = key;
         fp_ready = 1;
      }
   }
   *total = card < mf.n ? mf.count[card] : mf.loose;
   *watched = fp_c[card].has ? fp_c[card].watched : 0;
   *fin = 0;
   *secs = 0;
   if (!fp_c[card].has)
      return 0;
   snprintf(ep, esz, "%s", fp_c[card].ep);
   *secs = fp_c[card].secs;
   *fin = fp_c[card].fin;
   return 1;
}

/* the players' first screen: their folders, as one row of cards like the home screen */
static void draw_media_carousel(canvas_t *c, double now)
{
   static card_t cards[MAX_SYSTEMS + 4];
   int i, n = 0, cnt = row_count(ROW_MEDIA);

   draw_background(c);
   for (i = 0; i < cnt; i++)
   {
      double off = item_offset(ROW_MEDIA, i);
      int cx = c->w / 2 + (int)(off * CARD_GAP) + side_push(off);
      if (cx < -CARD_W || cx > c->w + CARD_W)
         continue;
      cards[n].r = ROW_MEDIA;
      cards[n].i = i;
      cards[n].cx = cx;
      cards[n].cy = HOME_CY;
      cards[n].scale = card_scale_x(fabs(off));
      cards[n].depth = fabs(off);
      cards[n].rowd = 0;
      n++;
   }
   qsort(cards, n, sizeof(card_t), card_cmp);
   smooth_budget = 2;
   for (i = 0; i < n; i++)
      draw_card(c, &cards[i], cards[i].i == ui.sel_[ROW_MEDIA] && cards[i].depth < 0.5);
   if (!cnt)
   {
      draw_centered(c, f_big, "Nothing here yet", c->w / 2, HOME_CY - 60, C_TEXT);
      draw_centered(c, f_small, mb.mode == MK_VIDEO ? "Copy videos into videos/ on G36-GAMES, a folder for each show or film."
            : "Copy music into music/ on G36-GAMES, a folder for each album or artist.", c->w / 2, HOME_CY, C_DIM);
      draw_centered(c, f_small, "File transfer from a computer works.", c->w / 2, HOME_CY + 30, C_DIM);
   }
   /* one row only: no row title, and the battery in the top right corner like the other screens */
   draw_status(c, c->w - 16, 25, 0);
   draw_toast(c, now);
   draw_volume(c, now);
   draw_hints(c, mb.mode == MK_VIDEO ? "A  Open     Dpad  Navigation     B  Back     X  Reset watch progress"
         : "A  Open     Dpad  Navigation     B  Back");
}

/* the pause sign, centred on (cx, cy) */
static void draw_pause_sign(canvas_t *c, int cx, int cy)
{
   fill_round_rect(c, cx - 44, cy - 44, 88, 88, 44, 0xc0130b16);
   fill_round_rect(c, cx - 15, cy - 20, 10, 40, 3, C_TEXT);
   fill_round_rect(c, cx + 5, cy - 20, 10, 40, 3, C_TEXT);
}

/* the video's controls, along the bottom */
static void draw_video_overlay(canvas_t *c, double now)
{
   const player_info_t *in = player_info();
   double pos = player_position();
   char a[32], b[32], tm[80];
   int y = c->h - 104;
   (void)now;
   gfx_fill_rect(c, 0, y, c->w, 104, 0xd0130b16);
   fmt_time(pos, a, sizeof(a));
   fmt_time(in->duration, b, sizeof(b));
   snprintf(tm, sizeof(tm), "%s / %s", a, b);
   gfx_draw_text(c, f_mid, in->title[0] ? in->title : pl.name, 24, y + 12, C_TEXT,
         c->w - 72 - font_text_width(f_small, tm));
   gfx_draw_text(c, f_small, tm, c->w - 24 - font_text_width(f_small, tm), y + 15, C_DIM, 0);
   draw_progress(c, 24, y + 50, c->w - 48, pos, in->duration);
   draw_hint_boxes(c, player_paused()
         ? "A  Play     Dpad:LR  10 s     Dpad:UD  1 min     L / R  Previous / next     B  Stop     Menu  Options"
         : "A  Pause     Dpad:LR  10 s     Dpad:UD  1 min     L / R  Previous / next     B  Stop     Menu  Options",
         20, y + 68, c->w - 20);
   draw_status(c, c->w - 16, 25, 1);
   if (player_paused())
      draw_pause_sign(c, c->w / 2, (c->h - 104) / 2);
}

/* a loading wheel like YouTube's: a thin white arc that spins, its length growing and
 * shrinking, drawn smooth (each pixel shaded by how much of it the ring covers) */
static void draw_spinner(canvas_t *c, int cx, int cy, double now)
{
   const double R = 22, W = 2.2, TAU = 6.2831853;
   /* Like Material's: the whole wheel turns steadily; in each cycle the front runs on
    * ahead (the arc grows), then the tail catches up (it shrinks). Both only ever go
    * forward. Measured in turns of the circle. */
   const double T = 1.6, GROW = 0.82, MIN = 0.06;
   double k = now / T, cyc = k - floor(k);
   double base = fmod(floor(k) * GROW, 1.0) + now * 0.22;   /* where this cycle began, plus the turning */
   double hf = cyc < 0.5 ? 0.5 - 0.5 * cos(cyc * 2 * 3.14159265) : 1;
   double tf = cyc < 0.5 ? 0 : 0.5 - 0.5 * cos((cyc - 0.5) * 2 * 3.14159265);
   double len = MIN + GROW * (hf - tf);                      /* of the circle, 0..1 */
   double head = fmod(base + MIN + GROW * hf, 1.0);          /* where its front is */
   int x, y, r = (int)(R + W + 2);
   for (y = -r; y <= r; y++)
   {
      uint32_t *row;
      if (cy + y < 0 || cy + y >= c->h)
         continue;
      row = c->px + (size_t)(cy + y) * c->stride;
      for (x = -r; x <= r; x++)
      {
         double d = sqrt((double)x * x + (double)y * y), cover = W - fabs(d - R) + 0.5, ang, back;
         unsigned a, k;
         uint32_t p;
         if (cover <= 0 || cx + x < 0 || cx + x >= c->w)
            continue;
         if (cover > 1)
            cover = 1;
         ang = atan2((double)x, (double)-y) / TAU;   /* 0 at the top, going clockwise */
         if (ang < 0)
            ang += 1;
         back = fmod(head - ang + 1, 1.0);           /* how far behind the front */
         if (back > len)
            continue;
         /* soften the two ends */
         if (back * TAU * R < 1)
            cover *= back * TAU * R;
         if ((len - back) * TAU * R < 1)
            cover *= (len - back) * TAU * R;
         a = (unsigned)(cover * 255);
         p = row[cx + x];
         for (k = 0; k < 24; k += 8)
         {
            unsigned ch = (p >> k) & 0xff;
            ch = ch + ((255 - ch) * a) / 255;
            p = (p & ~(0xffu << k)) | (ch << k);
         }
         row[cx + x] = p | 0xff000000u;
      }
   }
}

/* the screen while a video plays: NULL when nothing changed (nothing to show) */
static canvas_t *media_video_screen(double t, int extra)
{
   int fresh = player_video_frame(), on_top, controls, waiting = player_waiting();
   canvas_t *dc;
   if (fresh)
      pl.got_frame = 1;
   controls = !vq.open && (t < pl.overlay_until || player_paused());
   on_top = extra || controls || vq.open || pl.sub_shown || waiting || t < ui.toast_until || t < vol_until;
   if (!fresh && !pl.redraw && !(on_top && t - pl.last_draw > (waiting ? 0.012 : 0.2)) && !(pl.on_top_last && !on_top))
      return NULL;
   dc = gfx_video_begin(pl.got_frame ? player_video() : NULL, pl.vx, pl.vy, pl.vw, pl.vh,
         pl.on_top_last || pl.redraw || !pl.got_frame);
   if (!pl.got_frame && pl.poster)
   {
      /* row by row (it's opaque and already the right size) */
      int y, w = pl.poster->w < dc->w - pl.vx ? pl.poster->w : dc->w - pl.vx;
      for (y = 0; y < pl.poster->h && pl.vy + y < dc->h; y++)
         if (pl.vy + y >= 0 && w > 0 && pl.vx >= 0)
            memcpy(dc->px + (size_t)(pl.vy + y) * dc->stride + pl.vx, pl.poster->px + (size_t)y * pl.poster->w, (size_t)w * 4);
   }
   if (waiting)
      draw_spinner(dc, dc->w / 2, pl.vy + pl.vh / 2 - (controls ? 52 : 0), t);
   pl.sub_shown = draw_subtitles(dc, controls ? 104 : vq.open ? 40 : 0);
   if (controls)
      draw_video_overlay(dc, t);
   if (vq.open)
      draw_quick_menu(dc, t);
   popup_lift = controls ? 62 : vq.open ? 40 : 0;
   draw_toast(dc, t);
   draw_volume(dc, t);
   popup_lift = 0;
   pl.on_top_last = on_top;
   pl.redraw = 0;
   pl.last_draw = t;
   return dc;
}

/* music: the album art, the song, where it's up to */
static void draw_music(canvas_t *c, double now)
{
   const player_info_t *in = player_info();
   double pos = player_position();
   char a[32], b[32], line[300];
   int x = 340, cx = 50, cy = 82, k, n = 0, at = 0;

   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   gfx_draw_text(c, f_big, mb.folder[0] ? mb.folder : "Music", 20, (50 - font_height(f_big)) / 2, C_TEXT, 560);
   for (k = 0; k < mb.n; k++)
      if (mb.e[k].kind == MK_MUSIC)
      {
         n++;
         if (k == pl.index)
            at = n;
      }
   snprintf(line, sizeof(line), "%d / %d", at, n);
   {
      int sw = draw_status(c, c->w - 16, 25, 0);
      gfx_draw_text(c, f_small, line, c->w - 20 - sw - 14 - font_text_width(f_small, line),
            (50 - font_height(f_small)) / 2, C_DIM, 0);
   }

   /* the album art, on a card */
   if (pl.cover)
      gfx_draw_image_smooth(c, pl.cover, cx, cy, COVER_PX, COVER_PX);
   else
   {
      image_t *word = media_word(MK_MUSIC);
      fill_vgradient(c, cx, cy, COVER_PX, COVER_PX, TH(TH_CARD_TOP), TH(TH_CARD_BOT));
      if (word)
      {
         grad = grad_logo;
         grad_edge = C_LET_EDGE;
         draw_gradient_mask(c, word, cx + COVER_PX / 2, cy + (COVER_PX - gradient_height(word)) / 2, 0);
      }
   }
   round_corners(c, cx, cy, COVER_PX, COVER_PX, 20);

   gfx_draw_text(c, f_big, in->title[0] ? in->title : pl.name, x, 100, C_TEXT, c->w - x - 30);
   if (in->artist[0])
      gfx_draw_text(c, f_mid, in->artist, x, 148, C_DIM, c->w - x - 30);
   if (in->album[0])
      gfx_draw_text(c, f_small, in->album, x, 182, C_FAINT, c->w - x - 30);
   draw_progress(c, x, 262, c->w - x - 40, pos, in->duration);
   fmt_time(pos, a, sizeof(a));
   fmt_time(in->duration, b, sizeof(b));
   gfx_draw_text(c, f_small, a, x, 280, C_DIM, 0);
   gfx_draw_text(c, f_small, b, c->w - 40 - font_text_width(f_small, b), 280, C_DIM, 0);
   if (player_paused())
      gfx_draw_text(c, f_title ? f_title : f_small, "PAUSED", x, 222, C_GOLD, 0);

   draw_hints(c, player_paused() ? "A  Play     Dpad:LR  10 s     L / R  Song     B  Back"
         : "A  Pause     Dpad:LR  10 s     L / R  Song     B  Back");
   draw_toast(c, now);
   draw_volume(c, now);
}

/* Select + A on a game: fill in the emulator picker; 1 if there's a choice to show */
static int picker_setup(const entry_t *e, const char *rom, double t)
{
   system_t *s = &systems[e->sys];
   int k;
   memset(&pick, 0, sizeof(pick));
   pick.sys = e->sys;
   snprintf(pick.rom, sizeof(pick.rom), "%s", rom);
   snprintf(pick.label, sizeof(pick.label), "%s", e->label);
   game_core(rom, pick.game, sizeof(pick.game));
   for (k = 0; k < s->ncores; k++)
      if (core_installed(s->cores[k]))
         snprintf(pick.cores[pick.n++], sizeof(pick.cores[0]), "%s", s->cores[k]);
   if (pick.n < 2)
   {
      toast(pick.n ? "Only one emulator for this console" : "No emulator for this console", t);
      return 0;
   }
   {
      /* start on the one it would use now */
      char cur[64];
      pick_core(s, rom, cur, sizeof(cur));
      for (k = 0; k < pick.n; k++)
         if (!strcmp(pick.cores[k], cur))
            pick.sel = k;
   }
   return 1;
}

/* ------------------------------------------------------------- search: everything */
/* What a search finds besides games: the menu's cards (consoles, collections, tools),
 * the Video and Music Players' folders, and the videos and songs in them. Cards first,
 * then games, then media; names that start with what's typed first in each. */
enum { H_CONSOLE, H_COLL, H_TOOL, H_GAME, H_MFOLDER, H_MFILE };
#define MAX_HITS 600
typedef struct
{
   int kind, a, mode;          /* a: visible[] / colls[] / tool / sk.res index; mode: MK_VIDEO / MK_MUSIC */
   char path[300];             /* media: the path inside videos/ or music/ */
   int starts;                 /* the name starts with what's typed */
} hit_t;
static hit_t hits[MAX_HITS];
static int nhits, hit_sel, hit_top;

/* the media files, found once each time the search screen opens */
static struct { char *path; int mode; } *mfiles;
static int nmfiles, mfiles_cap;

static void mfiles_walk(const char *root, const char *rel, int mode, int depth)
{
   char dir[1300];
   DIR *d;
   struct dirent *e;
   snprintf(dir, sizeof(dir), "%s%s%s", root, rel[0] ? "/" : "", rel);
   if (!(d = opendir(dir)))
      return;
   while ((e = readdir(d)))
   {
      char p[1400], r[1100];
      struct stat sb;
      if (e->d_name[0] == '.')
         continue;
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      snprintf(r, sizeof(r), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
      if (stat(p, &sb) < 0)
         continue;
      if (S_ISDIR(sb.st_mode))
      {
         if (depth < 4)
            mfiles_walk(root, r, mode, depth + 1);
         continue;
      }
      if (media_kind(e->d_name) != mode)
         continue;
      if (nmfiles == mfiles_cap)
      {
         int nc = mfiles_cap ? mfiles_cap * 2 : 128;
         void *ne = realloc(mfiles, (size_t)nc * sizeof(*mfiles));
         if (!ne)
            break;
         mfiles = ne;
         mfiles_cap = nc;
      }
      mfiles[nmfiles].path = strdup(r);
      mfiles[nmfiles++].mode = mode;
   }
   closedir(d);
}

static int mfile_cmp(const void *a, const void *b)
{
   return natcmp(*(char *const *)a, *(char *const *)b);
}

static void mfiles_scan(void)
{
   int i;
   for (i = 0; i < nmfiles; i++)
      free(mfiles[i].path);
   nmfiles = 0;
   mfiles_walk(VIDEOS, "", MK_VIDEO, 0);
   mfiles_walk(MUSIC, "", MK_MUSIC, 0);
   qsort(mfiles, nmfiles, sizeof(*mfiles), mfile_cmp);   /* the path is first: "S1 Ep 2" before "S10" */
   mfiles_done = 1;
}

/* a hit's name and what it is, for the lists */
static void hit_text(int i, char *name, size_t nsz, char *kind, size_t ksz)
{
   const hit_t *h = &hits[i];
   const char *slash;
   switch (h->kind)
   {
   case H_CONSOLE:
      snprintf(name, nsz, "%s", systems[visible[h->a]].name);
      snprintf(kind, ksz, "Console");
      break;
   case H_COLL:
      snprintf(name, nsz, "%s", colls[h->a].name);
      snprintf(kind, ksz, "Collection");
      break;
   case H_TOOL:
      snprintf(name, nsz, "%s", tool_names[h->a]);
      snprintf(kind, ksz, "Tool");
      break;
   case H_GAME:
      snprintf(name, nsz, "%s", sk.res.e[h->a].label);
      snprintf(kind, ksz, "%s", systems[sk.res.e[h->a].sys].name);
      break;
   case H_MFOLDER:
      snprintf(name, nsz, "%s", h->path);
      snprintf(kind, ksz, h->mode == MK_VIDEO ? "Video folder" : "Music folder");
      break;
   default:
      slash = strrchr(h->path, '/');
      media_title(slash ? slash + 1 : h->path, name, nsz);
      snprintf(kind, ksz, h->mode == MK_VIDEO ? "Video" : "Song");
      break;
   }
}

static int nhits_cached(void)
{
   return search_q[0] ? nhits : 0;
}

static void hit_add(int kind, int a, int mode, const char *path, const char *name, const char *q)
{
   char n[600];
   if (nhits >= MAX_HITS || !search_match(q, name))
      return;
   search_norm(name, n, sizeof(n));
   hits[nhits].kind = kind;
   hits[nhits].a = a;
   hits[nhits].mode = mode;
   snprintf(hits[nhits].path, sizeof(hits[0].path), "%s", path ? path : "");
   hits[nhits].starts = !strncmp(n, q, strlen(q));
   nhits++;
}

/* names-first within each group (stable: the order inside stays as added) */
static void hits_prefix_first(int from, int to)
{
   hit_t *tmp = malloc((size_t)(to - from + 1) * sizeof(hit_t));
   int pass, i, n = 0;
   if (!tmp)
      return;
   for (pass = 0; pass < 2; pass++)
      for (i = from; i < to; i++)
         if (hits[i].starts == !pass)
            tmp[n++] = hits[i];
   memcpy(hits + from, tmp, (size_t)n * sizeof(hit_t));
   free(tmp);
}

static int hits_build(void)
{
   char q[sizeof(search_q) * 2];
   int i, g0;
   nhits = 0;
   hit_sel = hit_top = 0;
   search_norm(search_q, q, sizeof(q));
   if (!q[0])
      return 0;
   /* the menu's cards */
   for (i = 0; i < nvisible; i++)
      hit_add(H_CONSOLE, i, 0, NULL, systems[visible[i]].name, q);
   for (i = 0; i < ncolls; i++)
      if (i != COLL_SEARCH)
         hit_add(H_COLL, i, 0, NULL, colls[i].name, q);
   for (i = 0; i < NTOOLS; i++)
      hit_add(H_TOOL, i, 0, NULL, tool_names[i], q);
   /* games (already in order) */
   g0 = nhits;
   for (i = 0; i < sk.res.n && nhits < MAX_HITS; i++)
   {
      hits[nhits].kind = H_GAME;
      hits[nhits].a = i;
      hits[nhits].path[0] = 0;
      {
         char n[600];
         search_norm(sk.res.e[i].label, n, sizeof(n));
         hits[nhits].starts = !strncmp(n, q, strlen(q));
      }
      nhits++;
   }
   (void)g0;
   /* the players' folders, then their videos and songs */
   {
      int m0 = nhits, mode;
      for (mode = MK_VIDEO; mode <= MK_MUSIC; mode++)
      {
         const char *root = mode == MK_VIDEO ? VIDEOS : MUSIC;
         DIR *d = opendir(root);
         struct dirent *e;
         while (d && (e = readdir(d)))
         {
            char p[1400];
            struct stat sb;
            if (e->d_name[0] == '.')
               continue;
            snprintf(p, sizeof(p), "%s/%s", root, e->d_name);
            if (stat(p, &sb) == 0 && S_ISDIR(sb.st_mode))
               hit_add(H_MFOLDER, 0, mode, e->d_name, e->d_name, q);
         }
         if (d)
            closedir(d);
      }
      /* only the folders: every episode of a show would bury the games */
      (void)m0;
   }
   /* names that start with what's typed first, whatever they are (cards, games, then
    * media inside each half) */
   hits_prefix_first(0, nhits);
   return nhits;
}

/* open a hit: 1 = a game or RetroArch started, 2 = show the controls, 3 = a game can't
 * start (the error prompt: err_* says why), 4 = a launch warning (ask: play anyway?) */
static int hit_open(int i, double t)
{
   const hit_t *h = &hits[i];
   switch (h->kind)
   {
   case H_CONSOLE:
      ui.row_pos += ROW_CONS - cur_row();
      ui.row_anim = ui.row_pos;
      row_set(ROW_CONS, h->a);
      ui.view = 1;
      ui.list_all = 0;
      ui.folder[0] = 0;
      ui.top = ui.sel = 0;
      ui.want_entry[0] = 0;
      list_free(&ui.list);
      reload_list();
      ui.list_check = t;
      return 0;
   case H_COLL:
      open_collection(h->a, t);   /* an empty one just says so, and the screen stays */
      return 0;
   case H_TOOL:
      ui.row_pos += ROW_TOOLS - cur_row();
      ui.row_anim = ui.row_pos;
      row_set(ROW_TOOLS, h->a);
      ui.view = 0;
      if (h->a == TOOL_SETTINGS)
      {
         settings_open();
         ui.view = 2;
      }
      else if (h->a == TOOL_CONTROLS)
         return 2;
      else if (h->a == TOOL_VIDEO || h->a == TOOL_MUSIC)
         media_open(h->a == TOOL_VIDEO ? MK_VIDEO : MK_MUSIC, t);
      else if (h->a == TOOL_PICTURES)
         gal_open();
      else if (h->a == TOOL_RETROARCH && launch_retroarch() == 0)
         return 1;
      return 0;
   case H_GAME:
   {
      const entry_t *e = &sk.res.e[h->a];
      char rom[1300];
      int r;
      snprintf(rom, sizeof(rom), "%s/%s/%s", ROMS, systems[e->sys].id, e->name);
      r = launch(&systems[e->sys], rom);
      return r == 0 ? 1 : r == -2 ? 3 : r == -3 ? 4 : 0;
   }
   case H_MFOLDER:
   {
      int k;
      media_open(h->mode, t);
      for (k = 0; k < mf.n; k++)
         if (!strcmp(mf.name[k], h->path))
         {
            row_set(ROW_MEDIA, k);
            ui.anim_[ROW_MEDIA] = ui.pos_[ROW_MEDIA];
            media_open_card(k, t);
         }
      return 0;
   }
   default:
   {
      /* a video or song: its folder's list, then play it */
      char dir[300], *slash;
      const char *file;
      int k;
      snprintf(dir, sizeof(dir), "%s", h->path);
      slash = strrchr(dir, '/');
      file = slash ? h->path + (slash - dir) + 1 : h->path;
      mb.mode = h->mode;
      media_folders();
      if (slash)
      {
         char *top = strchr(dir, '/');
         *slash = 0;
         snprintf(mb.base, sizeof(mb.base), "%.*s", (int)(top - dir), dir);
         snprintf(mb.folder, sizeof(mb.folder), "%s", dir);
      }
      else
         mb.base[0] = mb.folder[0] = 0;
      snprintf(mb.want, sizeof(mb.want), "%s", file);
      mb.sel = mb.top = 0;
      mb.sig = media_scan(0);
      mb.check = t;
      ui.view = 3;
      for (k = 0; k < mb.n; k++)
         if (!strcmp(mb.e[k].name, file))
         {
            media_launch(k);
            break;
         }
      return 0;
   }
   }
}

static void draw_hits(canvas_t *c, double now)
{
   char title[200], name[300], kind[64];
   int i;

   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   snprintf(title, sizeof(title), "Search: %s", search_q);
   gfx_draw_text(c, f_big, title, 20, (50 - font_height(f_big)) / 2, C_TEXT, 560);
   snprintf(name, sizeof(name), "%d / %d", nhits ? hit_sel + 1 : 0, nhits);
   {
      int sw = draw_status(c, c->w - 16, 25, 0);
      gfx_draw_text(c, f_small, name, c->w - 20 - sw - 14 - font_text_width(f_small, name),
            (50 - font_height(f_small)) / 2, C_DIM, 0);
   }
   if (hit_sel < hit_top)
      hit_top = hit_sel;
   if (hit_sel >= hit_top + LIST_ROWS)
      hit_top = hit_sel - LIST_ROWS + 1;
   for (i = 0; i < LIST_ROWS && hit_top + i < nhits; i++)
   {
      int y = LIST_Y + i * ROW_H, sel = hit_top + i == hit_sel, kw;
      hit_text(hit_top + i, name, sizeof(name), kind, sizeof(kind));
      if (sel)
      {
         gfx_fill_rect(c, 12, y, 518, ROW_H, C_SEL);
         gfx_fill_rect(c, 12, y, 4, ROW_H, C_ACCENT);
      }
      if (hits[hit_top + i].kind == H_GAME)
      {
         const entry_t *e = &sk.res.e[hits[hit_top + i].a];
         char key[1300];
         snprintf(key, sizeof(key), "%s/%s", systems[e->sys].id, e->name);
         if (is_fav(key))
            draw_star(c, 508, y + (ROW_H - 12) / 2, C_GOLD);
      }
      kw = font_text_width(f_small, kind);
      gfx_draw_text(c, f_small, kind, 500 - kw, y + (ROW_H - font_height(f_small)) / 2, C_FAINT, 0);
      gfx_draw_text(c, f_mid, name, 30, y + (ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 460 - kw);
   }
   if (nhits > LIST_ROWS)
   {
      int track = LIST_ROWS * ROW_H, bar = track * LIST_ROWS / nhits;
      gfx_fill_rect(c, 534, LIST_Y, 3, track, C_FAINT);
      gfx_fill_rect(c, 534, LIST_Y + (track - bar) * hit_top / (nhits - LIST_ROWS), 3, bar, C_ACCENT);
   }

   /* details panel: the game's thumbnail or the video's picture, otherwise its kind */
   gfx_fill_rect(c, 552, LIST_Y, 290, 360, C_PANEL);
   if (nhits)
   {
      const hit_t *h = &hits[hit_sel];
      const image_t *pic = NULL;
      int shown = 0;
      hit_text(hit_sel, name, sizeof(name), kind, sizeof(kind));
      gfx_fill_rect(c, 567, LIST_Y + 15, 260, 195, TH(TH_PICBOX));
      if (h->kind == H_GAME && !(pic = large_for(&sk.res.e[h->a])))
         pic = thumb_for(&sk.res.e[h->a]);
      else if (h->kind == H_MFILE && h->mode == MK_VIDEO)
      {
         char path[1400];
         int st;
         snprintf(path, sizeof(path), "%s/%s", VIDEOS, h->path);
         pic = vthumb_get(path, now, 1, &st);
      }
      if (pic)
      {
         int pw = pic->w, ph = pic->h;
         double sc = 260.0 / pw < 195.0 / ph ? 260.0 / pw : 195.0 / ph;
         if (sc > 3)
            sc = 3;
         pw = (int)(pw * sc);
         ph = (int)(ph * sc);
         gfx_draw_image_smooth(c, pic, 697 - pw / 2, LIST_Y + 15 + (195 - ph) / 2, pw, ph);
         shown = 1;
      }
      if (!shown && h->kind == H_GAME)
         draw_no_art_hint(c, sk.res.e[h->a].sys);
      else if (!shown)
      {
         image_t *word = gradient_mask(kind, 230, 80);
         if (word)
         {
            int gray = h->kind != H_GAME && h->kind != H_MFILE;
            grad = gray ? grad_gray : grad_logo;
            grad_edge = gray ? 0xff3b3946 : C_LET_EDGE;
            draw_gradient_mask(c, word, 697, LIST_Y + 15 + (195 - gradient_height(word)) / 2, 0);
            image_free(word);
         }
      }
      gfx_draw_text(c, f_mid, name, 567, LIST_Y + 225, C_TEXT, 260);
      gfx_draw_text(c, f_small, kind, 567, LIST_Y + 262, C_DIM, 260);
   }
   draw_hints(c, nhits && hits[hit_sel].kind == H_GAME ? "A  Play     B  Back to search     Y  Favorite     Select + A  Emulator"
         : "A  Open     B  Back to search");
   draw_toast(c, now);
   draw_volume(c, now);
}

/* buttons on the results: returns as hit_open() does */
static int hits_key(int code, double t)
{
   if (code == KEY_UP && nhits)
      hit_sel = (hit_sel + nhits - 1) % nhits;
   else if (code == KEY_DOWN && nhits)
      hit_sel = (hit_sel + 1) % nhits;
   else if (code == BTN_TL && nhits)
      hit_sel = hit_sel > LIST_ROWS ? hit_sel - LIST_ROWS : 0;
   else if (code == BTN_TR && nhits)
      hit_sel = hit_sel + LIST_ROWS < nhits ? hit_sel + LIST_ROWS : nhits - 1;
   else if (code == BTN_EAST)
      ui.view = 6;
   else if (code == BTN_WEST && nhits && hits[hit_sel].kind == H_GAME)
   {
      const entry_t *e = &sk.res.e[hits[hit_sel].a];
      char key[1300];
      int now_fav;
      snprintf(key, sizeof(key), "%s/%s", systems[e->sys].id, e->name);
      now_fav = toggle_fav(key);
      toast(now_fav ? "Added to Favorites" : "Removed from Favorites", t);
      load_collections();
   }
   else if (code == BTN_SOUTH && nhits)
      return hit_open(hit_sel, t);
   return 0;
}

/* ------------------------------------------------------------- theme editor */
#define THEME_FILE SDCARD "/g36/theme.cfg"

/* colours as hue (0-360), saturation and brightness (0-100) */
static void rgb_hsv(uint32_t c, int *h, int *s, int *v)
{
   int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
   int mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;
   double hh = 0;
   if (d)
   {
      if (mx == r)
         hh = 60.0 * fmod((double)(g - b) / d, 6);
      else if (mx == g)
         hh = 60.0 * ((double)(b - r) / d + 2);
      else
         hh = 60.0 * ((double)(r - g) / d + 4);
   }
   if (hh < 0)
      hh += 360;
   *h = (int)(hh + 0.5) % 360;
   *s = mx ? (d * 100 + mx / 2) / mx : 0;
   *v = (mx * 100 + 127) / 255;
}

static uint32_t hsv_rgb(int h, int s, int v)
{
   double C = v / 100.0 * s / 100.0, X = C * (1 - fabs(fmod(h / 60.0, 2) - 1)), m = v / 100.0 - C, r = 0, g = 0, b = 0;
   if (h < 60) { r = C; g = X; }
   else if (h < 120) { r = X; g = C; }
   else if (h < 180) { g = C; b = X; }
   else if (h < 240) { g = X; b = C; }
   else if (h < 300) { r = X; b = C; }
   else { r = C; b = X; }
   return 0xff000000 | (uint32_t)((r + m) * 255 + 0.5) << 16 | (uint32_t)((g + m) * 255 + 0.5) << 8 | (uint32_t)((b + m) * 255 + 0.5);
}

/* ready-made themes, each colour picked by hand, in the order of the TH_ list (0 = the
 * default). The lettering only follows them with "Lettering: Match theme". */
static const struct { const char *name; uint32_t c[TH_COUNT]; } presets[] = {
   { "Midnight", { 0 } },
   { "Ocean", { 0x0e1a2b, 0x07101c, 0x1f5f8a, 0x1e3550, 0x102033, 0x2e4d6e,
      0x7fe3ff, 0x4fc3f7, 0x3b8fe0, 0x2f5fd0, 0x5a3fd0, 0x0d2f6b,
      0x4fc3f7, 0xffffff, 0x93aec4, 0x557089, 0x1f4a6e, 0x132638, 0x1e3550, 0x1f4060,
      0x08121e, 0x112236, 0x2a4664, 0x1d3a58, 0x4a6f94, 0x2a5078 } },
   { "Forest", { 0x13221a, 0x09130d, 0x2e6a3c, 0x23402c, 0x132619, 0x35583f,
      0xd4ff7a, 0x9be35a, 0x4fc36a, 0x2a9a6a, 0x1f6f5a, 0x0f3a22,
      0x6fdc8c, 0xffffff, 0x9fbfa8, 0x5c7a64, 0x2a5236, 0x17291d, 0x23402c, 0x24432e,
      0x0a140e, 0x14261a, 0x2f4f38, 0x23402c, 0x4f7a5a, 0x2f5a3a } },
   { "Crimson", { 0x2a0a0e, 0x140306, 0x6e0f1a, 0x4a121b, 0x26070c, 0x6a1c28,
      0xff8a8a, 0xff3b4f, 0xd81e3a, 0xa3122a, 0x6e0a1a, 0x3a0408,
      0xff3b4f, 0xffffff, 0xc79aa0, 0x7a4a52, 0x6a1622, 0x2e0a10, 0x4a121b, 0x4e1520,
      0x120305, 0x2a080d, 0x5a1822, 0x4a121b, 0x8a3040, 0x6a1a26 } },
   { "Gold", { 0x2a2008, 0x140f03, 0x7a5a0a, 0x4a3a12, 0x261c07, 0x7a6022,
      0xfff6b0, 0xffe45c, 0xffd23f, 0xffb300, 0xff8f00, 0x6a3a00,
      0xffd23f, 0xffffff, 0xd9c48f, 0x8a7444, 0x6a5214, 0x2e240a, 0x4a3a12, 0x5a4614,
      0x120d03, 0x2a2008, 0x6a5218, 0x4a3a12, 0x9a7e30, 0x6a5418 } },
   { "Mono", { 0x1e1e22, 0x101012, 0x4a4a52, 0x3a3a40, 0x1e1e22, 0x4c4c54,
      0xf4f4f8, 0xd2d2dc, 0xb4b4c0, 0x93939f, 0x74747f, 0x3b3946,
      0xe0e0e8, 0xffffff, 0xa8a8b0, 0x6a6a72, 0x44444c, 0x26262b, 0x3a3a40, 0x404048,
      0x0e0e10, 0x22222a, 0x44444c, 0x3a3a42, 0x6a6a74, 0x50505a } },
};
#define NPRESETS ((int)(sizeof(presets) / sizeof(presets[0])))
static char theme_name[32] = "Midnight";
static int theme_lettering;   /* 0 default, 1.. that theme's lettering, NPRESETS: follow the theme */

static int is_lettering(int i)
{
   return i >= TH_LET1 && i <= TH_LET_EDGE;
}

/* the light cards' two colours from a dark card colour and the accent: the card colour with a little accent, mixed towards
 * white until it reaches the brightness where dark and white logos are both readable (see tone_card) */
static void derive_light_cards(uint32_t card_top, uint32_t accent, uint32_t *top, uint32_t *bot)
{
   uint32_t tint = mix_colour(card_top, accent, 90);
   *top = tone_card(tint, 0.27f);
   *bot = tone_card(tint, 0.21f);
}

static uint32_t preset_colour(int p, int i)
{
   if (i == TH_CARDL_TOP || i == TH_CARDL_BOT)
   {
      uint32_t t, b, ct = p == 0 ? th[TH_CARD_TOP].def : 0xff000000 | presets[p].c[TH_CARD_TOP],
               ac = p == 0 ? th[TH_ACCENT].def : 0xff000000 | presets[p].c[TH_ACCENT];
      derive_light_cards(ct, ac, &t, &b);
      return i == TH_CARDL_TOP ? t : b;
   }
   return p == 0 ? th[i].def : 0xff000000 | presets[p].c[i];
}

/* the theme's colours put to use: the lettering's stops; drawn things redrawn */
static void theme_changed(void)
{
   int i, r, k;
   for (i = 0; i < 5; i++)
      grad_logo[i].c = TH(TH_LET1 + i) & 0xffffff;
   preview_defer = 1;
   /* the background (half a second of work to build, both versions) only depends on these colours */
   {
      static uint32_t sig[6];
      uint32_t now_sig[6] = { TH(TH_BG_TOP), TH(TH_BG_BOT), TH(TH_BG_LIGHT), TH(TH_ACCENT), (uint32_t)theme_skyline, (uint32_t)theme_sky_style };
      if (memcmp(sig, now_sig, sizeof(sig)))
      {
         memcpy(sig, now_sig, sizeof(sig));
         free(ctl_bgbuf);
         ctl_bgbuf = NULL;
         free(bg);
         bg = NULL;
         free(bg_stage);
         bg_stage = NULL;
         bg_cur = NULL;
      }
   }
   for (r = 0; r <= ROW_MEDIA; r++)
      for (k = 0; k < MAX_SYSTEMS; k++)
         card_cache[r][k].key[0] = 0;   /* each card is drawn again when next shown */
   for (i = 0; i < 3; i++)
   {
      image_free(mword_cache[i]);
      mword_cache[i] = NULL;
   }
}

static void theme_save(void)
{
   FILE *f = fopen(THEME_FILE ".tmp", "w");
   int i;
   if (!f)
      return;
   fprintf(f, "# G36 menu colours (RRGGBB). Delete this file for the default theme.\nname = %s\nlettering = %s\nstars = %d\nglint = %s\npill_glass = %s\nbar_glass = %s\ngames_carousel = %s\nskyline = %s\nskyline_style = %s\n",
         theme_name, theme_lettering >= NPRESETS ? "theme" : presets[theme_lettering].name, theme_stars,
         theme_glint ? "on" : "off", theme_glass_pill ? "on" : "off", theme_glass_bar ? "on" : "off",
         theme_games_carousel ? "on" : "off", theme_skyline ? "on" : "off", theme_sky_style == 2 ? "forest" : theme_sky_style ? "mountains" : "city");
   for (i = 0; i < TH_COUNT; i++)
      fprintf(f, "%s = %06X\n", th[i].key, th[i].v & 0xffffff);
   if (!ferror(f) && fclose(f) == 0)
      rename(THEME_FILE ".tmp", THEME_FILE);
   else
      unlink(THEME_FILE ".tmp");
}

static void theme_load(void)
{
   char line[200];
   FILE *f = fopen(THEME_FILE, "r");
   int i, got_light = 0;
   th[TH_CARDL_TOP].v = th[TH_CARDL_TOP].def = preset_colour(0, TH_CARDL_TOP);
   th[TH_CARDL_BOT].v = th[TH_CARDL_BOT].def = preset_colour(0, TH_CARDL_BOT);
   if (f)
   {
      while (fgets(line, sizeof(line), f))
      {
         char *eq = strchr(line, '='), *k = line, *v;
         if (line[0] == '#' || !eq)
            continue;
         *eq = 0;
         k = trim(k);
         v = trim(eq + 1);
         if (!strcmp(k, "name"))
            snprintf(theme_name, sizeof(theme_name), "%s", strcmp(v, "Default") ? v : "Midnight");
         if (!strcmp(k, "stars"))
            theme_stars = !strcmp(v, "off") ? 0 : !strcmp(v, "on") ? 50 : atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
         if (!strcmp(k, "pill_glass"))
            theme_glass_pill = strcmp(v, "off") != 0;
         if (!strcmp(k, "bar_glass"))
            theme_glass_bar = strcmp(v, "off") != 0;
         if (!strcmp(k, "glint"))
            theme_glint = strcmp(v, "off") != 0;
         if (!strcmp(k, "games_carousel"))
            theme_games_carousel = strcmp(v, "off") != 0;
         if (!strcmp(k, "skyline"))
            theme_skyline = strcmp(v, "off") != 0;
         if (!strcmp(k, "skyline_style"))
            theme_sky_style = !strcmp(v, "forest") ? 2 : !strcmp(v, "city") ? 0 : 1;
         if (!strcmp(k, "lettering"))
         {
            int q;
            theme_lettering = !strcmp(v, "theme") ? NPRESETS : 0;
            for (q = 1; q < NPRESETS; q++)
               if (!strcmp(v, presets[q].name))
                  theme_lettering = q;
         }
         for (i = 0; i < TH_COUNT; i++)
            if (!strcmp(k, th[i].key) && strlen(v) >= 6)
            {
               th[i].v = 0xff000000 | (uint32_t)strtoul(v, NULL, 16);
               if (i == TH_CARDL_TOP)
                  got_light = 1;
            }
      }
      fclose(f);
      if (!got_light)   /* a file from before the light cards had their own colours: what they looked like then */
         derive_light_cards(th[TH_CARD_TOP].v, th[TH_ACCENT].v, &th[TH_CARDL_TOP].v, &th[TH_CARDL_BOT].v);
   }
   theme_changed();
}

static void theme_preset(int p)
{
   int i;
   for (i = 0; i < TH_COUNT; i++)
      th[i].v = is_lettering(i) && theme_lettering < NPRESETS ? preset_colour(theme_lettering, i) : preset_colour(p, i);
   snprintf(theme_name, sizeof(theme_name), "%s", presets[p].name);
   theme_changed();
   switch_prewarm();   /* (the new accent's switch pictures, now rather than on the first slide) */
   theme_save();
}

/* the editor: rows are the preset, Reset, then the colours under their group names */
enum { TR_SKYLINE = -10, TR_SKYSTYLE = -9, TR_CAROUSEL = -8, TR_BARGLASS = -7, TR_PILLGLASS = -6, TR_GLINT = -5, TR_STARS = -4, TR_LETTERING = -3, TR_PRESET = -2, TR_RESET = -1 };
static struct
{
   int sel, top;          /* index into rows */
   int rows[TH_COUNT + 7 + 10], nrows;   /* TR_PRESET, TR_RESET, colour index, or 1000 + group */
   int picking, field;    /* the colour picker is open; 0 hue, 1 saturation, 2 brightness */
   int h, s, v;
   uint32_t before;
   image_t *word;         /* the preview's lettering */
} te;

static void theme_rows(void)
{
   int i;
   te.nrows = 0;
   te.rows[te.nrows++] = TR_PRESET;
   te.rows[te.nrows++] = TR_LETTERING;
   te.rows[te.nrows++] = TR_STARS;
   te.rows[te.nrows++] = TR_GLINT;
   te.rows[te.nrows++] = TR_PILLGLASS;
   te.rows[te.nrows++] = TR_BARGLASS;
   te.rows[te.nrows++] = TR_CAROUSEL;
   te.rows[te.nrows++] = TR_SKYLINE;
   te.rows[te.nrows++] = TR_SKYSTYLE;
   te.rows[te.nrows++] = TR_RESET;
   {
      /* the colours, in the order shown: the light cards' two follow the dark cards', then the rest; a heading where the group changes */
      int ord[TH_COUNT], no = 0, q;
      const char *last = "";
      for (i = 0; i <= TH_CARD_LINE; i++)
         ord[no++] = i;
      ord[no++] = TH_CARDL_TOP;
      ord[no++] = TH_CARDL_BOT;
      for (i = TH_CARD_LINE + 1; i < TH_CARDL_TOP; i++)
         ord[no++] = i;
      for (q = 0; q < no; q++)
      {
         i = ord[q];
         if (th[i].group && strcmp(th[i].group, last))
         {
            te.rows[te.nrows++] = 1000 + i;   /* a heading */
            last = th[i].group;
         }
         te.rows[te.nrows++] = i;
      }
   }
}

/* ---- Pictures: everything the Image Generator has made (imagegen/pictures/), as a grid,
 * then one picture at a time with its prompt. Newest first (the names are dates). */
#define PICS_DIR    SDCARD "/imagegen/pictures"
#define PICS_NOTES  PICS_DIR "/prompts.txt"
#define GAL_COLS    5
#define GAL_ROWS    2
#define GAL_PER     (GAL_COLS * GAL_ROWS)
#define GAL_TH      140
static struct
{
   char (*name)[80];
   int n, cap;
   int sel, big;                /* big: one picture on its own */
   image_t *th[GAL_PER];        /* the page showing */
   int th_page;
   image_t *full;               /* the big one */
   int full_idx;
   int info_idx;                /* whose prompt, seed and steps are below */
   char prompt[300];
   unsigned seed;
   int steps;
} gal = { NULL, 0, 0, 0, 0, { NULL }, -1, NULL, -1, -1, "", 0, 0 };

static int gal_cmp(const void *a, const void *b)
{
   return strcmp((const char *)b, (const char *)a);   /* newest (largest name) first */
}

static void gal_free_images(void)
{
   int i;
   for (i = 0; i < GAL_PER; i++)
   {
      image_free(gal.th[i]);
      gal.th[i] = NULL;
   }
   gal.th_page = -1;
   image_free(gal.full);
   gal.full = NULL;
   gal.full_idx = -1;
}

static void gal_scan(void)
{
   DIR *d = opendir(PICS_DIR);
   struct dirent *de;
   gal.n = 0;
   if (!d)
      return;
   while ((de = readdir(d)))
   {
      size_t l = strlen(de->d_name);
      if (de->d_name[0] == '.' || l < 5 || strcasecmp(de->d_name + l - 4, ".png") || l >= sizeof(gal.name[0]))
         continue;
      if (gal.n >= gal.cap)
      {
         int cap = gal.cap ? gal.cap * 2 : 64;
         char (*bigger)[80] = realloc(gal.name, (size_t)cap * sizeof(*bigger));
         if (!bigger)
            break;
         gal.name = bigger;
         gal.cap = cap;
      }
      snprintf(gal.name[gal.n++], sizeof(gal.name[0]), "%s", de->d_name);
   }
   closedir(d);
   if (gal.n > 1)
      qsort(gal.name, (size_t)gal.n, sizeof(gal.name[0]), gal_cmp);
}

static void gal_open(void)
{
   gal_free_images();
   gal_scan();
   gal.sel = 0;
   gal.big = 0;
   gal.info_idx = -1;
   ui.view = 9;
}

/* the prompt, seed and steps of the selected picture, from prompts.txt */
static void gal_info(void)
{
   FILE *f;
   char line[600];
   if (gal.info_idx == gal.sel)
      return;
   gal.info_idx = gal.sel;
   gal.prompt[0] = 0;
   gal.seed = 0;
   gal.steps = 0;
   if (gal.sel >= gal.n || !(f = fopen(PICS_NOTES, "r")))
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *t1, *t2, *t3;
      line[strcspn(line, "\r\n")] = 0;
      if (!(t1 = strchr(line, '\t')))
         continue;
      *t1++ = 0;
      if (strcmp(line, gal.name[gal.sel]))
         continue;
      t2 = strchr(t1, '\t');
      if (t2)
         *t2++ = 0;
      snprintf(gal.prompt, sizeof(gal.prompt), "%s", t1);
      if (t2 && (t3 = strchr(t2, '\t')))
      {
         *t3++ = 0;
         sscanf(t2, "seed %u", &gal.seed);
         sscanf(t3, "%d steps", &gal.steps);
      }
      break;
   }
   fclose(f);
}

static image_t *gal_load(int i, int w)
{
   char path[400];
   image_t *img, *out;
   snprintf(path, sizeof(path), "%s/%s", PICS_DIR, gal.name[i]);
   if (!(img = image_load(path)))
      return NULL;
   out = image_scaled(img, w, w);
   image_free(img);
   return out;
}

static void gal_remove_note(const char *name)
{
   FILE *f = fopen(PICS_NOTES, "r"), *o;
   char line[600], keep[600];
   size_t nl = strlen(name);
   if (!f)
      return;
   if (!(o = fopen(PICS_NOTES ".tmp", "w")))
   {
      fclose(f);
      return;
   }
   while (fgets(line, sizeof(line), f))
   {
      snprintf(keep, sizeof(keep), "%s", line);
      if (!(strncmp(line, name, nl) == 0 && line[nl] == '\t'))
         fputs(keep, o);
   }
   fclose(f);
   fclose(o);
   rename(PICS_NOTES ".tmp", PICS_NOTES);
}

static void gal_delete(double t)
{
   char path[400];
   int i;
   if (gal.sel >= gal.n)
      return;
   snprintf(path, sizeof(path), "%s/%s", PICS_DIR, gal.name[gal.sel]);
   unlink(path);
   gal_remove_note(gal.name[gal.sel]);
   for (i = gal.sel; i + 1 < gal.n; i++)
      memcpy(gal.name[i], gal.name[i + 1], sizeof(gal.name[0]));
   gal.n--;
   if (gal.sel >= gal.n)
      gal.sel = gal.n ? gal.n - 1 : 0;
   if (!gal.n)
      gal.big = 0;
   gal_free_images();
   gal.info_idx = -1;
   toast("Picture deleted", t);
}

/* a button; 1 when the delete prompt should open */
static int gal_key(int code, double t)
{
   int pages;
   (void)t;
   if (code == BTN_EAST)
   {
      if (gal.big)
         gal.big = 0;
      else
      {
         gal_free_images();
         ui.view = 0;
      }
      return 0;
   }
   if (!gal.n)
      return 0;
   if (code == BTN_NORTH)
      return 1;
   if (gal.big)
   {
      if (code == KEY_LEFT || code == BTN_TL)
         gal.sel = (gal.sel + gal.n - 1) % gal.n;
      else if (code == KEY_RIGHT || code == BTN_TR)
         gal.sel = (gal.sel + 1) % gal.n;
      return 0;
   }
   pages = (gal.n + GAL_PER - 1) / GAL_PER;
   if (code == KEY_LEFT)
      gal.sel = (gal.sel + gal.n - 1) % gal.n;
   else if (code == KEY_RIGHT)
      gal.sel = (gal.sel + 1) % gal.n;
   else if (code == KEY_UP && gal.sel >= GAL_COLS)
      gal.sel -= GAL_COLS;
   else if (code == KEY_DOWN)
      gal.sel = gal.sel + GAL_COLS < gal.n ? gal.sel + GAL_COLS : gal.n - 1;
   else if (code == BTN_TL)
      gal.sel = gal.sel / GAL_PER > 0 ? (gal.sel / GAL_PER - 1) * GAL_PER : (pages - 1) * GAL_PER;
   else if (code == BTN_TR)
      gal.sel = gal.sel / GAL_PER + 1 < pages ? (gal.sel / GAL_PER + 1) * GAL_PER : 0;
   else if (code == BTN_SOUTH)
      gal.big = 1;
   return 0;
}

static void draw_gallery(canvas_t *c, double now)
{
   char line[96];
   (void)now;
   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   gfx_draw_text(c, f_big, "Pictures", 20, (50 - font_height(f_big)) / 2, C_TEXT, 400);
   if (gal.n)
   {
      int sw = draw_status(c, c->w - 16, 25, 0);
      snprintf(line, sizeof(line), "%d / %d", gal.sel + 1, gal.n);
      gfx_draw_text(c, f_small, line, c->w - 20 - sw - 14 - font_text_width(f_small, line),
            (50 - font_height(f_small)) / 2, C_DIM, 0);
   }
   else
      draw_status(c, c->w - 16, 25, 0);

   if (!gal.n)
   {
      draw_centered(c, f_mid, "No pictures yet.", c->w / 2, 190, C_DIM);
      draw_centered(c, f_small, "Make some with the Image Generator on the Tools row.", c->w / 2, 226, C_FAINT);
      draw_hints(c, "B  Back");
      return;
   }
   if (gal.sel >= gal.n)
      gal.sel = gal.n - 1;
   gal_info();

   if (gal.big)
   {
      int x = 30, y = 58, side = 380, tx = x + side + 34, tw = c->w - tx - 30, ty = 66, i, lh = font_height(f_mid) + 3;
      char word[300], cur[300];
      const char *p;
      if (gal.full_idx != gal.sel)
      {
         image_free(gal.full);
         gal.full = gal_load(gal.sel, 256);
         gal.full_idx = gal.sel;
      }
      fill_round_rect(c, x - 3, y - 3, side + 6, side + 6, 10, C_ACCENT);
      if (gal.full)
         gfx_draw_image_smooth(c, gal.full, x, y, side, side);
      round_corners(c, x, y, side, side, 8);
      /* the prompt, wrapped */
      cur[0] = 0;
      i = 0;
      for (p = gal.prompt[0] ? gal.prompt : "(no prompt saved)"; ; )
      {
         size_t wl = strcspn(p, " ");
         char test[300];
         snprintf(word, sizeof(word), "%.*s", (int)(wl < sizeof(word) - 1 ? wl : sizeof(word) - 1), p);
         snprintf(test, sizeof(test), "%s%s%s", cur, cur[0] ? " " : "", word);
         if (cur[0] && font_text_width(f_mid, test) > tw)
         {
            gfx_draw_text(c, f_mid, cur, tx, ty + i * lh, C_TEXT, 0);
            i++;
            snprintf(cur, sizeof(cur), "%s", word);
         }
         else
            snprintf(cur, sizeof(cur), "%s", test);
         p += wl;
         if (!*p)
            break;
         p++;
         if (i >= 6)
            break;
      }
      gfx_draw_text(c, f_mid, cur, tx, ty + i * lh, C_TEXT, tw);
      ty += (i + 1) * lh + 16;
      if (gal.steps)
      {
         snprintf(line, sizeof(line), "%d steps, seed %u", gal.steps, gal.seed);
         gfx_draw_text(c, f_small, line, tx, ty, C_DIM, tw);
         ty += 26;
      }
      gfx_draw_text(c, f_small, gal.name[gal.sel], tx, ty, C_FAINT, tw);
      draw_hints(c, "Dpad:LR  Previous / next     X  Delete     B  Back");
      return;
   }

   /* the grid: the page the selection is on */
   {
      int page = gal.sel / GAL_PER, i, first = page * GAL_PER;
      if (gal.th_page != page)
      {
         for (i = 0; i < GAL_PER; i++)
         {
            image_free(gal.th[i]);
            gal.th[i] = first + i < gal.n ? gal_load(first + i, GAL_TH) : NULL;
         }
         gal.th_page = page;
      }
      for (i = 0; i < GAL_PER && first + i < gal.n; i++)
      {
         int col = i % GAL_COLS, row = i / GAL_COLS, x = 29 + col * (GAL_TH + 24), y = 64 + row * 170, sel = first + i == gal.sel;
         fill_round_rect(c, x - 4, y - 4, GAL_TH + 8, GAL_TH + 8, 10, sel ? C_ACCENT : TH(TH_CARD_LINE));
         if (gal.th[i])
            gfx_draw_image(c, gal.th[i], x, y);
         else
            fill_vgradient(c, x, y, GAL_TH, GAL_TH, TH(TH_CARD_TOP), TH(TH_CARD_BOT));
         round_corners(c, x, y, GAL_TH, GAL_TH, 7);
      }
      if (gal.n > GAL_PER)
      {
         /* which page */
         int pages = (gal.n + GAL_PER - 1) / GAL_PER;
         snprintf(line, sizeof(line), "Page %d of %d", page + 1, pages);
         gfx_draw_text(c, f_small, line, c->w - 29 - font_text_width(f_small, line), 52 + 2 * 170 + 8, C_FAINT, 0);
      }
      /* the selected one's prompt */
      if (gal.prompt[0])
         gfx_draw_text(c, f_small, gal.prompt, 29, 52 + 2 * 170 + 8, C_DIM, c->w - 220);
   }
   draw_hints(c, "A  View     Dpad  Navigation     L / R  Page     X  Delete     B  Back");
}

static void theme_open(void)
{
   theme_rows();
   te.sel = te.top = 0;
   te.picking = 0;
   switch_prewarm();
   ui.view = 8;
}

static int theme_preset_index(void)
{
   int i;
   for (i = 0; i < NPRESETS; i++)
      if (!strcmp(presets[i].name, theme_name))
         return i;
   return -1;
}

static void theme_word_drop(void)
{
   image_free(te.word);
   te.word = NULL;
}

/* buttons in the editor; returns 1 to ask about resetting */
static int theme_key(int code)
{
   if (te.picking)
   {
      int *f = te.field == 0 ? &te.h : te.field == 1 ? &te.s : &te.v, mx = te.field == 0 ? 359 : 100;
      int step = te.field == 0 ? 5 : 2, ci = te.rows[te.sel];
      if (code == KEY_UP)
         te.field = (te.field + 2) % 3;
      else if (code == KEY_DOWN)
         te.field = (te.field + 1) % 3;
      else if (code == KEY_LEFT || code == KEY_RIGHT)
      {
         *f += code == KEY_LEFT ? -step : step;
         if (te.field == 0)
            *f = (*f + 360) % 360;
         else
            *f = *f < 0 ? 0 : *f > mx ? mx : *f;
         th[ci].v = hsv_rgb(te.h, te.s, te.v);
         theme_changed();
         theme_word_drop();
      }
      else if (code == BTN_SOUTH)
      {
         te.picking = 0;
         snprintf(theme_name, sizeof(theme_name), "Custom");
         switch_prewarm();   /* (the accent may have changed) */
         theme_save();
      }
      else if (code == BTN_EAST)
      {
         th[ci].v = te.before;   /* put it back */
         te.picking = 0;
         theme_changed();
         switch_prewarm();
         theme_word_drop();
      }
      return 0;
   }
   if (code == KEY_UP || code == KEY_DOWN)
   {
      int d = code == KEY_UP ? -1 : 1;
      do
         te.sel = (te.sel + d + te.nrows) % te.nrows;
      while (te.rows[te.sel] >= 1000);   /* headings can't be chosen */
   }
   else if (code == BTN_TL || code == BTN_TR)
   {
      /* to the first row of the previous / next section (the rows below a heading; the top block is the first) */
      int q, cur = 0, starts[16], ns = 0;
      starts[ns++] = 0;
      for (q = 0; q + 1 < te.nrows && ns < 16; q++)
         if (te.rows[q] >= 1000)
            starts[ns++] = q + 1;
      for (q = 0; q < ns; q++)
         if (starts[q] <= te.sel)
            cur = q;
      cur = (cur + (code == BTN_TR ? 1 : te.sel == starts[cur] ? ns - 1 : 0) + (code == BTN_TR ? 0 : 0)) % ns;
      te.sel = starts[cur];
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT) && te.rows[te.sel] == TR_PRESET)
   {
      int p = theme_preset_index();
      p = p < 0 ? 0 : (p + (code == KEY_LEFT ? NPRESETS - 1 : 1)) % NPRESETS;
      theme_preset(p);
      theme_word_drop();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT || code == BTN_SOUTH) && te.rows[te.sel] == TR_LETTERING)
   {
      int p = theme_preset_index(), i;
      theme_lettering = (theme_lettering + (code == KEY_LEFT ? NPRESETS : 1)) % (NPRESETS + 1);
      for (i = TH_LET1; i <= TH_LET_EDGE; i++)
         th[i].v = theme_lettering < NPRESETS ? preset_colour(theme_lettering, i) : p >= 0 ? preset_colour(p, i) : th[i].def;
      theme_changed();
      theme_save();
      theme_word_drop();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT) && te.rows[te.sel] == TR_STARS)
   {
      theme_stars += code == KEY_LEFT ? -10 : 10;   /* a slider: 0 to 100 in steps of 10 */
      theme_stars = theme_stars < 0 ? 0 : theme_stars > 100 ? 100 : theme_stars;
      theme_save();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT || code == BTN_SOUTH) && te.rows[te.sel] == TR_PILLGLASS)
   {
      theme_glass_pill = !theme_glass_pill;
      theme_save();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT || code == BTN_SOUTH) && te.rows[te.sel] == TR_BARGLASS)
   {
      theme_glass_bar = !theme_glass_bar;
      theme_save();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT || code == BTN_SOUTH) && te.rows[te.sel] == TR_GLINT)
   {
      theme_glint = !theme_glint;
      theme_save();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT || code == BTN_SOUTH) && te.rows[te.sel] == TR_CAROUSEL)
   {
      theme_games_carousel = !theme_games_carousel;
      theme_save();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT || code == BTN_SOUTH) && te.rows[te.sel] == TR_SKYLINE)
   {
      theme_skyline = !theme_skyline;
      theme_changed();
      theme_save();
   }
   else if ((code == KEY_LEFT || code == KEY_RIGHT || code == BTN_SOUTH) && te.rows[te.sel] == TR_SKYSTYLE)
   {
      theme_sky_style = (theme_sky_style + (code == KEY_LEFT ? 2 : 1)) % 3;
      theme_changed();
      theme_save();
   }
   else if (code == BTN_SOUTH && te.rows[te.sel] == TR_RESET)
      return 1;
   else if (code == BTN_SOUTH && te.rows[te.sel] >= 0)
   {
      int ci = te.rows[te.sel];
      te.before = th[ci].v;
      rgb_hsv(th[ci].v, &te.h, &te.s, &te.v);
      te.field = 0;
      te.picking = 1;
   }
   else if (code == BTN_EAST)
      ui.view = 0;
   return 0;
}

static void theme_reset(void)
{
   theme_stars = 50;
   theme_glint = 1;
   theme_glass_pill = 1;
   theme_glass_bar = 1;
   theme_games_carousel = 1;
   theme_skyline = 1;
   theme_sky_style = 1;
   theme_lettering = 0;
   theme_preset(0);
   theme_word_drop();
}

/* The Theme card's preview: the real home screen (Consoles row, the focused console) drawn into a canvas of its own, then
 * averaged down to a quarter-ish size and put in the panel, so every colour, effect and toggle shows as it will on the
 * home screen. It is drawn again only when a colour or one of the effect settings has changed (or cards are still catching
 * up); otherwise the same picture, already scaled to the panel, is copied. The menu's own position is saved and put back,
 * so the real home screen doesn't notice. */
static void theme_preview(canvas_t *c, int x, int y, int w, int h, double now)
{
   static canvas_t off;
   static uint32_t *half;
   static image_t himg;
   static double last;
   static int settling, have, sw, sh;
   static uint32_t last_sig, *scl;
   uint32_t sig = 2166136261u;
   int i;
   if (!off.px)
   {
      off.w = off.stride = c->w;
      off.h = c->h;
      off.px = malloc((size_t)off.w * off.h * 4);
      half = malloc((size_t)(off.w / 2) * (off.h / 2) * 4);
      himg.w = off.w / 2;
      himg.h = off.h / 2;
      himg.px = half;
      last = 0;
   }
   if (!off.px || !half)
      return;
   for (i = 0; i < TH_COUNT; i++)
      sig = (sig ^ th[i].v) * 16777619u;
   sig = (sig ^ (uint32_t)(theme_stars * 8 + theme_glint * 4 + theme_glass_pill * 2 + theme_glass_bar)) * 16777619u;
   sig = (sig ^ (uint32_t)(theme_skyline * 4 + theme_sky_style)) * 16777619u;
   if (preview_defer)
   {
      preview_defer = 0;
      settling = 1;
      last = now;   /* (the next frame does the work) */
      card_pending = 1;
   }
   else if (!have || card_pending || sig != last_sig)
   {
      int row_pos = ui.row_pos, x2, y2;
      double row_anim = ui.row_anim, anim[NROWS + 1];
      for (i = 0; i <= NROWS; i++)
         anim[i] = ui.anim_[i];
      ui.row_pos = ROW_CONS;
      ui.row_anim = ROW_CONS;
      for (i = 0; i <= NROWS; i++)
         ui.anim_[i] = ui.pos_[i];
      card_pending = 0;
      draw_home(&off, now);
      ui.row_pos = row_pos;
      ui.row_anim = row_anim;
      for (i = 0; i <= NROWS; i++)
         ui.anim_[i] = anim[i];
      bg_stage_on = 0;
      bg_cur = bg;   /* (the panel's own corners are cut from the plain background again) */
      for (y2 = 0; y2 < himg.h; y2++)
         for (x2 = 0; x2 < himg.w; x2++)
         {
            const uint32_t *p0 = off.px + (size_t)(2 * y2) * off.stride + 2 * x2, *p1 = p0 + off.stride;
            uint32_t a = p0[0], b = p0[1], cc = p1[0], d = p1[1];
            half[(size_t)y2 * himg.w + x2] = 0xff000000 |
               ((((a >> 16 & 255) + (b >> 16 & 255) + (cc >> 16 & 255) + (d >> 16 & 255)) >> 2) << 16) |
               ((((a >> 8 & 255) + (b >> 8 & 255) + (cc >> 8 & 255) + (d >> 8 & 255)) >> 2) << 8) |
               (((a & 255) + (b & 255) + (cc & 255) + (d & 255)) >> 2);
         }
      last = now;
      settling = card_pending;
      last_sig = sig;
      have = 1;
      sw = 0;   /* (scaled again below) */
   }
   if (settling && now - last > 0.5)
      settling = 0;   /* (a safety: never leave the wheel up) */
   if (!have)
      return;
   if (sw != w || sh != h)
   {
      /* the picture, scaled to the panel once (not on every frame) */
      canvas_t sc;
      free(scl);
      scl = malloc((size_t)w * h * 4);
      if (!scl)
         return;
      sc.w = sc.stride = sw = w;
      sc.h = sh = h;
      sc.px = scl;
      gfx_draw_image_smooth(&sc, &himg, 0, 0, w, h);
   }
   for (i = 0; i < h && y + i < c->h; i++)
      memcpy(c->px + (size_t)(y + i) * c->stride + x, scl + (size_t)i * w, (size_t)w * 4);
   if (settling)
   {
      /* some cards still show their old colours: a theme-coloured wash over the preview (a light at the top fading to the
       * background's bottom colour; the old picture shows faintly through) with the video player's loading wheel */
      uint32_t lt = mix_colour(TH(TH_BG_TOP), TH(TH_BG_LIGHT), 160), db = TH(TH_BG_BOT);
      int xx, yy;
      for (yy = 0; yy < h && y + yy < c->h; yy++)
      {
         uint32_t wash = mix_colour(lt, db, yy * 255 / h), *row = c->px + (size_t)(y + yy) * c->stride + x;
         for (xx = 0; xx < w; xx++)
            row[xx] = mix_colour(row[xx], wash, 235) | 0xff000000;
      }
      draw_spinner(c, x + w / 2, y + h / 2, now);
   }
   stroke_round_rect(c, x, y, w, h, 6, TH(TH_CARD_LINE));
}

#define TE_ROWS 11
#define TE_ROW_H 32
static void draw_theme(canvas_t *c, double now)
{
   int i, y;
   char val[64];

   draw_background(c);
   gfx_fill_rect(c, 0, 0, c->w, 50, 0x60000000);
   gfx_draw_text(c, f_big, "Theme", 20, (50 - font_height(f_big)) / 2, C_TEXT, 400);
   draw_status(c, c->w - 16, 25, 0);

   if (te.sel < te.top + 1)
      te.top = te.sel > 0 ? te.sel - 1 : 0;   /* keep a heading above in view */
   if (te.sel >= te.top + TE_ROWS)
      te.top = te.sel - TE_ROWS + 1;
   for (i = 0; i < TE_ROWS && te.top + i < te.nrows; i++)
   {
      int r = te.rows[te.top + i], sel = te.top + i == te.sel;
      y = 58 + i * TE_ROW_H;
      if (r >= 1000)
      {
         gfx_draw_text(c, f_small, th[r - 1000].group, 40, y + TE_ROW_H - font_height(f_small) - 3, C_ACCENT, 0);
         continue;
      }
      if (sel)
      {
         gfx_fill_rect(c, 24, y + 2, 500, TE_ROW_H - 4, C_SEL);
         gfx_fill_rect(c, 24, y + 2, 4, TE_ROW_H - 4, C_ACCENT);
      }
      if (r == TR_PRESET)
      {
         gfx_draw_text(c, f_mid, "Theme", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         snprintf(val, sizeof(val), "<  %s  >", theme_name);
         gfx_draw_text(c, f_mid, val, 510 - font_text_width(f_mid, val), y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
      }
      else if (r == TR_LETTERING)
      {
         gfx_draw_text(c, f_mid, "Lettering", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         snprintf(val, sizeof(val), "<  %s  >", theme_lettering >= NPRESETS ? "Match theme"
               : theme_lettering ? presets[theme_lettering].name : "Default");
         gfx_draw_text(c, f_mid, val, 510 - font_text_width(f_mid, val), y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
      }
      else if (r == TR_STARS)
      {
         gfx_draw_text(c, f_mid, "Stars", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         if (theme_stars)
            snprintf(val, sizeof(val), "<  %d%%  >", theme_stars);
         else
            snprintf(val, sizeof(val), "<  Off  >");
         gfx_draw_text(c, f_mid, val, 510 - font_text_width(f_mid, val), y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
      }
      else if (r == TR_PILLGLASS || r == TR_BARGLASS)
      {
         gfx_draw_text(c, f_mid, r == TR_PILLGLASS ? "Translucent pill" : "Translucent bar", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         draw_switch(c, r == TR_PILLGLASS ? 1 : 2, 510, y + TE_ROW_H / 2, r == TR_PILLGLASS ? theme_glass_pill : theme_glass_bar, sel, now);
      }
      else if (r == TR_CAROUSEL)
      {
         gfx_draw_text(c, f_mid, "Games Carousel", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         draw_switch(c, 6, 510, y + TE_ROW_H / 2, theme_games_carousel, sel, now);
      }
      else if (r == TR_SKYLINE)
      {
         gfx_draw_text(c, f_mid, "Skyline", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         draw_switch(c, 7, 510, y + TE_ROW_H / 2, theme_skyline, sel, now);
      }
      else if (r == TR_SKYSTYLE)
      {
         gfx_draw_text(c, f_mid, "Skyline style", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         snprintf(val, sizeof(val), "<  %s  >", theme_sky_style == 2 ? "Forest" : theme_sky_style ? "Mountains" : "City");
         gfx_draw_text(c, f_mid, val, 510 - font_text_width(f_mid, val), y + (TE_ROW_H - font_height(f_mid)) / 2,
               sel ? C_TEXT : C_DIM, 0);
      }
      else if (r == TR_GLINT)
      {
         gfx_draw_text(c, f_mid, "Glint", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         draw_switch(c, 0, 510, y + TE_ROW_H / 2, theme_glint, sel, now);
      }
      else if (r == TR_RESET)
         gfx_draw_text(c, f_mid, "Reset to defaults", 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
      else
      {
         gfx_draw_text(c, f_mid, th[r].label, 44, y + (TE_ROW_H - font_height(f_mid)) / 2, sel ? C_TEXT : C_DIM, 0);
         snprintf(val, sizeof(val), "#%06X", th[r].v & 0xffffff);
         gfx_draw_text(c, f_small, val, 430 - font_text_width(f_small, val), y + (TE_ROW_H - font_height(f_small)) / 2, C_FAINT, 0);
         fill_round_rect(c, 448, y + 6, 62, TE_ROW_H - 12, 6, 0xff9a8aa4);
         fill_round_rect(c, 449, y + 7, 60, TE_ROW_H - 14, 5, th[r].v);
      }
   }

   /* the preview: the real home screen, made small (see theme_preview), with the list's text colours and the bar below it */
   {
      int px = 552, py = 58, pw = 290, ph = 360, cx = px + 10, cw = pw - 20;
      canvas_t sub = { pw, ph, c->stride, c->px + (size_t)py * c->stride + px };
      fill_vgradient(&sub, 0, 0, pw, ph, TH(TH_BG_TOP), TH(TH_BG_BOT));
      round_corners(c, px, py, pw, ph, 14);
      theme_preview(c, cx, py + 10, cw, cw * 480 / 854, now);
      y = py + 10 + cw * 480 / 854 + 16;
      gfx_fill_rect(c, cx, y, cw, 28, C_SEL);
      gfx_fill_rect(c, cx, y, 4, 28, C_ACCENT);
      gfx_draw_text(c, f_mid, "Chosen game", cx + 12, y + (28 - font_height(f_mid)) / 2, C_TEXT, cw - 20);
      gfx_draw_text(c, f_mid, "Another game", cx + 12, y + 32 + (28 - font_height(f_mid)) / 2, C_DIM, cw - 20);
      gfx_draw_text(c, f_small, "Faint note", cx + 12, y + 64 + (28 - font_height(f_small)) / 2, C_FAINT, 0);
      gfx_draw_text(c, f_small, "HEADING", cx + 150, y + 64 + (28 - font_height(f_small)) / 2, C_ACCENT, 0);
      y += 100;
      gfx_fill_rect(c, px + 10, y, pw - 20, 34, TH(TH_BAR));
      fill_round_rect(c, px + 19, y + 4, 92, 26, 8, TH(TH_BAR_LINE));
      fill_round_rect(c, px + 20, y + 5, 90, 24, 7, TH(TH_BAR_BOX));
      draw_glyphs(c, "A", px + 24, y + 17, 1);
      gfx_draw_text(c, f_small, "Play", px + 50, y + 17 - font_height(f_small) / 2, C_DIM, 0);
      fill_round_rect(c, px + 130, y + 6, 30, 22, 6, TH(TH_KEY_LINE));
      fill_round_rect(c, px + 131, y + 7, 28, 20, 5, TH(TH_KEY));
      draw_centered(c, f_small, "Q", px + 145, y + 17 - font_height(f_small) / 2, C_TEXT);
      fill_round_rect(c, px + 170, y + 6, 100, 22, 11, TH(TH_SUGGEST));
      draw_centered(c, f_small, "WORD", px + 220, y + 17 - font_height(f_small) / 2, C_TEXT);
   }

   if (te.picking)
   {
      /* the colour picker, over the list */
      static const char *const names[3] = { "Hue", "Saturation", "Brightness" };
      int x0 = 60, y0 = 120, w = 440, h = 230, k;
      int ci = te.rows[te.sel], vals[3] = { te.h, te.s, te.v }, mx[3] = { 359, 100, 100 };
      fill_round_rect(c, x0 - 1, y0 - 1, w + 2, h + 2, 13, C_ACCENT);
      fill_round_rect(c, x0, y0, w, h, 12, C_PANEL);
      snprintf(val, sizeof(val), "%s", th[ci].label);
      for (i = te.sel; i >= 0; i--)   /* the heading above it in the list */
         if (te.rows[i] >= 1000)
         {
            snprintf(val, sizeof(val), "%s: %s", th[te.rows[i] - 1000].group, th[ci].label);
            break;
         }
      gfx_draw_text(c, f_mid, val, x0 + 20, y0 + 14, C_TEXT, w - 140);
      fill_round_rect(c, x0 + w - 100, y0 + 12, 38, 30, 6, te.before);
      fill_round_rect(c, x0 + w - 58, y0 + 12, 38, 30, 6, th[ci].v);
      for (k = 0; k < 3; k++)
      {
         int sy = y0 + 62 + k * 46, sx = x0 + 130, sw = 220, j, sel = te.field == k;
         gfx_draw_text(c, f_small, names[k], x0 + 20, sy + 2, sel ? C_TEXT : C_DIM, 0);
         /* the slider's track shows the colours it runs through */
         for (j = 0; j < sw; j++)
         {
            int hv = k == 0 ? j * 359 / (sw - 1) : te.h, sv = k == 1 ? j * 100 / (sw - 1) : te.s,
                vv = k == 2 ? j * 100 / (sw - 1) : te.v;
            if (k == 0)
               sv = vv = 100;
            gfx_fill_rect(c, sx + j, sy + 6, 1, 12, hsv_rgb(hv, sv, vv));
         }
         {
            int kx = sx + vals[k] * (sw - 1) / mx[k];
            fill_round_rect(c, kx - 5, sy + 1, 10, 22, 4, sel ? 0xffffffff : 0xffb0a0b8);
         }
         snprintf(val, sizeof(val), "%d", vals[k]);
         gfx_draw_text(c, f_small, val, sx + sw + 14, sy + 2, sel ? C_TEXT : C_DIM, 0);
      }
      draw_hints(c, "Dpad:UD  Choose     Dpad:LR  Change     A  Keep     B  Cancel");
   }
   else
   {
      int tr = te.rows[te.sel];
      draw_hints(c, tr == TR_PRESET ? "Dpad:UD  Move     Dpad:LR  Theme     L / R  Section     B  Back"
            : tr == TR_LETTERING ? "Dpad:UD  Move     Dpad:LR  Change     L / R  Section     B  Back"
            : tr == TR_STARS ? "Dpad:UD  Move     Dpad:LR  Fewer / more     L / R  Section     B  Back"
            : tr == TR_SKYSTYLE ? "Dpad:UD  Move     Dpad:LR  Change     L / R  Section     B  Back"
            : tr == TR_GLINT || tr == TR_PILLGLASS || tr == TR_BARGLASS || tr == TR_CAROUSEL || tr == TR_SKYLINE ? "Dpad:UD  Move     A  On / off     L / R  Section     B  Back"
            : tr == TR_RESET ? "A  Reset     L / R  Section     B  Back" : "A  Change colour     L / R  Section     B  Back");
   }
   draw_toast(c, now);
   draw_volume(c, now);
}

int main(int argc, char **argv)
{
   canvas_t *c;
   int i;
   double last_counts = 0, power_down = 0, repeat_at = 0, usb_check = 0, unplugged_at = 0, ft_off_at = 0, bat_check = 0;
   int select_down = 0, select_used = 0, show_controls = 0;
   double last_input = 0, last_key = 0, vol_check = 0, thumb_check = 0;

   int busy = 1, bat_level = 0, prompt = 0, prompt_choice = 1, screen_on = 1, launched = 0, held_dir = 0;
   int video_mode = 0, chat = 0, prewarm_more = 0, tip_done = 0;
   int freezable = 0, use_frozen = 0, same_frames = 0;
   uint32_t fkey = 0;
   canvas_t *dc;

   signal(SIGTERM, on_signal);
   signal(SIGINT, on_signal);
   signal(SIGUSR1, on_usr1);
   signal(SIGUSR2, on_usr2);
   signal(SIGCHLD, SIG_IGN);   /* file-transfer helpers reap themselves */

   if (freopen(MENU_LOG, "a", stderr))
      setvbuf(stderr, NULL, _IOLBF, 0);
   play_mode = argc >= 6 && !strcmp(argv[1], "--play");
   mlog(play_mode ? "player start" : "menu start");
   touch(ALIVE_FLAG);

   if (gfx_init() < 0)
   {
      fprintf(stderr, "g36menu: display init failed\n");
      return 1;
   }
   input_open();
   remap_load();
   menu_map_apply();   /* the menu's own buttons, as set on the Controls screen */
   c = gfx_canvas();
   f_huge  = font_load(FONT, 50);
   f_big   = font_load(FONT, 30);
   f_mid   = font_load(FONT, 22);
   f_small = font_load(FONT, 18);
   f_title = font_load_bold(FONT, 18);
   f_label = font_load_bold(FONT, 24);
   f_tiny  = font_load_bold(FONT, 12);
   if (!f_huge || !f_big || !f_mid || !f_small)
   {
      fprintf(stderr, "g36menu: font load failed\n");
      return 1;
   }

   theme_load();
   vq_load();
   if (play_mode)
   {
      /* only what playing needs: no games, box art or cards */
      double t = now_s();
      st_battery = battery_level();
      st_power = external_power();
      last_input = t;
      screen_off_s = setting_int("screen_off_seconds", 180);
      power_off_min = setting_int("auto_power_off_minutes", 10);
      clock_24h = setting_int("clock_24h", 0);
      apply_brightness(setting_int("brightness_percent", 0));
      mb.mode = atoi(argv[2]) == MK_MUSIC ? MK_MUSIC : MK_VIDEO;
      snprintf(mb.base, sizeof(mb.base), "%s", argv[3]);
      snprintf(mb.folder, sizeof(mb.folder), "%s", argv[4]);
      snprintf(mb.want, sizeof(mb.want), "%s", argv[5]);
      ui.sel_[ROW_MEDIA] = argc > 6 ? atoi(argv[6]) : 0;
      mb.sig = media_scan(0);
      vq.speed = SPEED_1;   /* picked from the list: normal speed (playing on keeps it) */
      if (!mb.n || strcmp(mb.e[mb.sel].name, argv[5]) || media_play(mb.sel, t) < 0)
      {
         mlog("player: couldn't play %s", argv[5]);
         media_return_write();
         input_close();
         gfx_shutdown();
         unlink(ALIVE_FLAG);
         return 0;
      }
      goto run;
   }
   mkdir(ROMS, 0755);
   load_systems();
   lib_load();
   refresh_counts();
   load_collections();
   load_favs();
   load_playtime();
   st_battery = battery_level();
   st_power = external_power();
   last_input = now_s();
   screen_off_s = setting_int("screen_off_seconds", 180);
   power_off_min = setting_int("auto_power_off_minutes", 10);
   clock_24h = setting_int("clock_24h", 0);
   kc.vol = setting_int("click_volume", 50);
   if (!file_exists("/tmp/g36-started"))
   {
      /* the first start since power-on: the start-up levels, if they're set */
      int sb = setting_int("startup_brightness", -1), sv = setting_int("startup_volume", -1);
      touch("/tmp/g36-started");
      if (sb >= 10)
         set_setting("brightness_percent", sb);
      if (sv >= 0)
         set_volume(sv);
      if (sb >= 10 || sv >= 0)
         mlog("start-up levels: brightness %d, volume %d (-1 = last used)", sb, sv);
   }
   apply_brightness(setting_int("brightness_percent", 0));   /* only once it has been set */
   if (check_launch())
   {
      prompt = PROMPT_ERROR;
      prompt_choice = 0;
   }
   if (!prompt && games_drive_missing())
   {
      mlog("the games drive isn't mounted");
      snprintf(err_title, sizeof(err_title), "Can't read the games drive");
      snprintf(err_line1, sizeof(err_line1), "The memory card's games drive didn't start up.");
      snprintf(err_line2, sizeof(err_line2), "Power off and check the card. Nothing was erased.");
      prompt = PROMPT_ERROR;
      prompt_choice = 0;
   }
   if (!prompt && file_exists(RECOVERED_FLAG))
   {
      mlog("recovered: the menu kept stopping; the saved view, library list and theme were set aside");
      snprintf(err_title, sizeof(err_title), "The menu had trouble starting");
      snprintf(err_line1, sizeof(err_line1), "Its saved view, game list and theme were set aside");
      snprintf(err_line2, sizeof(err_line2), "(g36/menu-state.bad, library.cache.bad, theme.cfg.bad).");
      prompt = PROMPT_ERROR;
      prompt_choice = 0;
   }
   unlink(RECOVERED_FLAG);
   mlog("%d systems, %d with games", nsystems, nvisible);
   last_counts = now_s();
   ui.row_pos = ROW_CONS;   /* start on the consoles unless the saved state says otherwise */
   load_state();
   if (!play_mode && !file_exists(BOOT_SEEN))
   {
      /* the first start since the handheld was switched on (/tmp is empty then): always open on Settings in the Tools row,
       * so there is one known place to start from. Later starts (back from a game, a restart) keep the saved place. */
      FILE *bf;
      int ti;
      ui.view = 0;
      ui.list_all = 0;
      ui.folder[0] = 0;
      ui.row_pos = ROW_TOOLS;
      for (ti = 0; ti < NTOOLS; ti++)
         if (!strcmp(tool_names[ti], "Settings"))
            ui.sel_[ROW_TOOLS] = ti;
      if ((bf = fopen(BOOT_SEEN, "w")))
         fclose(bf);
   }
   ui.row_anim = ui.row_pos;
   for (i = 0; i < NROWS; i++)
      row_set(i, ui.sel_[i]);
   if (ui.view == 1)
      reload_list();
   media_return_read(now_s());   /* back from playing a video or song */
   if (!play_mode && file_exists("/tmp/sleep-failed"))
   {
      unlink("/tmp/sleep-failed");
      toast("That game can't sleep (no state to come back to). It was saved and closed.", now_s());
      ui.toast_until = now_s() + 5;
   }
   if (!play_mode && file_exists("/tmp/sleeping"))
   {
      /* g36-sleep closed a game to sleep: the menu waits with the screen off */
      screen_on = 0;
      gfx_screen_power(0);
   }

run:
   while (!quit && !launched && !chat && !play_req && !(play_mode && ui.view != 4))
   {
      key_event_t ev;
      double t = now_s();
      int moved = 0, wait_ms;

      card_spent = 0;

      /* keep lists in step with the folders */
      if ((ui.view == 0 || ui.view == 1) && screen_on)
      {
         /* the folders are checked on a thread; what it found is used here, once it's in */
         double *last = ui.view == 0 ? &last_counts : &ui.list_check;
         double every = ui.view == 1 && !ui.list_all ? 2.0 : 2.5;
         if (__atomic_load_n(&watch_state, __ATOMIC_ACQUIRE) == 2)
         {
            if (ui.view == 0)
            {
               refresh_counts_x(watch_changed);
               load_collections();   /* new files in collections/, changed counts */
            }
            else if (ui.list_all)
            {
               if (refresh_counts_x(watch_changed))
               {
                  load_collections();
                  reload_list();
               }
            }
            else
            {
               system_t *sy = cur_system();
               if (sy && watch_changed[sy - systems])
               {
                  lib_scan(sy);
                  mlog("library: scanned %s (%d games)", sy->id, sy->ngames);
                  lib_save();
                  reload_list();
               }
            }
            __atomic_store_n(&watch_state, 0, __ATOMIC_RELEASE);
            *last = t;
         }
         else if (t - *last > every)
         {
            watch_start();
            *last = t;
         }
      }

      /* moving: draw every refresh (the page flip sets the pace); still: ~10 frames a
       * second. After the first button, take the rest without waiting. */
      wait_ms = !screen_on ? 200 : (busy || prewarm_more || card_pending) ? 0 : 100;
      if (screen_on && !busy && ui.view == 1 && fold_wake > 0)
      {
         /* a folder's list is about to slide: be awake for the start, so it doesn't begin late */
         int w = (int)((fold_wake - now_s()) * 1000);
         if (w < 0)
            w = 0;
         if (w < wait_ms)
            wait_ms = w;
      }
      if (screen_on && ui.view == 4 && pl.kind == MK_VIDEO && !busy)
         wait_ms = player_wait_ms(player_paused() ? 100 : 20);
      /* a screen that hasn't changed for a few frames is looked at less often (a button, or anything moving, is quick again;
       * the Power button's hold and a folder about to slide keep their short waits) */
      if (wait_ms == 100 && screen_on && !busy && same_frames >= 3 && ui.view != 4 && !power_down && !(ui.view == 1 && fold_wake > 0))
         wait_ms = same_frames >= 12 ? 1000 : same_frames >= 6 ? 400 : 200;
      /* a still screen with only stars twinkling (they take 3 to 7 seconds a cycle) needs about 5 frames a second, not 10 */
      if (wait_ms == 100 && screen_on && !busy && home_clean && theme_stars > 0 && (ui.view == 0 || (ui.view == 1 && gc.on)) && now_s() - home_clean_t < 2.0)
         wait_ms = bat_low ? 500 : 200;   /* (a low battery: 2 frames a second, about a third of the work) */
      while (input_next(&ev, wait_ms))
      {
         wait_ms = 0;
         t = now_s();   /* (the wait may have been long: a button's time is when it came) */
         home_clean = 0;
         last_input = t;
         last_key = t;   /* (last_input is also held up by a charger; this is the buttons only) */

         /* volume: the factory helper changes it; the menu just shows it */
         if (ev.code == KEY_VOLUMEUP || ev.code == KEY_VOLUMEDOWN)
         {
            if (ev.value && screen_on)
            {
               vol_until = t + 1.5;
               vol_check = 0;
            }
            continue;
         }
         if (show_controls)
         {
            /* the Controls screen takes the real buttons (input_set_raw) */
            if (ev.value == 1 && remap_key(ev.code))
               show_controls = 0;
            if (ev.code == BTN_SELECT && ev.value == 0)
               select_down = 0;
            continue;
         }
         if (ev.code == KEY_POWER)
         {
            double pnow = now_s();
            int guarded = wake_guard(pnow);
            mlog("power %s (%.2f s after waking%s) screen_on=%d %s", ev.value ? "down" : "up", pnow - wake_seen_at,
                 "", screen_on, screen_dbg());
            if (ev.value == 1)
               power_down = t;
            else if (ev.value == 0 && power_down)
            {
               if (t - power_down < 1.0 && !prompt && !file_exists("/tmp/sleep-wake"))
               {
                  if (guarded)
                     wake_tap_used = 1;
                  screen_on = !screen_on;
                  gfx_screen_power(screen_on);
                  if (guarded)
                     mlog("screen %s, %.2f s after waking: %s", screen_on ? "on" : "off", now_s() - wake_seen_at, screen_dbg());
                  if (!screen_on && ui.view == 4 && pl.kind == MK_VIDEO && !player_sound_only())
                     player_set_paused(1);
               }
               power_down = 0;
            }
            continue;
         }
         if (ev.value != 1)
         {
            if (ev.value == 0 && (ev.code == KEY_UP || ev.code == KEY_DOWN || ev.code == KEY_LEFT || ev.code == KEY_RIGHT))
               held_dir = 0;
            /* Select tapped on its own (not held for Select + A): All Games */
            if (ev.value == 0 && ev.code == BTN_SELECT)
            {
               if (select_down && !select_used && !prompt && screen_on)
                  open_all_games(t);
               select_down = 0;
            }
            continue;
         }
         if (!screen_on)
         {
            /* any button wakes the screen without acting */
            screen_on = 1;
            gfx_screen_power(1);
            continue;
         }

         if (ev.code == BTN_SELECT)
         {
            select_down = 1;
            select_used = 0;
            if (ui.view >= 3 && !prompt)
            {
               select_used = 1;
               if (ui.view == 4 && !vq.open)
                  media_key(BTN_SELECT, t);
            }
            continue;
         }

         if (prompt == PROMPT_CORE)
         {
            if (ev.code == KEY_UP)
               pick.sel = (pick.sel + pick.n - 1) % pick.n;
            else if (ev.code == KEY_DOWN)
               pick.sel = (pick.sel + 1) % pick.n;
            else if (ev.code == BTN_EAST)
               prompt = 0;
            else if (ev.code == BTN_SOUTH)
            {
               prompt = PROMPT_CORE_HOW;
               prompt_choice = 0;
            }
            continue;
         }

         if (prompt == PROMPT_CORE_HOW && ev.code == BTN_SOUTH)
         {
            system_t *s = &systems[pick.sys];
            const char *core = pick.cores[pick.sel];
            if (prompt_choice == 1)
            {
               /* this game; choosing the console's own default just clears the choice */
               set_game_core(pick.rom, strcmp(core, s->cores[0]) ? core : NULL);
               mlog("emulator for %s: %s", pick.rom, core);
            }
            else if (prompt_choice == 2)
            {
               set_console_core(s, core);
               set_game_core(pick.rom, NULL);
               mlog("emulator for %s: %s (console default)", s->id, core);
            }
            prompt = 0;
            emu_cache_dirty = 1;
            {
               int rr = launch_core(s, pick.rom, core);
               if (rr == 0)
                  launched = 1;
               else if (rr == -2 || rr == -3)
               {
                  prompt = rr == -2 ? PROMPT_ERROR : PROMPT_BIOS;
                  prompt_choice = rr == -2 ? 0 : 1;
               }
            }
            continue;
         }
         if (prompt == PROMPT_CORE_HOW && ev.code == BTN_EAST)
         {
            prompt = PROMPT_CORE;   /* back to the emulator list */
            continue;
         }

         if (prompt)
         {
            int toggle_prompt = prompt == PROMPT_LAUNCH_TIP || (prompt == PROMPT_BIOS && pending.psx && !pending.sleep);
            if (toggle_prompt && (ev.code == KEY_UP || ev.code == KEY_DOWN))
               prompt_choice ^= 1;
            else if (ev.code == KEY_LEFT)
               prompt_choice = (prompt_choice + prompt_opts(prompt) - 1) % prompt_opts(prompt);
            else if (ev.code == KEY_RIGHT)
               prompt_choice = (prompt_choice + 1) % prompt_opts(prompt);
            else if (ev.code == BTN_EAST)
            {
               if (prompt == PROMPT_LAUNCH_TIP)
               {
                  if (prompt_toggle)
                     set_setting("tips_left", 0);
                  launched = 1;   /* (B is "Play" here: the game was already chosen) */
               }
               prompt = 0;
            }
            else if (ev.code == BTN_SOUTH)
            {
               int sleep_at = power_restart ? 2 : 1;   /* where Sleep is in the Power prompt */
               if (toggle_prompt && prompt_choice == 0)
               {
                  prompt_toggle ^= 1;   /* the switch: flip it, stay here */
                  continue;
               }
               if (prompt == PROMPT_LAUNCH_TIP)
               {
                  if (prompt_toggle)
                     set_setting("tips_left", 0);   /* never again */
                  launched = 1;
                  prompt = 0;
                  continue;
               }
               if (prompt == PROMPT_POWER && prompt_choice == sleep_at)
               {
                  prompt = PROMPT_SLEEP;   /* the note first */
                  prompt_choice = 0;       /* Sleep first: it's what was just picked */
                  continue;
               }
               else if (prompt == PROMPT_POWER && prompt_choice < sleep_at)
               {
                  save_state();
                  mlog(prompt_choice == 0 ? "power off" : "restart");
                  touch(prompt_choice == 0 ? HALT_FLAG : REBOOT_FLAG);
                  quit = 1;
               }
               else if (prompt_choice == 0 && prompt == PROMPT_SLEEP)
               {
                  prompt = 0;
                  go_to_sleep(&screen_on);
                  continue;
               }
               else if (prompt_choice == 0 && prompt == PROMPT_USB)
               {
                  filetransfer("on");
                  ft_on = 1;
               }
               else if (prompt_choice == 0 && prompt == PROMPT_RESET)
                  resume_reset(t);
               else if (prompt_choice == 0 && prompt == PROMPT_PIC_DELETE)
                  gal_delete(t);
               else if (prompt_choice == 0 && prompt == PROMPT_SET_DEFAULTS)
               {
                  settings_defaults();
                  toast("Settings back to the defaults", t);
               }
               else if (prompt_choice == 0 && prompt == PROMPT_THEME_RESET)
               {
                  theme_reset();
                  toast("Back to the default colours", t);
               }
               else if (prompt == PROMPT_BIOS && pending.sleep)
               {
                  sleepmark_t m;
                  if (prompt_choice == 1 && sleep_marker_for(pending.rom, &m) && sleep_load(&m) == 0)
                  {
                     launched = 1;
                     tip_done = 1;   /* (no tip on the way into a resumed game) */
                  }
                  else if (prompt_choice == 0)
                  {
                     int rr;
                     unlink(SLEEP_MARKER);   /* a normal start: the offer is done with */
                     mlog("launch: starting normally, without the sleep state");
                     rr = launch_core_checked(&systems[pending.sys], pending.rom, pending.core, 0);
                     if (rr == 0)
                        launched = 1;
                     else if (rr == -2 || rr == -3)
                     {
                        prompt = rr == -2 ? PROMPT_ERROR : PROMPT_BIOS;
                        prompt_choice = rr == -2 ? 0 : 1;
                        continue;
                     }
                  }
               }
               else if (prompt == PROMPT_BIOS && pending.psx)
               {
                  if (prompt_toggle)
                     set_setting("psx_sticks_warned", 1);
                  if (launch_core_checked(&systems[pending.sys], pending.rom, pending.core, 1) == 0)
                     launched = 1;
               }
               else if (prompt_choice == 0 && prompt == PROMPT_BIOS)
               {
                  mlog("launch warning: playing anyway");
                  if (launch_core_checked(&systems[pending.sys], pending.rom, pending.core, 1) == 0)
                     launched = 1;
               }
               prompt = 0;
            }
            continue;
         }

         if (ev.code == KEY_UP || ev.code == KEY_DOWN || ev.code == KEY_LEFT || ev.code == KEY_RIGHT)
         {
            held_dir = ev.code;
            repeat_at = t + 0.35;
         }
         if (ev.code == KEY_UP || ev.code == KEY_DOWN || ev.code == KEY_LEFT || ev.code == KEY_RIGHT ||
               ev.code == BTN_TL || ev.code == BTN_TR)
         {
            if (!click_volume_adjust(ev.code))
               key_click();
         }

         if (ui.view == 8)
         {
            if (theme_key(ev.code))
            {
               prompt = PROMPT_THEME_RESET;
               prompt_choice = 1;   /* Cancel first */
            }
         }
         else if (ui.view == 9)
         {
            if (gal_key(ev.code, t))
            {
               prompt = PROMPT_PIC_DELETE;
               prompt_choice = 1;   /* Cancel first */
            }
         }
         else if (ui.view == 6 && ev.code == BTN_SOUTH && sk.r == -2 && sk.p < nhits)
         {
            if (select_down && hits[sk.p].kind == H_GAME)
            {
               const entry_t *e = &sk.res.e[hits[sk.p].a];
               char rom[1300];
               select_used = 1;
               snprintf(rom, sizeof(rom), "%s/%s/%s", ROMS, systems[e->sys].id, e->name);
               if (picker_setup(e, rom, t))
                  prompt = PROMPT_CORE;
            }
            else
            {
               int r = hit_open(sk.p, t);
               if (r == 1)
                  launched = 1;
               else if (r == 2)
                  show_controls = controls_open();
               else if (r == 3 || r == 4)
               {
                  prompt = r == 3 ? PROMPT_ERROR : PROMPT_BIOS;
                  prompt_choice = r == 3 ? 0 : 1;   /* BIOS: Cancel first */
               }
            }
         }
         else if (ui.view == 6)
            search_key(ev.code, t);
         else if (ui.view == 7 && ev.code == BTN_SOUTH && select_down && nhits && hits[hit_sel].kind == H_GAME)
         {
            const entry_t *e = &sk.res.e[hits[hit_sel].a];
            char rom[1300];
            select_used = 1;
            snprintf(rom, sizeof(rom), "%s/%s/%s", ROMS, systems[e->sys].id, e->name);
            if (picker_setup(e, rom, t))
               prompt = PROMPT_CORE;
         }
         else if (ui.view == 7)
         {
            int r = hits_key(ev.code, t);
            if (r == 1)
               launched = 1;
            else if (r == 2)
               show_controls = controls_open();
            else if (r == 3 || r == 4)
            {
               prompt = r == 3 ? PROMPT_ERROR : PROMPT_BIOS;
               prompt_choice = r == 3 ? 0 : 1;   /* BIOS: Cancel first */
            }
         }
         else if ((ui.view == 3 || ui.view == 5) && mb.mode == MK_VIDEO && ev.code == BTN_NORTH)
         {
            if (resume_count())
            {
               prompt = PROMPT_RESET;
               prompt_choice = 1;   /* Cancel first */
            }
            else
               toast("No watch progress to reset", t);
         }
         else if (ui.view >= 3 && ui.view <= 5)
            media_key(ev.code, t);
         else if (ui.view == 0)
         {
            int r = cur_row(), n = row_count(r);
            if (ev.code == KEY_UP)
               ui.row_pos--;
            else if (ev.code == KEY_DOWN)
               ui.row_pos++;
            else if ((ev.code == KEY_LEFT || ev.code == BTN_TL) && n)
               row_move(r, -1);
            else if ((ev.code == KEY_RIGHT || ev.code == BTN_TR) && n)
               row_move(r, 1);
            else if (ev.code == BTN_SOUTH && r == ROW_CONS && !nvisible)
               toast("Copy games into roms/<console>/ on G36-GAMES", t);
            else if (ev.code == BTN_SOUTH && r == ROW_CONS && n)
            {
               ui.view = 1;
               ui.list_all = 0;
               ui.folder[0] = 0;
               ui.top = 0;
               ui.sel = 0;
               reload_list();
               ui.list_check = t;
            }
            else if ((ev.code == BTN_SOUTH && r == ROW_COLL && ui.sel_[r] == COLL_SEARCH) || ev.code == BTN_NORTH)
               search_open();   /* the Search card, or X from anywhere on the home screen */
            else if (ev.code == BTN_WEST)
               show_controls = controls_open();   /* Y on the home screen: the controls */
            else if (ev.code == BTN_SOUTH && r == ROW_COLL && n)
               open_collection(ui.sel_[r], t);
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_RETROARCH)
            {
               if (launch_retroarch() == 0)
                  launched = 1;
            }
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_SETTINGS)
            {
               settings_open();
               ui.view = 2;
            }
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_CONTROLS)
               show_controls = controls_open();
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_THEME)
               theme_open();
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_CHAT)
               chat = 1;   /* below, once the loop ends */
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_IMAGES)
               chat = 2;   /* the Image Generator, the same way */
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_PICTURES)
               gal_open();
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_VIDEO)
               media_open(MK_VIDEO, t);
            else if (ev.code == BTN_SOUTH && r == ROW_TOOLS && ui.sel_[r] == TOOL_MUSIC)
               media_open(MK_MUSIC, t);
            else if (ev.code == BTN_SOUTH)
               toast("Coming soon", t);
         }
         else if (ui.view == 2 && st.editing_clock)
         {
            if (ev.code == KEY_LEFT)
               st.field = (st.field + 4) % 5;
            else if (ev.code == KEY_RIGHT)
               st.field = (st.field + 1) % 5;
            else if (ev.code == KEY_UP)
               clock_edit_change(1);
            else if (ev.code == KEY_DOWN)
               clock_edit_change(-1);
            else if (ev.code == BTN_SOUTH)
               clock_edit_save(t);
            else if (ev.code == BTN_EAST)
               st.editing_clock = 0;
         }
         else if (ui.view == 2 && st.about)
         {
            if (ev.code == BTN_EAST || ev.code == BTN_SOUTH)
               st.about = 0;
         }
         else if (ui.view == 2)
         {
            if (ev.code == KEY_UP)
               settings_move(-1);
            else if (ev.code == KEY_DOWN)
               settings_move(1);
            else if (ev.code == KEY_LEFT)
               settings_change(-1);
            else if (ev.code == KEY_RIGHT)
               settings_change(1);
            else if (ev.code == BTN_EAST)
               ui.view = 0;
            else if (ev.code == BTN_SOUTH && srows[st.sel].kind == S_CHOICE && srows[st.sel].nvals == 2)
               settings_change(st.val[st.sel] == srows[st.sel].vals[1] ? -1 : 1);   /* A flips an on/off switch */
            else if (ev.code == BTN_SOUTH && srows[st.sel].kind == S_ACTION)
            {
               int a = srows[st.sel].action;
               if (a == A_CLOCK)
                  clock_edit_open();
               else if (a == A_ABOUT)
               {
                  st.about = 1;
                  du.measured = 0;   /* measured when the page is first drawn */
               }
               else if (a == A_SLEEP)
               {
                  prompt = PROMPT_SLEEP;
                  prompt_choice = 0;   /* Sleep first: it's what was just picked */
               }
               else if (a == A_DEFAULTS)
               {
                  prompt = PROMPT_SET_DEFAULTS;
                  prompt_choice = 1;   /* Cancel first */
               }
               else if (a == A_RESCAN)
               {
                  /* forget the cache and look at every folder again */
                  int k;
                  char msg[64];
                  pthread_mutex_lock(&lib_m);
                  for (k = 0; k < nsystems; k++)
                  {
                     lib_clear(&systems[k]);
                     systems[k].known = 0;
                  }
                  pthread_mutex_unlock(&lib_m);
                  refresh_counts();
                  load_collections();
                  mlog("library rescanned by hand: %d games", total_games());
                  snprintf(msg, sizeof(msg), "Library rescanned: %d games", total_games());
                  toast(msg, t);
               }
               else if (a == A_RESTART || a == A_POWEROFF)
               {
                  save_state();
                  mlog(a == A_RESTART ? "restart (Settings)" : "power off (Settings)");
                  touch(a == A_RESTART ? REBOOT_FLAG : HALT_FLAG);
                  quit = 1;
               }
            }
         }
         else if (ui.view == 1)
         {
            if (gc.on && gc_nav(ev.code, t))
               ;   /* (the games carousel: along its rows, between them, by letter) */
            else if (ev.code == KEY_UP && ui.list.n)
               ui.sel = (ui.sel + ui.list.n - 1) % ui.list.n;
            else if (ev.code == KEY_DOWN && ui.list.n)
               ui.sel = (ui.sel + 1) % ui.list.n;
            else if (ev.code == BTN_WEST && ui.list.n && !ui.list.e[ui.sel].is_dir)
            {
               char key[1300];
               int now_fav;
               entry_key(&ui.list.e[ui.sel], key, sizeof(key));
               now_fav = toggle_fav(key);
               toast(now_fav ? "Added to Favorites" : "Removed from Favorites", t);
               mlog("favorite %s: %s", now_fav ? "added" : "removed", key);
               load_collections();
               if (ui.list_all && ui.coll == COLL_FAVS)
               {
                  /* the list is Favorites itself: the game leaves it */
                  int keep = ui.sel;
                  reload_list();
                  ui.sel = keep < ui.list.n ? keep : ui.list.n - 1;
                  gc_focus();
                  if (!ui.list.n)
                  {
                     ui.view = 0;
                     ui.list_all = 0;
                  }
               }
            }
            else if (ev.code == BTN_START && ui.list_all && ui.coll == COLL_RECENT && ui.list.n && !ui.list.e[ui.sel].is_dir)
            {
               /* Start in Recently Played: this game leaves the list */
               char key[1300];
               int keep = ui.sel;
               entry_key(&ui.list.e[ui.sel], key, sizeof(key));
               remove_recent(key);
               mlog("removed from Recently Played: %s", key);
               load_collections();
               reload_list();
               ui.sel = keep < ui.list.n ? keep : ui.list.n - 1;
               if (ui.sel < 0)
                  ui.sel = 0;
               gc_focus();   /* (the carousel follows the choice) */
               toast("Removed from Recently Played", t);
               if (!ui.list.n)
               {
                  ui.view = 0;
                  ui.list_all = 0;
               }
            }
            else if ((ev.code == KEY_LEFT || ev.code == KEY_RIGHT) && ui.list_all)
            {
               /* the previous or next collection that has games */
               int k, ci = ui.coll;
               for (k = 1; k < ncolls; k++)
               {
                  ci = wrap(ci + (ev.code == KEY_LEFT ? -1 : 1), ncolls);
                  if (colls[ci].count)
                     break;
               }
               if (ci != ui.coll && colls[ci].count)
               {
                  ui.coll = ci;
                  row_set(ROW_COLL, ci);
                  list_free(&ui.list);
                  ui.sel = ui.top = 0;
                  reload_list();
                  toast(colls[ci].name, t);
               }
            }
            else if (ev.code == BTN_NORTH)
               search_open();
            else if (ev.code == BTN_EAST && ui.list_all && ui.coll == COLL_SEARCH)
            {
               ui.view = 6;   /* back to the search, as it was */
               ui.list_all = 0;
               list_free(&ui.list);
            }
            else if (ev.code == BTN_TL)
               letter_jump(-1, t);
            else if (ev.code == BTN_TR)
               letter_jump(1, t);
            else if ((ev.code == KEY_LEFT || ev.code == KEY_RIGHT) && !ui.list_all && nvisible > 1)
            {
               /* the previous or next console's list, without going back out */
               row_move(ROW_CONS, ev.code == KEY_LEFT ? -1 : 1);
               ui.folder[0] = 0;
               ui.want_entry[0] = 0;
               list_free(&ui.list);
               ui.sel = ui.top = 0;
               reload_list();
               ui.list_check = t;
               toast(cur_system()->name, t);
            }
            else if (ev.code == BTN_EAST)
            {
               char *slash = strrchr(ui.folder, '/');
               if (ui.folder[0])
               {
                  /* up one folder, reselecting the folder we came from */
                  snprintf(ui.want_entry, sizeof(ui.want_entry), "%s", slash ? slash + 1 : ui.folder);
                  if (slash)
                     *slash = 0;
                  else
                     ui.folder[0] = 0;
                  reload_list();
               }
               else
               {
                  ui.view = 0;
                  ui.list_all = 0;
                  list_free(&ui.list);
                  last_counts = 0;   /* recheck every console right away */
               }
            }
            else if (ev.code == BTN_SOUTH && ui.list.n)
            {
               entry_t *e = &ui.list.e[ui.sel];
               if (e->is_dir)
               {
                  size_t l = strlen(ui.folder);
                  snprintf(ui.folder + l, sizeof(ui.folder) - l, "%s%s", l ? "/" : "", e->name);
                  ui.sel = ui.top = 0;
                  reload_list();
               }
               else
               {
                  char dir[1024], rom[1300];
                  if (ui.list_all)
                     snprintf(dir, sizeof(dir), "%s/%s", ROMS, systems[e->sys].id);
                  else
                     current_dir(dir, sizeof(dir));
                  snprintf(rom, sizeof(rom), "%s/%s", dir, e->name);
                  if (select_down)
                  {
                     /* Select + A: the emulator picker */
                     select_used = 1;
                     if (picker_setup(e, rom, t))
                        prompt = PROMPT_CORE;
                  }
                  else
                  {
                     int r = launch(&systems[e->sys], rom);
                     if (r == 0)
                        launched = 1;
                     else if (r == -2 || r == -3)
                     {
                        prompt = r == -2 ? PROMPT_ERROR : PROMPT_BIOS;
                        prompt_choice = r == -2 ? 0 : 1;   /* BIOS: Cancel first */
                     }
                  }
               }
            }
         }
         moved = 1;
      }
      (void)moved;
      t = now_s();   /* (and so is the frame's, after a long wait with nothing) */

      /* the first few launches: a tip on sleeping and powering off, before the menu hands over */
      if (launched && !tip_done)
      {
         int left = setting_int("tips_left", 3);
         tip_done = 1;
         if (left > 0)
         {
            set_setting("tips_left", left - 1);
            launched = 0;
            prompt = PROMPT_LAUNCH_TIP;
            prompt_choice = 1;   /* on the button */
            prompt_toggle = 0;
            held_dir = 0;
            screen_on = 1;
         }
      }

      /* hold power for the shutdown prompt */
      if (power_down && prompt != PROMPT_POWER && t - power_down >= 1.5)
      {
         if (!screen_on)
         {
            screen_on = 1;
            gfx_screen_power(1);
         }
         prompt = PROMPT_POWER;
         power_restart = external_power();
         prompt_choice = prompt_opts(prompt) - 1;   /* Cancel */
         power_down = 0;
      }

      /* computer plugged in: offer file transfer, once per plug-in; unplugged: stop it */
      if (t - usb_check > 0.5)
      {
         usb_check = t;
         ft_on = file_exists(FT_FLAG);
         st_power = external_power();
         {
            /* every change of the cable or of the USB controller's state goes in the log, for when a plug-in
             * doesn't offer file transfer (or ADB) the way it should */
            static int last_cable = -1;
            static char last_state[32];
            char st[32];
            int cab = cable_in();
            udc_state(st, sizeof(st));
            if (cab != last_cable || strcmp(st, last_state))
            {
               mlog("usb: cable %s, controller \"%s\"%s%s", cab ? "in" : "out", st, ft_on ? ", file transfer on" : "",
                     file_exists(USB_ASKED) ? ", already asked" : "");
               last_cable = cab;
               snprintf(last_state, sizeof(last_state), "%s", st);
            }
         }
         if (cable_in())
            unplugged_at = 0;
         else if (!unplugged_at)
            unplugged_at = t;
         if (unplugged_at && t - unplugged_at > 2.0)
         {
            unlink(USB_ASKED);
            if (prompt == PROMPT_USB)
               prompt = 0;
            if (ft_on && t - ft_off_at > 5.0)
            {
               filetransfer("off");
               ft_off_at = t;
            }
         }
         else if (!prompt && !ft_on && !file_exists(USB_ASKED) && computer_connected())
         {
            touch(USB_ASKED);
            if (!screen_on)
            {
               screen_on = 1;
               gfx_screen_power(1);
            }
            mlog("computer connected: asking about file transfer");
            prompt = PROMPT_USB;
            prompt_choice = 0;
            held_dir = 0;
         }
      }

      /* thumbnails inbox: resize in the background; show the results when done */
      if (!play_mode && t - thumb_check > 3.0)   /* (the menu's job, not the player's) */
      {
         thumb_check = t;
         if (thumbs_check())
         {
            thumbs_clear();
            mlog("thumbnails: inbox done");
         }
      }


      /* the volume bar stays up while a volume button is held (g36d repeats the
       * steps) and follows the real level */
      if (screen_on && (input_held(KEY_VOLUMEUP) || input_held(KEY_VOLUMEDOWN)))
         vol_until = t + 1.5;
      if (t < vol_until && t - vol_check > 0.1)
      {
         st_volume = read_volume();
         vol_check = t;
      }

      /* screen-off and power-off timers; never on a charger or computer, or while a
       * video plays (music lets the screen go off, but not the power) */
      if (st_power || (ui.view == 4 && pl.kind == MK_VIDEO && !player_paused()))
         last_input = t;
      else if (ui.view == 4 && !player_paused())
      {
         if (screen_on && screen_off_s > 0 && t - last_input > screen_off_s)
         {
            screen_on = 0;
            gfx_screen_power(0);
            mlog("screen off after %d s untouched (music playing)", screen_off_s);
         }
      }
      else
      {
         if (screen_on && screen_off_s > 0 && t - last_input > screen_off_s)
         {
            screen_on = 0;
            gfx_screen_power(0);
            mlog("screen off after %d s untouched", screen_off_s);
         }
         if (power_off_min > 0 && t - last_input > power_off_min * 60.0)
         {
            mlog("powering off after %d min untouched", power_off_min);
            save_state();
            touch(HALT_FLAG);
            quit = 1;
         }
      }

      /* low battery (off the charger): warn once; g36d does the shutdown */
      if (t - bat_check > 10.0)
      {
         int level = battery_level();
         bat_check = t;
         st_battery = level;
         bat_low = level >= 0 && level <= setting_int("battery_warn_percent", 20) && !external_power();
         screen_off_s = setting_int("screen_off_seconds", 180);
         power_off_min = setting_int("auto_power_off_minutes", 10);
         if (external_power())
            unlink(BAT_WARNED);
         else if (level >= 0 && level <= setting_int("battery_warn_percent", 20)
               && !prompt && !file_exists(BAT_WARNED))
         {
            touch(BAT_WARNED);
            bat_level = level;
            mlog("battery low: %d%%", level);
            if (!screen_on)
            {
               screen_on = 1;
               gfx_screen_power(1);
            }
            prompt = PROMPT_BATTERY;
            prompt_choice = 0;
            held_dir = 0;
         }
      }

      /* key repeat for the d-pad */
      if (held_dir && input_held(held_dir) && t >= repeat_at && !prompt)
      {
         repeat_at = t + 0.07;
         if (!click_volume_adjust(held_dir))
            key_click();
         if (ui.view == 1 && gc.on)
         {
            repeat_at = t + (held_dir == KEY_LEFT || held_dir == KEY_RIGHT ? 0.1 : 0.2);
            gc_nav(held_dir, t);
         }
         else if (ui.view == 1 && ui.list.n)
         {
            if (held_dir == KEY_UP && ui.sel > 0)
               ui.sel--;
            else if (held_dir == KEY_DOWN && ui.sel < ui.list.n - 1)
               ui.sel++;
         }
         else if (ui.view == 3 && mb.n)
         {
            if (held_dir == KEY_UP && mb.sel > 0)
               mb.sel--;
            else if (held_dir == KEY_DOWN && mb.sel < mb.n - 1)
               mb.sel++;
         }
         else if (ui.view == 6)
         {
            repeat_at = t + 0.12;
            search_move(held_dir);
         }
         else if (ui.view == 8)
         {
            repeat_at = t + (te.picking ? 0.05 : 0.12);
            theme_key(held_dir);
         }
         else if (ui.view == 9)
         {
            repeat_at = t + 0.12;
            gal_key(held_dir, t);
         }
         else if (ui.view == 7 && nhits && (held_dir == KEY_UP || held_dir == KEY_DOWN))
            hit_sel = held_dir == KEY_UP ? (hit_sel > 0 ? hit_sel - 1 : 0) : (hit_sel < nhits - 1 ? hit_sel + 1 : hit_sel);
         else if (ui.view == 5 && (held_dir == KEY_LEFT || held_dir == KEY_RIGHT))
         {
            repeat_at = t + 0.18;
            row_move(ROW_MEDIA, held_dir == KEY_LEFT ? -1 : 1);
         }
         else if (ui.view == 4 && vq.open)
         {
            repeat_at = t + 0.15;
            if (held_dir == KEY_UP || held_dir == KEY_DOWN)
               qm_key(held_dir, t);
         }
         else if (ui.view == 4 && (held_dir == KEY_LEFT || held_dir == KEY_RIGHT))
         {
            repeat_at = t + 0.3;
            player_seek_by(held_dir == KEY_LEFT ? -10 : 10);
            pl.overlay_until = t + 3;
         }
         else if (ui.view == 2 && !st.about)
         {
            repeat_at = t + 0.12;
            if (st.editing_clock)
               clock_edit_change(held_dir == KEY_UP ? 1 : held_dir == KEY_DOWN ? -1 : 0);
            else if (held_dir == KEY_UP || held_dir == KEY_DOWN)
               settings_move(held_dir == KEY_UP ? -1 : 1);
            else
               settings_change(held_dir == KEY_LEFT ? -1 : 1);
         }
         else if (ui.view == 0 && (held_dir == KEY_UP || held_dir == KEY_DOWN))
         {
            repeat_at = t + 0.18;   /* held: on through the rows, as Left / Right go on along one */
            ui.row_pos += held_dir == KEY_UP ? -1 : 1;
         }
         else if (ui.view == 0 && row_count(cur_row()) && (held_dir == KEY_LEFT || held_dir == KEY_RIGHT))
         {
            int r = cur_row();
            repeat_at = t + 0.18;
            row_move(r, held_dir == KEY_LEFT ? -1 : 1);
         }
      }

      media_tick(t);
      if (sound_only_req)
      {
         /* "Sound only" from the quick menu: the picture isn't decoded any more, the screen goes off,
          * the sound plays on (any button, or a tap on Power, brings the picture back where it is) */
         sound_only_req = 0;
         if (ui.view == 4 && pl.kind == MK_VIDEO)
         {
            player_set_sound_only(1);
            screen_on = 0;
            gfx_screen_power(0);
            mlog("media: sound only, screen off");
         }
      }
      else if (screen_on && ui.view == 4 && pl.kind == MK_VIDEO && player_sound_only())
      {
         player_set_sound_only(0);
         video_layout();
         /* the reopened file has a new, blank picture buffer (blank is green): until its first frame
          * is ready, show the video's picture from the list again, as when a video starts */
         pl.got_frame = 0;
         if (pl.poster && pl.vw > 0 && pl.vh > 0 && (pl.poster->w != pl.vw || pl.poster->h != pl.vh))
         {
            image_t *big = image_scaled(pl.poster, pl.vw, pl.vh);
            if (big)
            {
               image_free(pl.poster);
               pl.poster = big;
            }
         }
         pl.redraw = 1;
         mlog("media: picture back");
      }
      vthumb_pause(ui.view == 4 && pl.kind == MK_VIDEO);
      if (ui.view == 4 && pl.kind == MK_VIDEO && t - pl.stats_at > 10)
      {
         int sh, dr;
         player_stats(&sh, &dr);
         if (pl.stats_at > 0)
            mlog("media: %d frames shown, %d skipped in %.0f s (at %.0f s)", sh, dr, t - pl.stats_at, player_position());
         pl.stats_at = t;
      }
      if (ui.view == 5 && t - mf.check > 3.0)
      {
         /* new or removed folders show up on their own */
         int keep = ui.sel_[ROW_MEDIA];
         unsigned sig;
         media_folders();
         sig = media_folders_sig();
         if (sig != mf.sig)
         {
            mlog("media: %d folders, %d loose files", mf.n, mf.loose);
            row_set(ROW_MEDIA, keep < row_count(ROW_MEDIA) ? keep : 0);
            ui.anim_[ROW_MEDIA] = ui.pos_[ROW_MEDIA];
         }
         mf.sig = sig;
         mf.check = t;
      }
      if (ui.view == 3 && t - mb.check > 2.5)
      {
         unsigned sig = media_scan(1);
         if (sig != mb.sig)
            mlog("media: folder changed (%d items)", mb.n);
         mb.sig = sig;
         mb.check = t;
      }

      if (!screen_on)
      {
         pl.screen_was_on = 0;
         continue;
      }
      if (!pl.screen_was_on)
         pl.redraw = 1;   /* the screen came back: show the picture again */
      pl.screen_was_on = 1;

      /* carousel easing: each row sideways, and the rows up and down */
      for (i = 0; i <= ROW_MEDIA; i++)
      {
         ui.anim_[i] += (ui.pos_[i] - ui.anim_[i]) * 0.3;
         if (fabs(ui.pos_[i] - ui.anim_[i]) < 0.01)
            ui.anim_[i] = ui.pos_[i];
      }
      ui.row_anim += (ui.row_pos - ui.row_anim) * 0.3;
      if (fabs(ui.row_pos - ui.row_anim) < 0.01)
         ui.row_anim = ui.row_pos;
      gc_moving = ui.view == 1 ? gc_ease() : 0;   /* the games carousel's rows and cards */

      /* anything still moving (or a button held) keeps the frame rate up */
      if (show_controls && rm.capture && t > rm.capture_until)
         rm.capture = 0;   /* nothing pressed in time: the change is dropped */
      busy = ui.row_anim != ui.row_pos || held_dir || (show_controls && rm.capture) || t < ui.toast_until || t < vol_until || (ui.view == 1 && fold_sliding) ||
             (ui.view == 1 && gc.on && (gc_moving || t < gc.glint_until)) ||
             (ui.view == 0 && t < glint_until) || t < switch_until;
      for (i = 0; i <= ROW_MEDIA; i++)
         if (ui.anim_[i] != ui.pos_[i])
            busy = 1;

      /* idle on the home screen: make the cards that haven't been drawn yet, one a frame */
      prewarm_more = 0;
      if (ui.view == 0 && !busy && !prompt && !show_controls && t - last_key > 0.4)
         prewarm_more = card_prewarm();
      else if (ui.view == 1 && gc.on && !busy && !prompt && !show_controls && t - last_key > 0.25)
         prewarm_more = gcard_prewarm();

      video_mode = ui.view == 4 && pl.kind == MK_VIDEO;
      dc = c;
      {
         /* the prompt's frozen frame (see frozen_save): which prompt, and the words it shows, make its key */
         freezable = prompt && prompt != PROMPT_CORE && !show_controls && !video_mode && !timing && screen_on && !st.editing_clock;
         if (freezable)
         {
            fkey = frozen_hash(2166136261u, err_title);
            fkey = frozen_hash(fkey, err_line1);
            fkey = frozen_hash(fkey, err_line2);
            fkey = (fkey ^ (uint32_t)(prompt * 977 + pending.psx * 31 + pending.sleep * 53 + power_restart * 71 + bat_level * 7 +
                    ui.view * 131 + pick.sel * 17 + pick.sys * 257)) * 16777619u;
         }
         else
            frozen_ok = 0;
         /* (a toast or the volume bar is part of the screen below, so while one shows the screen is drawn each frame) */
         use_frozen = freezable && frozen_usable(c, fkey, t) && t >= ui.toast_until && t >= vol_until;
      }
      if (video_mode)
      {
         /* a video: the picture goes straight to the screen, with the controls on top */
         if (!(dc = media_video_screen(t, prompt || show_controls)))
            continue;
      }
      else
      {
         /* frame timing (temporary): kill -USR2 toggles logging of draw and present times */
         static double acc_d, acc_p, max_d, max_p;
         if (!screen_on)
            home_clean = 0;   /* (the canvas was blacked for the power-off: nothing of the old screen to touch up) */
         static int frames;
         double t0 = now_s(), t1, t2;
         if (use_frozen)
            frozen_restore(c);
         else if (ui.view == 0 || (ui.view == 1 && gc.on))
         {
            /* A quiet home screen (or games carousel) (nothing moving, no overlay, no button for a moment) only has the stars twinkling: the
             * patches under them are put back and the stars drawn again, instead of drawing the whole screen. The whole
             * screen is drawn again about once a second (the battery, the counts) and after any button. */
            int quiet = !busy && !prompt && !show_controls && !card_pending && screen_on;
            if (quiet && home_clean && t - home_clean_t < 2.0 && theme_stars > 0)
            {
               stars_restore(c);
               stars_draw(c, t, star_ex, star_nex);
            }
            else
            {
               card_pending = 0;
               if (ui.view == 1)
                  draw_games_carousel(c, t);
               else
                  draw_home(c, t);
               home_clean = quiet;
               home_clean_t = t;
            }
         }
         else if (ui.view == 2)
         {
            if (st.about)
               draw_about(c);
            else
               draw_settings(c, t);
            if (st.editing_clock)
               draw_clock_edit(c);
         }
         else if (ui.view == 3)
            draw_media_list(c, t);
         else if (ui.view == 5)
            draw_media_carousel(c, t);
         else if (ui.view == 6)
            draw_search(c, t);
         else if (ui.view == 7)
            draw_hits(c, t);
         else if (ui.view == 8)
            draw_theme(c, t);
         else if (ui.view == 9)
            draw_gallery(c, t);
         else if (ui.view == 4)
            draw_music(c, t);
         else if (ui.view == 1 && gc.on)
            draw_games_carousel(c, now_s());
         else
            draw_list(c, now_s());
         t1 = now_s();
         if (timing)
         {
            gfx_present();
            t2 = now_s();
            acc_d += t1 - t0;
            acc_p += t2 - t1;
            if (t1 - t0 > max_d) max_d = t1 - t0;
            if (t2 - t1 > max_p) max_p = t2 - t1;
            if (++frames == 30)
            {
               static double last_log;
               double since = last_log ? t1 - last_log : 0;
               last_log = t1;
               mlog("frames: %.0f fps, draw avg %.1f max %.1f ms, present avg %.1f max %.1f ms | per frame: background %.1f, cards %.1f (picture lookup %.1f, scaling %.1f, corners %.1f, outline %.1f), labels+bar %.1f (row name %.1f, battery %.1f, button bar %.1f)",
                     since > 0 ? 30 / since : 0.0, acc_d / 30 * 1000, max_d * 1000, acc_p / 30 * 1000, max_p * 1000, pf[PF_BG] / 30 * 1000,
                     pf[PF_CARDS] / 30 * 1000, pf[PF_IMG] / 30 * 1000, pf[PF_BLIT] / 30 * 1000, pf[PF_ROUND] / 30 * 1000,
                     pf[PF_STROKE] / 30 * 1000, pf[PF_LABELS] / 30 * 1000, pf[PF_PILL] / 30 * 1000, pf[PF_STATUS] / 30 * 1000,
                     pf[PF_HINTS] / 30 * 1000);
               memset(pf, 0, sizeof(pf));
               acc_d = acc_p = max_d = max_p = 0;
               frames = 0;
            }
            continue;   /* timing mode: home/list only, no prompts */
         }
      }
      {
         /* the still part of a prompt is drawn once and kept (frozen_save); after that only what moves is drawn */
         int pass = use_frozen ? 2 : freezable && !busy ? 1 : 0;
         for (;;)
         {
            prompt_phase = pass;
            if (prompt == PROMPT_POWER)
            {
               if (power_restart)
                  draw_prompt(dc, "Power off?", NULL, NULL, power_opts, 4, prompt_choice);
               else
                  draw_prompt(dc, "Power off?", NULL, NULL, power_opts_battery, 3, prompt_choice);
            }
            else if (prompt == PROMPT_LAUNCH_TIP)
               draw_prompt_toggle(dc, "Tip: sleep while you play", "Tap the Power button twice to put the handheld to sleep.",
                     "Hold it for 3 seconds to save and power off.",
                     "Heads up: sleeping can turn off ADB until you restart,",
                     "and a game may crash when it wakes (it saves first).", "Don't show these tips again", "Play", prompt_choice, t);
            else if (prompt == PROMPT_SLEEP)
               draw_prompt(dc, "Sleep?", "Press Power to wake. In a game this isn't real sleep:",
                     "it saves a state, then loads it back when you wake.", sleep_opts, 2, prompt_choice);
            else if (prompt == PROMPT_USB)
               draw_prompt(dc, "Computer connected", "Allow file transfer? Both drives will show up on the",
                     "computer, and you can keep playing while it's on.", usb_opts, 2, prompt_choice);
            else if (prompt == PROMPT_CORE)
               draw_picker(c);
            else if (prompt == PROMPT_ERROR)
               draw_prompt(dc, err_title, err_line1, err_line2, ok_opts, 1, prompt_choice);
            else if (prompt == PROMPT_BIOS && pending.psx && !pending.sleep)
               draw_prompt_toggle(dc, err_title, "They aren't analog. They're digital, and send the same", "inputs as the D-pad and the A, B, X, Y buttons,",
                     "so games that need two analog sticks can't be", "played properly. We know of no way to separate them.",
                     "Don't show this again", "Play", prompt_choice, t);
            else if (prompt == PROMPT_BIOS)
               draw_prompt(dc, err_title, err_line1, err_line2, pending.sleep ? sleep_state_opts : bios_opts, 2, prompt_choice);
            else if (prompt == PROMPT_THEME_RESET)
               draw_prompt(dc, "Reset to the default colours?", "Every colour goes back to how the menu came,",
                     "including any you've changed.", reset_opts, 2, prompt_choice);
            else if (prompt == PROMPT_SET_DEFAULTS)
               draw_prompt(dc, "Restore the default settings?", "Brightness, volume, the timers, the battery limits",
                     "and the rest go back to how they came.", restore_opts, 2, prompt_choice);
            else if (prompt == PROMPT_PIC_DELETE)
               draw_prompt(dc, "Delete this picture?", "It's removed from the card for good,",
                     "along with its prompt.", delete_opts, 2, prompt_choice);
            else if (prompt == PROMPT_RESET)
               draw_prompt(dc, "Reset all watch progress?", "Every folder forgets which video you were on",
                     "and where you stopped, and starts from the first.", reset_opts, 2, prompt_choice);
            if (show_controls)
               draw_controls(c);
            else if (prompt == PROMPT_CORE_HOW)
            {
               char title[96], note[160];
               snprintf(title, sizeof(title), "Use %s", core_title(pick.cores[pick.sel]));
               snprintf(note, sizeof(note), "or for every %s game?", systems[pick.sys].name);
               draw_prompt(dc, title, "Just this once, always for this game,", note, how_opts, 3, prompt_choice);
            }
            else if (prompt == PROMPT_BATTERY)
            {
               char title[48], note[96];
               snprintf(title, sizeof(title), "Battery low: %d%%", bat_level);
               snprintf(note, sizeof(note), "it saves and powers off by itself at %d%%.",
                     setting_int("battery_off_percent", 10));
               draw_prompt(dc, title, "Plug in a charger soon. If you keep playing,", note,
                     ok_opts, 1, prompt_choice);
            }
            if (pass == 1)
            {
               frozen_save(dc, fkey, t);
               pass = 2;
               continue;
            }
            break;
         }
         prompt_phase = 0;
      }
      if (want_shot)
      {
         want_shot = 0;
         save_shot(dc);
      }
      if (video_mode)
      {
         frame_forget();
         same_frames = 0;
         gfx_video_end();
      }
      else
      {
         /* a frame like the last one needn't be shown again (unless something is moving: then the page flip sets the pace) */
         if (frame_changed(dc) || busy || !screen_on)
         {
            same_frames = 0;
            gfx_present();
            if (!screen_on)
               frame_forget();   /* (the screen comes back with a frame shown) */
         }
         else
            same_frames++;
      }
   }

   media_stop(1);
   if (play_mode)
      media_return_write();
   else if (!launched)
      save_state();   /* (the Chatbot and the players too: the menu comes back where it was) */
   font_free(f_huge);
   font_free(f_big);
   font_free(f_mid);
   font_free(f_small);
   input_close();
   gfx_shutdown();
   unlink(ALIVE_FLAG);   /* quitting on purpose (a crash never gets here) */
   if (chat)
   {
      /* the chatbot takes this process's place, so all the menu's memory is its; when
       * it quits, the start-up loop starts the menu again */
      const char *app = chat == 2 ? "/usr/bin/g36-imagegen" : "/usr/bin/g36-chat";
      mlog("%s", chat == 2 ? "image generator" : "chatbot");
      signal(SIGCHLD, SIG_DFL);
      execl(app, app, (char *)NULL);
      mlog("%s didn't start: %s", app, strerror(errno));
   }
   if (play_req)
   {
      /* the player takes this process's place (like the chatbot) */
      char mode[8], card[16];
      snprintf(mode, sizeof(mode), "%d", mb.mode);
      snprintf(card, sizeof(card), "%d", ui.sel_[ROW_MEDIA]);
      mlog("player: %s", play_name);
      signal(SIGCHLD, SIG_DFL);
      execl("/proc/self/exe", "g36menu", "--play", mode, mb.base, mb.folder, play_name, card, (char *)NULL);
      mlog("player didn't start: %s", strerror(errno));
   }
   return 0;
}
