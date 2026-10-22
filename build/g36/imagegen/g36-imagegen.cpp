/*
 * g36-imagegen - the G36's Image Generator: type a prompt and the handheld draws a
 * 256x256 picture of it with Supra2-IMG, on its own processor (minutes per picture).
 * The menu closes while it runs, to leave it the memory; Select + B comes back to the
 * menu, as it leaves a game.
 *
 * The models are in /mnt/sdcard/imagegen/models/; pictures are saved as PNGs in
 * imagegen/pictures/, with each one's prompt, seed and steps in prompts.txt. The step
 * count (L / R) is kept in imagegen/imagegen.cfg, with how long a step takes here.
 */
#include "ig.h"

extern "C" {
#include "gfx.h"
#include "input.h"
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <linux/input.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#define DIR_APP    "/mnt/sdcard/imagegen"
#define DIR_MODELS DIR_APP "/models"
#define DIR_PICS   DIR_APP "/pictures"
#define CFG        DIR_APP "/imagegen.cfg"
#define THEME      "/mnt/sdcard/g36/theme.cfg"
#define FONT       "/oem/release/assets/default/raw/fonts/OpenSans_Semibold.ttf"
#define HALT_FLAG  "/tmp/halt"
#define THREADS    3
#define GUIDANCE   3.0f

/* every multiple of 5, 5 to 100 (about 40 minutes on the handheld) */
#define NCHOICES 20
#define STEPS_OF(i) (5 * ((i) + 1))

static volatile sig_atomic_t want_shot;
static void on_usr1(int) { want_shot = 1; }

static double now_s()
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec / 1e9;
}

/* kill -USR1: the screen to /tmp/g36menu-shot.ppm, as the menu does */
static void save_shot(const canvas_t *c)
{
   FILE *f = fopen("/tmp/g36menu-shot.ppm", "wb");
   if (!f)
      return;
   fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
   for (int y = 0; y < c->h; y++)
      for (int x = 0; x < c->w; x++)
      {
         uint32_t p = c->px[(size_t)y * c->stride + x];
         unsigned char rgb[3] = { (unsigned char)(p >> 16), (unsigned char)(p >> 8), (unsigned char)p };
         fwrite(rgb, 1, 3, f);
      }
   fclose(f);
}

/* ---------------------------------------------------------------- colours (the theme's) */
static uint32_t C_BG_TOP = 0xff24152b, C_BG_BOT = 0xff130b17, C_ACCENT = 0xffcd5cff, C_TEXT = 0xffffffff,
                C_DIM = 0xffab93b5, C_FAINT = 0xff6e5677, C_PANEL = 0xff291830, C_SEL = 0xff592f6b,
                C_KEY = 0xff4a2f58, C_KEY_LINE = 0xff7a5a8c, C_BAR = 0xff130b16, C_CARD_TOP = 0xff3f264b,
                C_BAR_BOX = 0xff2a1832, C_BAR_LINE = 0xff4b3158;

static void load_theme()
{
   FILE *f = fopen(THEME, "r");
   char line[200];
   static const struct { const char *k; uint32_t *v; } map[] = {
      { "background_top", &C_BG_TOP }, { "background_bottom", &C_BG_BOT }, { "highlight", &C_ACCENT },
      { "text", &C_TEXT }, { "text_dim", &C_DIM }, { "text_faint", &C_FAINT }, { "panel", &C_PANEL },
      { "selected_row", &C_SEL }, { "key", &C_KEY }, { "key_outline", &C_KEY_LINE }, { "bar", &C_BAR },
      { "card_top", &C_CARD_TOP }, { "bar_box", &C_BAR_BOX }, { "bar_box_outline", &C_BAR_LINE } };
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char k[64];
      unsigned v;
      if (sscanf(line, " %63[a-z_0-9] = %x", k, &v) == 2)
         for (auto &m : map)
            if (!strcmp(k, m.k))
               *m.v = 0xff000000 | v;
   }
   fclose(f);
}

/* ---------------------------------------------------------------- drawing (as the chatbot) */
static font_t *f_big, *f_mid, *f_small, *f_bold, *f_tiny;

static void round_rect(canvas_t *c, int x0, int y0, int w, int h, int r, uint32_t col)
{
   for (int y = std::max(y0, 0); y < y0 + h && y < c->h; y++)
      for (int x = std::max(x0, 0); x < x0 + w && x < c->w; x++)
      {
         float px = x + 0.5f, py = y + 0.5f, dx = 0, dy = 0;
         if (px < x0 + r) dx = x0 + r - px; else if (px > x0 + w - r) dx = px - (x0 + w - r);
         if (py < y0 + r) dy = y0 + r - py; else if (py > y0 + h - r) dy = py - (y0 + h - r);
         float cov = r + 0.5f - sqrtf(dx * dx + dy * dy);
         if (cov <= 0)
            continue;
         uint32_t *d = &c->px[(size_t)y * c->stride + x], o = *d, out = 0xff000000;
         int a = cov >= 1 ? 256 : (int)(cov * 256);
         for (int k = 0; k <= 16; k += 8)
            out |= (((col >> k & 0xff) * a + (o >> k & 0xff) * (256 - a)) >> 8) << k;
         *d = out;
      }
}

static void background(canvas_t *c)
{
   for (int y = 0; y < c->h; y++)
   {
      float t = (float)y / c->h;
      uint32_t col = 0xff000000;
      for (int k = 0; k <= 16; k += 8)
      {
         float a = C_BG_TOP >> k & 0xff, b = C_BG_BOT >> k & 0xff;
         col |= (uint32_t)(a + (b - a) * t) << k;
      }
      gfx_fill_rect(c, 0, y, c->w, 1, col);
   }
}

static void centered(canvas_t *c, font_t *f, const char *t, int cx, int y, uint32_t col)
{
   gfx_draw_text(c, f, t, cx - font_text_width(f, t) / 2, y, col, 0);
}

#define BTN_SHOULDER 0xff8a52b0
#define BTN_SYSTEM   0xff6a4f7e

static void ink_centered(canvas_t *c, font_t *f, const char *t, int cx, int cy, uint32_t col)
{
   int x0, y0, x1, y1;
   font_ink_box(f, t, &x0, &y0, &x1, &y1);
   gfx_draw_text(c, f, t, cx - (x0 + x1) / 2, cy - (y0 + y1) / 2, col, 0);
}

static uint32_t face_colour(char b)
{
   return b == 'A' ? 0xffd9434c : b == 'B' ? 0xffd08a16 : b == 'X' ? 0xff3a74d6 : 0xff2f9e5e;
}

static int glyph(canvas_t *c, const char *t, int x, int cy, int draw)
{
   if (strlen(t) == 1 && strchr("ABXY", t[0]))
   {
      if (draw)
      {
         round_rect(c, x, cy - 10, 20, 20, 10, face_colour(t[0]));
         ink_centered(c, f_bold, t, x + 10, cy, 0xffffffff);
      }
      return 20;
   }
   if (strlen(t) == 1 && strchr("LR", t[0]))
   {
      if (draw)
      {
         round_rect(c, x, cy - 9, 24, 18, 6, BTN_SHOULDER);
         ink_centered(c, f_bold, t, x + 12, cy, 0xffffffff);
      }
      return 24;
   }
   if (!strncmp(t, "Dpad", 4))
   {
      if (draw)
      {
         round_rect(c, x + 7, cy - 11, 8, 22, 2, 0xffffffff);
         round_rect(c, x, cy - 4, 22, 8, 2, 0xffffffff);
      }
      return 22;
   }
   int w = font_text_width(f_tiny, t) + 14;
   if (draw)
   {
      round_rect(c, x, cy - 9, w, 18, 9, BTN_SYSTEM);
      ink_centered(c, f_tiny, t, x + w / 2, cy, 0xffffffff);
   }
   return w;
}

static int glyphs(canvas_t *c, const char *key, int x, int cy, int draw)
{
   int x0 = x, first = 1;
   std::string k = key;
   size_t p = 0;
   while (p < k.size())
   {
      size_t sp = k.find(' ', p);
      std::string tok = k.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
      p = sp == std::string::npos ? k.size() : sp + 1;
      if (tok.empty())
         continue;
      if (tok == "+" || tok == "/")
      {
         if (draw)
            gfx_draw_text(c, f_small, tok.c_str(), x + 3, cy - font_height(f_small) / 2, C_DIM, 0);
         x += font_text_width(f_small, tok.c_str()) + 6;
         first = 1;
         continue;
      }
      if (!first)
         x += 3;
      x += glyph(c, tok.c_str(), x, cy, draw);
      first = 0;
   }
   return x - x0;
}

static int hint_box(canvas_t *c, const char *key, const char *what, int x, int cy)
{
   int bh = 26, by = cy - bh / 2, kw = glyphs(c, key, 0, 0, 0);
   int w = kw + 8 + font_text_width(f_small, what) + 14;
   round_rect(c, x - 1, by - 1, w + 2, bh + 2, 9, C_BAR_LINE);
   round_rect(c, x, by, w, bh, 8, C_BAR_BOX);
   glyphs(c, key, x + 4, cy, 1);
   gfx_draw_text(c, f_small, what, x + 4 + kw + 8, by + (bh - font_height(f_small)) / 2, C_DIM, 0);
   return w;
}

static void hints(canvas_t *c, const char *h)
{
   int bh = 26, by = c->h - 40 + (40 - bh) / 2, ty = by + (bh - font_height(f_small)) / 2, x = 12;
   std::string s = h;
   size_t p = 0;
   gfx_fill_rect(c, 0, c->h - 40, c->w, 40, C_BAR);
   while (p < s.size())
   {
      size_t e = s.find("     ", p);
      std::string entry = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
      p = e == std::string::npos ? s.size() : e + 5;
      while (p < s.size() && s[p] == ' ')
         p++;
      size_t sep = entry.find("  ");
      std::string key = entry.substr(0, sep), what = sep == std::string::npos ? "" : entry.substr(sep + 2);
      int kw = glyphs(c, key.c_str(), 0, 0, 0);
      int w = kw + (what.empty() ? 0 : 8 + font_text_width(f_small, what.c_str())) + 14;
      if (x + w > c->w - 12)
         break;
      round_rect(c, x - 1, by - 1, w + 2, bh + 2, 9, C_BAR_LINE);
      round_rect(c, x, by, w, bh, 8, C_BAR_BOX);
      glyphs(c, key.c_str(), x + 4, by + bh / 2, 1);
      if (!what.empty())
         gfx_draw_text(c, f_small, what.c_str(), x + 4 + kw + 8, ty, C_DIM, 0);
      x += w + 8;
   }
}

static std::vector<std::string> wrap(font_t *f, const std::string &s, int w)
{
   std::vector<std::string> out;
   std::string line;
   size_t p = 0;
   while (p < s.size())
   {
      size_t sp = s.find(' ', p);
      std::string word = s.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
      p = sp == std::string::npos ? s.size() : sp + 1;
      std::string tryl = line.empty() ? word : line + " " + word;
      if (font_text_width(f, tryl.c_str()) <= w || line.empty())
         line = tryl;
      else
      {
         out.push_back(line);
         line = word;
      }
   }
   if (!line.empty())
      out.push_back(line);
   return out;
}

/* an RGB picture (s x s) scaled smoothly onto the canvas as a w x w square */
static void draw_rgb(canvas_t *c, const uint8_t *rgb, int s, int x0, int y0, int w)
{
   for (int y = 0; y < w; y++)
   {
      if (y0 + y < 0 || y0 + y >= c->h)
         continue;
      float fy = (y + 0.5f) * s / w - 0.5f;
      int sy = (int)floorf(fy);
      float ay = fy - sy;
      int y1 = std::min(sy + 1, s - 1);
      sy = std::max(sy, 0);
      uint32_t *row = c->px + (size_t)(y0 + y) * c->stride;
      for (int x = 0; x < w; x++)
      {
         float fx = (x + 0.5f) * s / w - 0.5f;
         int sx = (int)floorf(fx);
         float ax = fx - sx;
         int x1 = std::min(sx + 1, s - 1);
         sx = std::max(sx, 0);
         uint32_t out = 0xff000000;
         for (int k = 0; k < 3; k++)
         {
            float a = rgb[(sy * s + sx) * 3 + k], b = rgb[(sy * s + x1) * 3 + k];
            float cc = rgb[(y1 * s + sx) * 3 + k], d = rgb[(y1 * s + x1) * 3 + k];
            float v = (a * (1 - ax) + b * ax) * (1 - ay) + (cc * (1 - ax) + d * ax) * ay;
            out |= (uint32_t)(v + 0.5f) << (16 - 8 * k);
         }
         row[x0 + x] = out;
      }
   }
}

/* ---------------------------------------------------------------- PNG (no compression) */
static uint32_t crc_table[256];
static uint32_t crc(const uint8_t *p, size_t n, uint32_t c = 0xffffffffu)
{
   if (!crc_table[1])
      for (uint32_t i = 0; i < 256; i++)
      {
         uint32_t v = i;
         for (int k = 0; k < 8; k++)
            v = v & 1 ? 0xedb88320u ^ (v >> 1) : v >> 1;
         crc_table[i] = v;
      }
   for (size_t i = 0; i < n; i++)
      c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
   return c;
}

static bool save_png(const char *path, const uint8_t *rgb, int w, int h)
{
   std::vector<uint8_t> raw;
   for (int y = 0; y < h; y++)
   {
      raw.push_back(0);
      raw.insert(raw.end(), rgb + (size_t)y * w * 3, rgb + (size_t)(y + 1) * w * 3);
   }
   /* zlib with stored (uncompressed) deflate blocks */
   std::vector<uint8_t> z = { 0x78, 0x01 };
   uint32_t a = 1, b = 0;
   for (uint8_t v : raw)
   {
      a = (a + v) % 65521;
      b = (b + a) % 65521;
   }
   for (size_t p = 0; p < raw.size();)
   {
      size_t n = std::min<size_t>(65535, raw.size() - p);
      z.push_back(p + n == raw.size());
      z.push_back(n & 0xff);
      z.push_back(n >> 8);
      z.push_back(~n & 0xff);
      z.push_back((~n >> 8) & 0xff);
      z.insert(z.end(), raw.begin() + p, raw.begin() + p + n);
      p += n;
   }
   uint32_t ad = (b << 16) | a;
   for (int k = 3; k >= 0; k--)
      z.push_back(ad >> (8 * k));
   FILE *f = fopen(path, "wb");
   if (!f)
      return false;
   auto be32 = [&](uint32_t v) {
      uint8_t q[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
      fwrite(q, 1, 4, f);
   };
   auto chunk = [&](const char *type, const std::vector<uint8_t> &d) {
      be32((uint32_t)d.size());
      std::vector<uint8_t> td(type, type + 4);
      td.insert(td.end(), d.begin(), d.end());
      fwrite(td.data(), 1, td.size(), f);
      be32(crc(td.data(), td.size()) ^ 0xffffffffu);
   };
   static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
   fwrite(sig, 1, 8, f);
   std::vector<uint8_t> ihdr = { (uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
      (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0 };
   chunk("IHDR", ihdr);
   chunk("IDAT", z);
   chunk("IEND", {});
   return fclose(f) == 0;
}

/* ---------------------------------------------------------------- the keyboard (as the chatbot) */
#define KB_SPACE 1
#define KB_DEL   2
#define KB_MAKE  3
#define KB_SYM   4
#define KB_COLS  13
#define KB_PAD   10
static const char kb[4][KB_COLS + 1] = {
   "QWERTYUIOP789", "ASDFGHJKL\002456", "ZXCVBNM\003\003\003123", "\001\001\001\001\001\001\001\001\004\004000" };
static const char kb_sym_rows[4][KB_COLS + 1] = {
   "!?.,'\":;()789", "-_=+*/&@#\002456", "$%<>[]~\003\003\003123", "\001\001\001\001\001\001\001\001\004\004000" };
static int kb_sym;   /* the symbols page is showing */
static int kkey(int r, int c) { return (unsigned char)(kb_sym && r < 3 ? kb_sym_rows : kb)[r][c]; }
static int kfirst(int r, int c)
{
   int k = kkey(r, c);
   while (c > 0 && kkey(r, c - 1) == k && (k < '0' || r == 3) && (c - 1 >= KB_PAD) == (c >= KB_PAD))
      c--;
   return c;
}
static int kspan(int r, int c)
{
   int k = kkey(r, c), f = kfirst(r, c), n = 1;
   while (f + n < KB_COLS && kkey(r, f + n) == k && (k < '0' || r == 3) && (f + n >= KB_PAD) == (f >= KB_PAD))
      n++;
   return n;
}

/* ---------------------------------------------------------------- making a picture (a thread) */
enum { ST_IDLE, ST_TEXT, ST_LOAD, ST_STEPS, ST_DECODE, ST_DONE, ST_FAILED };
static std::mutex mtx;
static std::atomic<int> state{ ST_IDLE }, step_done{ 0 };
static std::atomic<bool> cancel{ false };
static uint8_t preview[256 * 256 * 3], picture[256 * 256 * 3];
static std::atomic<bool> preview_new{ false };
static std::string fail_msg;
static double step_secs = 24.0;   /* how long one step takes here (measured, kept in the cfg) */

static void make(ig_t *ig, std::string prompt, int steps, uint32_t seed)
{
   state = ST_TEXT;
   step_done = 0;
   if (ig_encode(ig, prompt.c_str()) < 0)
   {
      fail_msg = ig_error(ig);
      state = ST_FAILED;
      return;
   }
   if (cancel)
   {
      state = ST_IDLE;
      return;
   }
   state = ST_LOAD;
   if (ig_begin(ig, steps, GUIDANCE, seed) < 0)
   {
      fail_msg = ig_error(ig);
      state = ST_FAILED;
      return;
   }
   state = ST_STEPS;
   double t0 = now_s();
   for (int i = 0; i < steps; i++)
   {
      if (cancel)
      {
         state = ST_IDLE;
         return;
      }
      if (ig_step(ig) < 0)
      {
         fail_msg = "A step failed (out of memory?).";
         state = ST_FAILED;
         return;
      }
      step_done = i + 1;
      {
         std::lock_guard<std::mutex> l(mtx);
         ig_preview(ig, preview);
      }
      preview_new = true;
   }
   step_secs = (now_s() - t0) / steps;
   state = ST_DECODE;
   if (ig_decode(ig, picture) < 0)
   {
      fail_msg = ig_error(ig);
      state = ST_FAILED;
      return;
   }
   state = ST_DONE;
}

/* ---------------------------------------------------------------- the app */
static void load_cfg(int &choice)
{
   FILE *f = fopen(CFG, "r");
   char line[200];
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      int v;
      double d;
      if (sscanf(line, "steps = %d", &v) == 1)
         for (int i = 0; i < NCHOICES; i++)
            if (STEPS_OF(i) == v)
               choice = i;
      if (sscanf(line, "seconds_per_step = %lf", &d) == 1 && d > 0.5 && d < 600)
         step_secs = d;
   }
   fclose(f);
}

static void save_cfg(int choice)
{
   FILE *f = fopen(CFG ".tmp", "w");
   if (!f)
      return;
   fprintf(f, "# G36 Image Generator\nsteps = %d\nseconds_per_step = %.1f\n", STEPS_OF(choice), step_secs);
   fclose(f);
   rename(CFG ".tmp", CFG);
}

static std::string minutes(double s)
{
   char b[48];
   if (s < 90)
      snprintf(b, sizeof(b), "%d s", (int)(s + 0.5));
   else
      snprintf(b, sizeof(b), "%d min", (int)(s / 60 + 0.5));
   return b;
}

int main()
{
   if (gfx_init() < 0)
      return 1;
   signal(SIGUSR1, on_usr1);
   input_open();
   canvas_t *c = gfx_canvas();
   f_big = font_load(FONT, 30);
   f_mid = font_load(FONT, 20);
   f_small = font_load(FONT, 16);
   f_bold = font_load_bold(FONT, 18);
   f_tiny = font_load_bold(FONT, 12);
   load_theme();
   mkdir(DIR_APP, 0755);
   mkdir(DIR_PICS, 0755);
   int choice = 3;   /* 20 steps */
   load_cfg(choice);
   ig_t *ig = ig_new(DIR_MODELS, THREADS);
   bool have_models = access(DIR_MODELS "/dit.bin", R_OK) == 0 && access(DIR_MODELS "/t5.bin", R_OK) == 0 &&
         access(DIR_MODELS "/taesd.bin", R_OK) == 0 && access(DIR_MODELS "/spiece.model", R_OK) == 0;

   std::string typed, prompt, saved_as;
   int kr = 0, kc = 0, quit = 0, sel_down = 0, sel_used = 0, held = 0, steps_now = 0;
   uint32_t seed = 0;
   double held_at = 0, power_down = 0, started = 0, last_draw = 0;
   std::thread worker;
   bool showing = false;   /* the finished picture is up (or a failure) */

   auto start = [&]() {
      if (typed.empty() || state != ST_IDLE || !have_models)
         return;
      prompt = typed;
      save_cfg(choice);   /* (the step count, which may have been run through with a held button) */
      seed = (uint32_t)time(nullptr) * 2654435761u ^ (uint32_t)(now_s() * 1000);
      steps_now = STEPS_OF(choice);
      cancel = false;
      memset(preview, 0, sizeof(preview));
      started = now_s();
      saved_as.clear();
      if (worker.joinable())
         worker.join();
      worker = std::thread(make, ig, prompt, steps_now, seed);
   };

   auto draw = [&]() {
      background(c);
      gfx_fill_rect(c, 0, 0, c->w, 46, 0x60000000);
      gfx_draw_text(c, f_big, "Image Generator", 18, (46 - font_height(f_big)) / 2, C_TEXT, 0);
      {
         int kw = glyphs(c, "Select + B", 0, 0, 0), w = kw + 8 + font_text_width(f_small, "Leave") + 14;
         hint_box(c, "Select + B", "Leave", c->w - 18 - w, 23);
      }
      int st = state;
      if (st == ST_IDLE && !showing)
      {
         /* the prompt and the keyboard */
         if (!have_models)
         {
            centered(c, f_mid, "The image models are missing.", c->w / 2, 70, C_DIM);
            centered(c, f_small, "They go in imagegen/models/ on G36-GAMES.", c->w / 2, 100, C_FAINT);
         }
         else
         {
            centered(c, f_mid, "Describe a picture, and the handheld will draw it.", c->w / 2, 58, C_DIM);
            centered(c, f_small, "It's tiny and slow: a 256 x 256 picture takes minutes. Plain descriptions work best.",
                  c->w / 2, 86, C_FAINT);
         }
         round_rect(c, 23, 118, c->w - 46, 34, 10, C_ACCENT);
         round_rect(c, 25, 120, c->w - 50, 30, 8, C_PANEL);
         if (typed.empty())
            gfx_draw_text(c, f_mid, "a red fox in a snowy forest", 40, 120 + (30 - font_height(f_mid)) / 2, C_FAINT, 0);
         else
         {
            int w = gfx_draw_text(c, f_mid, typed.c_str(), 40, 120 + (30 - font_height(f_mid)) / 2, C_TEXT, c->w - 100);
            if (fmod(now_s(), 1.0) < 0.6)
               gfx_fill_rect(c, 42 + w, 126, 2, 18, C_ACCENT);
         }
         /* the steps: L / R */
         {
            char b[120];
            int n = STEPS_OF(choice);
            snprintf(b, sizeof(b), "%d steps   (about %s)", n, minutes(n * step_secs + 50).c_str());
            int y = 166, w = font_text_width(f_mid, b) + 120, x = (c->w - w) / 2;
            round_rect(c, x, y, w, 34, 17, C_BAR_BOX);
            glyph(c, "L", x + 10, y + 17, 1);
            glyph(c, "R", x + w - 34, y + 17, 1);
            centered(c, f_mid, b, c->w / 2, y + (34 - font_height(f_mid)) / 2, C_TEXT);
            centered(c, f_small, n <= 15 ? "Fewer steps: quicker, rougher." : n >= 50 ? "More steps: slower, a little finer."
                  : "A good balance.", c->w / 2, 206, C_FAINT);
         }
         int kw = 55, kh = 26, gap = 4, pad = 14, kx0 = (c->w - (KB_COLS * kw + (KB_COLS - 1) * gap + pad)) / 2, ky0 = 300;
         for (int r = 0; r < 4; r++)
            for (int k = 0; k < KB_COLS; k++)
            {
               if (kfirst(r, k) != k)
                  continue;
               int key = kkey(r, k), span = kspan(r, k), x = kx0 + k * (kw + gap) + (k >= KB_PAD ? pad : 0);
               int y = ky0 + r * (kh + gap), w = span * kw + (span - 1) * gap, sel = r == kr && kfirst(kr, kc) == k;
               char ch[2] = { (char)key, 0 };
               const char *label = key == KB_SPACE ? "Space" : key == KB_DEL ? "Del" : key == KB_MAKE ? "Make"
                     : key == KB_SYM ? (kb_sym ? "ABC" : "Symbols") : ch;
               round_rect(c, x - 1, y - 1, w + 2, kh + 2, 8, sel ? 0xffe2a8ff : C_KEY_LINE);
               round_rect(c, x, y, w, kh, 7, sel ? C_ACCENT : C_KEY);
               centered(c, f_small, label, x + w / 2, y + (kh - font_height(f_small)) / 2, C_TEXT);
            }
         hints(c, "A  Type     B  Delete     Y  Space     L / R  Steps     Start  Make");
      }
      else
      {
         /* the picture: the preview while it's made, then the real thing */
         int px = 30, py = 58, pw = 374;
         round_rect(c, px - 3, py - 3, pw + 6, pw + 6, 10, C_ACCENT);
         {
            std::lock_guard<std::mutex> l(mtx);
            draw_rgb(c, st == ST_DONE ? picture : preview, 256, px, py, pw);
         }
         int x = px + pw + 34, w = c->w - x - 30, y = 64;
         for (auto &ln : wrap(f_mid, "\"" + prompt + "\"", w))
         {
            gfx_draw_text(c, f_mid, ln.c_str(), x, y, C_TEXT, 0);
            y += font_height(f_mid) + 2;
            if (y > 170)
               break;
         }
         y += 14;
         char b[160];
         double el = now_s() - started;
         const char *what = st == ST_TEXT ? "Reading the prompt..." : st == ST_LOAD ? "Loading the picture model..."
               : st == ST_STEPS ? "Drawing..." : st == ST_DECODE ? "Finishing the picture..."
               : st == ST_DONE ? "Done." : st == ST_FAILED ? "It didn't work." : "Stopped.";
         gfx_draw_text(c, f_mid, what, x, y, st == ST_FAILED ? 0xffff5a5a : C_ACCENT, 0);
         y += 34;
         if (st == ST_STEPS || st == ST_TEXT || st == ST_LOAD || st == ST_DECODE)
         {
            int d = step_done;
            snprintf(b, sizeof(b), "Step %d of %d", d, steps_now);
            gfx_draw_text(c, f_small, b, x, y, C_DIM, 0);
            y += 26;
            gfx_fill_rect(c, x, y, w, 8, C_BAR_BOX);
            gfx_fill_rect(c, x, y, w * d / std::max(1, steps_now), 8, C_ACCENT);
            y += 22;
            double left = (steps_now - d) * step_secs + (st == ST_DECODE ? 20 : 40);
            snprintf(b, sizeof(b), "%s so far, about %s to go", minutes(el).c_str(), minutes(left).c_str());
            gfx_draw_text(c, f_small, b, x, y, C_FAINT, 0);
            y += 40;
            gfx_draw_text(c, f_small, "The picture sharpens as it goes. You can put", x, y, C_FAINT, 0);
            gfx_draw_text(c, f_small, "the handheld down; it keeps going.", x, y + 22, C_FAINT, 0);
            hints(c, "Select + X  Stop");
         }
         else if (st == ST_DONE)
         {
            snprintf(b, sizeof(b), "%d steps in %s, seed %u", steps_now, minutes(el).c_str(), seed);
            gfx_draw_text(c, f_small, b, x, y, C_DIM, 0);
            y += 26;
            if (!saved_as.empty())
            {
               gfx_draw_text(c, f_small, "Saved in imagegen/pictures/ as", x, y, C_FAINT, 0);
               gfx_draw_text(c, f_small, saved_as.c_str(), x, y + 22, C_FAINT, w);
            }
            hints(c, "A  Another (new seed)     B  Change the prompt");
         }
         else
         {
            if (st == ST_FAILED)
               for (auto &ln : wrap(f_small, fail_msg, w))
               {
                  gfx_draw_text(c, f_small, ln.c_str(), x, y, C_DIM, 0);
                  y += 22;
               }
            hints(c, "A  Try again     B  Change the prompt");
         }
      }
      if (want_shot)
      {
         want_shot = 0;
         save_shot(c);
      }
      gfx_present();
      last_draw = now_s();
   };

   while (!quit)
   {
      key_event_t ev;
      int st = state;
      bool working = st == ST_TEXT || st == ST_LOAD || st == ST_STEPS || st == ST_DECODE;
      while (input_next(&ev, working ? 250 : 60))
      {
         double t = now_s();
         if (ev.code == KEY_POWER)
         {
            power_down = ev.value ? (power_down ? power_down : t) : 0;
            continue;
         }
         if (ev.code == BTN_SELECT)
         {
            if (ev.value == 1)
               sel_down = 1, sel_used = 0;
            else if (ev.value == 0)
               sel_down = 0;
            continue;
         }
         if (ev.value == 0)
         {
            if (ev.code == held)
               held = 0;
            continue;
         }
         if (ev.code == BTN_EAST && sel_down)
         {
            sel_used = 1;
            quit = 1;   /* Select + B: back to the menu, like leaving a game */
            continue;
         }
         if (ev.value != 1)
            continue;
         st = state;
         if (st == ST_TEXT || st == ST_LOAD || st == ST_STEPS || st == ST_DECODE)
         {
            if (ev.code == BTN_NORTH && sel_down)
            {
               sel_used = 1;
               cancel = true;   /* Select + X: stop (a single button is too easy to hit) */
            }
            continue;
         }
         if (showing || st == ST_DONE || st == ST_FAILED)
         {
            if (ev.code == BTN_SOUTH)
            {
               state = ST_IDLE;
               showing = false;
               typed = prompt;
               start();
               showing = true;
            }
            else if (ev.code == BTN_EAST)
            {
               state = ST_IDLE;
               showing = false;
               typed = prompt;
            }
            continue;
         }
         if (ev.code == KEY_UP || ev.code == KEY_DOWN || ev.code == KEY_LEFT || ev.code == KEY_RIGHT ||
               ev.code == BTN_TL || ev.code == BTN_TR)
         {
            held = ev.code;
            held_at = t + 0.35;
         }
         switch (ev.code)
         {
         case KEY_UP: kr = (kr + 3) % 4; break;
         case KEY_DOWN: kr = (kr + 1) % 4; break;
         case KEY_LEFT: kc = (kfirst(kr, kc) + KB_COLS - 1) % KB_COLS; break;
         case KEY_RIGHT: kc = (kfirst(kr, kc) + kspan(kr, kc)) % KB_COLS; break;
         case BTN_TL:
            choice = std::max(0, choice - 1);
            save_cfg(choice);
            break;
         case BTN_TR:
            choice = std::min(NCHOICES - 1, choice + 1);
            save_cfg(choice);
            break;
         case BTN_SOUTH:
         {
            int k = kkey(kr, kc);
            if (k == KB_SYM)
               kb_sym ^= 1;
            else if (k == KB_MAKE)
            {
               start();
               showing = state != ST_IDLE || worker.joinable();
            }
            else if (k == KB_DEL)
            {
               if (!typed.empty())
                  typed.pop_back();
            }
            else if (typed.size() < 300 && !(k == KB_SPACE && (typed.empty() || typed.back() == ' ')))
               typed += k == KB_SPACE ? ' ' : (char)tolower(k);
            break;
         }
         case BTN_WEST:
            if (!typed.empty() && typed.back() != ' ' && typed.size() < 300)
               typed += ' ';
            break;
         case BTN_EAST:
            if (!typed.empty())
               typed.pop_back();
            break;
         case BTN_START:
            start();
            showing = true;
            break;
         }
      }
      double t = now_s();
      st = state;
      if (held && input_held(held) && t >= held_at && st == ST_IDLE && !showing)
      {
         held_at = t + 0.12;
         if (held == KEY_LEFT) kc = (kfirst(kr, kc) + KB_COLS - 1) % KB_COLS;
         else if (held == KEY_RIGHT) kc = (kfirst(kr, kc) + kspan(kr, kc)) % KB_COLS;
         else if (held == KEY_UP) kr = (kr + 3) % 4;
         else if (held == KEY_DOWN) kr = (kr + 1) % 4;
         else if (held == BTN_TL && choice > 0)
            choice--;
         else if (held == BTN_TR && choice < NCHOICES - 1)
            choice++;
      }
      if ((st == ST_DONE || st == ST_FAILED || (st == ST_IDLE && showing)) && worker.joinable())
      {
         worker.join();
         if (st == ST_IDLE)
            showing = false;   /* stopped: back to the prompt */
         if (st == ST_DONE)
         {
            /* keep it: a PNG, and its prompt in prompts.txt */
            char name[64], path[300];
            time_t now = time(nullptr);
            struct tm tm;
            localtime_r(&now, &tm);
            strftime(name, sizeof(name), "%Y-%m-%d %H-%M-%S", &tm);
            snprintf(path, sizeof(path), DIR_PICS "/%s.png", name);
            if (save_png(path, picture, 256, 256))
            {
               saved_as = std::string(name) + ".png";
               FILE *f = fopen(DIR_PICS "/prompts.txt", "a");
               if (f)
               {
                  fprintf(f, "%s.png\t%s\tseed %u\t%d steps\n", name, prompt.c_str(), seed, steps_now);
                  fclose(f);
               }
               sync();
            }
            save_cfg(choice);   /* with the measured speed */
         }
      }
      if (power_down && t - power_down > 1.5)
      {
         FILE *f = fopen(HALT_FLAG, "w");
         if (f)
            fclose(f);
         quit = 1;
      }
      /* while it works, the screen is drawn a few times a second (the processor is busy) */
      bool busy_now = st == ST_TEXT || st == ST_LOAD || st == ST_STEPS || st == ST_DECODE;
      if (!busy_now || preview_new || t - last_draw > 0.5)
      {
         preview_new = false;
         draw();
      }
   }
   /* leaving: a picture being made is simply dropped (nothing to save) */
   input_close();
   gfx_shutdown();
   _exit(0);
}
