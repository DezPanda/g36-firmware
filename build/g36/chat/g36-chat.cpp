/*
 * g36-chat - the G36's (deliberately terrible) chatbot: a tiny language model run on the
 * handheld itself with llama.cpp. The menu closes while it runs, to leave it the memory;
 * Select + B comes back to the menu, as it leaves a game.
 *
 * Models are the .gguf files in /mnt/sdcard/chatbot/; Select switches between them and
 * the choice is kept in chatbot/chatbot.cfg. Colours follow the menu's theme.
 */
#include "llama.h"

extern "C" {
#include "gfx.h"
#include "input.h"
}

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <dirent.h>
#include <linux/input.h>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <algorithm>
#include <vector>

#define DIR_MODELS "/mnt/sdcard/chatbot"
#define CFG        DIR_MODELS "/chatbot.cfg"
#define THEME      "/mnt/sdcard/g36/theme.cfg"
#define FONT       "/oem/release/assets/default/raw/fonts/OpenSans_Semibold.ttf"
#define HALT_FLAG  "/tmp/halt"
#define THREADS    3
#define N_CTX      2048
#define MAX_REPLY  600            /* tokens: a safety net; repeating stops it sooner */
#define MAX_THINK  1100           /* tokens, for a reasoning model (its thinking counts) */
#define THINK_SOFT 300            /* thinking this long gets nudged to wrap up (not cut off) */
#define LOOP_N     12             /* this many tokens seen twice before, in a row: it's looping */

#define CHAT_LOG   DIR_MODELS "/last-chat.txt"
static volatile sig_atomic_t want_shot;
static void on_usr1(int) { want_shot = 1; }

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

/* the conversation as plain text (the reply exactly as the model wrote it) */
static void log_start(const std::string &model)
{
   FILE *f = fopen(CHAT_LOG, "w");
   if (f)
   {
      fprintf(f, "Chat with %s\n\n", model.c_str());
      fclose(f);
   }
}
static void log_add(const char *who, const std::string &t)
{
   FILE *f = fopen(CHAT_LOG, "a");
   if (f)
   {
      fprintf(f, "%s: %s\n\n", who, t.c_str());
      fclose(f);
   }
}

static double now_s()
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec / 1e9;
}

/* ---------------------------------------------------------------- colours */
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

/* ---------------------------------------------------------------- drawing helpers */
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

/* ---- the hint bar, as in the menu: each entry in a box, the buttons drawn as buttons */
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
   if (!strcmp(t, "Dpad"))
   {
      if (draw)
      {
         round_rect(c, x + 7, cy - 9, 6, 19, 2, 0xffffffff);
         round_rect(c, x + 1, cy - 3, 18, 6, 2, 0xffffffff);
      }
      return 21;
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

/* one boxed hint ("Select + B" / "Leave") with its left edge at x; returns its width */
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

/* "A  Type     B  Delete ..." (entries five spaces apart, button and meaning two) */
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


/* text broken into lines no wider than w */
static std::vector<std::string> wrap(font_t *f, const std::string &s, int w)
{
   std::vector<std::string> out;
   size_t i = 0;
   while (i < s.size())
   {
      size_t nl = s.find('\n', i);
      std::string para = s.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
      i = nl == std::string::npos ? s.size() : nl + 1;
      std::string line;
      size_t p = 0;
      if (para.empty())
         out.push_back("");
      while (p < para.size())
      {
         size_t sp = para.find(' ', p);
         std::string word = para.substr(p, sp == std::string::npos ? std::string::npos : sp - p);
         p = sp == std::string::npos ? para.size() : sp + 1;
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
   }
   return out;
}

static void build_words(const llama_vocab *v);

/* No facts about the handheld (tiny models fixate on whatever they're told); just a
 * nudge that keeps weak models plainer and less likely to make things up. */
static std::string facts(const std::string &)
{
   return "You are a friendly assistant. Answer in plain words, as long as the question needs. "
          "If you don't know something, say so instead of making it up.";
}

/* ---------------------------------------------------------------- the model */
struct Bot
{
   llama_model *model = nullptr;
   llama_context *ctx = nullptr;
   llama_sampler *smpl = nullptr;
   const llama_vocab *vocab = nullptr;
   std::vector<llama_chat_message> msgs;
   bool reasoning = false;
   std::vector<char> formatted;
   int prev_len = 0;
   std::string name;

   void unload()
   {
      for (auto &m : msgs)
         free((char *)m.content);
      msgs.clear();
      prev_len = 0;
      if (smpl) llama_sampler_free(smpl);
      if (ctx) llama_free(ctx);
      if (model) llama_model_free(model);
      smpl = nullptr; ctx = nullptr; model = nullptr;
   }

   bool load(const std::string &path)
   {
      unload();
      llama_model_params mp = llama_model_default_params();
      mp.n_gpu_layers = 0;
      model = llama_model_load_from_file(path.c_str(), mp);
      if (!model)
         return false;
      vocab = llama_model_get_vocab(model);
      llama_context_params cp = llama_context_default_params();
      cp.n_ctx = N_CTX;
      cp.n_batch = N_CTX;
      cp.n_threads = cp.n_threads_batch = THREADS;
      ctx = llama_init_from_model(model, cp);
      if (!ctx)
         return false;
      smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
      llama_sampler_chain_add(smpl, llama_sampler_init_min_p(0.05f, 1));
      llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.8f));
      llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));
      formatted.assign(llama_n_ctx(ctx), 0);
      build_words(vocab);
      return true;
   }

   /* forget the conversation (the model stays loaded) */
   void clear()
   {
      for (auto &m : msgs)
         free((char *)m.content);
      msgs.clear();
      prev_len = 0;
      llama_memory_clear(llama_get_memory(ctx), true);
   }

   int apply(bool add_ass)
   {
      const char *tmpl = llama_model_chat_template(model, nullptr);
      int n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), add_ass, formatted.data(), formatted.size());
      if (n > (int)formatted.size())
      {
         formatted.resize(n);
         n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), add_ass, formatted.data(), formatted.size());
      }
      return n;
   }
};

/* Words for autocomplete: the model's own whole-word tokens (" hello"), in token order,
 * which for these tokenizers is roughly most common first. */
static std::vector<std::string> words;
static void build_words(const llama_vocab *v)
{
   words.clear();
   int n = llama_vocab_n_tokens(v);
   for (int i = 0; i < n; i++)
   {
      char buf[64];
      int len = llama_token_to_piece(v, i, buf, sizeof(buf) - 1, 0, false);
      if (len < 3 || len > 16 || buf[0] != ' ')
         continue;
      buf[len] = 0;
      bool ok = true;
      for (int k = 1; k < len; k++)
         if (!((buf[k] >= 'a' && buf[k] <= 'z') || buf[k] == '\''))
            ok = false;
      if (ok)
         words.push_back(buf + 1);
   }
}

/* the reply being written, shared with the screen */
static std::mutex mtx;
static std::string reply;
static std::atomic<bool> busy{false}, stop_req{false};

static void generate(Bot *b, std::string user)
{
   std::string out;
   if (b->msgs.empty())
      b->msgs.push_back({ "system", strdup(facts(b->name).c_str()) });
   b->msgs.push_back({ "user", strdup(user.c_str()) });
   int n = b->apply(true);
   if (n < 0)
   {
      std::lock_guard<std::mutex> l(mtx);
      reply = "(the model has no chat template)";
      busy = false;
      return;
   }
   {
      /* Nearly full: forget the oldest exchanges (down to about 60%, so it doesn't happen
       * every message) and read what's left again from the start. */
      auto count = [&](int len) {
         return -llama_tokenize(b->vocab, b->formatted.data(), len, NULL, 0, true, true);
      };
      int limit = N_CTX - (b->reasoning ? MAX_THINK : MAX_REPLY) - 64;
      if (count(n) > limit && b->msgs.size() > 1)
      {
         while (b->msgs.size() > 2 && count(n) > limit * 6 / 10)
         {
            free((char *)b->msgs[1].content);   /* the oldest after the facts */
            b->msgs.erase(b->msgs.begin() + 1);
            n = b->apply(true);
         }
         llama_memory_clear(llama_get_memory(b->ctx), true);
         b->prev_len = 0;
      }
   }
   std::string prompt(b->formatted.begin() + b->prev_len, b->formatted.begin() + n);
   bool first = llama_memory_seq_pos_max(llama_get_memory(b->ctx), 0) == -1;
   int np = -llama_tokenize(b->vocab, prompt.c_str(), prompt.size(), NULL, 0, first, true);
   std::vector<llama_token> toks(np);
   llama_tokenize(b->vocab, prompt.c_str(), prompt.size(), toks.data(), toks.size(), first, true);
   llama_batch batch = llama_batch_get_one(toks.data(), toks.size());
   llama_token tok;
   int made = 0;
   std::vector<llama_token> gen;   /* what it's written, to spot it going round in circles */
   int think_tokens = 0;           /* tokens since <think>, while it's open */
   bool thinking = false, nudged = false;
   /* a reasoning model that's thought long enough (or gone round in circles while
    * thinking) is handed "that's enough, time to answer" and carries on from there */
   auto wrap_up = [&]() {
      const char *t = "\nOkay, I've thought about this enough. Time to answer.\n</think>\n\n";
      int nt = -llama_tokenize(b->vocab, t, strlen(t), NULL, 0, false, true);
      std::vector<llama_token> tk(nt);
      llama_tokenize(b->vocab, t, strlen(t), tk.data(), nt, false, true);
      llama_batch nb = llama_batch_get_one(tk.data(), nt);
      if (llama_decode(b->ctx, nb))
         return false;
      out += t;
      thinking = false;
      nudged = true;
      gen.clear();
      std::lock_guard<std::mutex> l(mtx);
      reply = out;
      return true;
   };
   for (;;)
   {
      int used = llama_memory_seq_pos_max(llama_get_memory(b->ctx), 0) + 1;
      if (used + batch.n_tokens > (int)llama_n_ctx(b->ctx))
      {
         out += "\n(I've run out of memory for this chat. Press X to start a new one.)";
         break;
      }
      if (llama_decode(b->ctx, batch))
         break;
      tok = llama_sampler_sample(b->smpl, b->ctx, -1);
      if (llama_vocab_is_eog(b->vocab, tok) || stop_req || ++made > (b->reasoning ? MAX_THINK : MAX_REPLY))
         break;
      gen.push_back(tok);
      if (thinking)
         think_tokens++;
      bool looping = false;
      if ((int)gen.size() >= LOOP_N * 3)
      {
         /* the last LOOP_N tokens already appeared twice: stop */
         int seen = 0, n = (int)gen.size();
         for (int st = 0; st + LOOP_N <= n - LOOP_N && seen < 2; st++)
            if (std::equal(gen.end() - LOOP_N, gen.end(), gen.begin() + st))
               seen++;
         looping = seen >= 2;
      }
      if (thinking && !nudged && (looping || think_tokens >= THINK_SOFT))
      {
         /* finish the thought instead of stopping: this token is dropped, the nudge
          * goes in, and the next token comes after it */
         if (!wrap_up())
            break;
         tok = llama_sampler_sample(b->smpl, b->ctx, -1);
         if (llama_vocab_is_eog(b->vocab, tok))
            break;
         gen.push_back(tok);
      }
      else if (looping)
         break;
      char buf[256];
      int len = llama_token_to_piece(b->vocab, tok, buf, sizeof(buf), 0, true);
      if (len > 0)
      {
         out.append(buf, len);
         if (!thinking && !nudged && out.find("<think>") != std::string::npos && out.find("</think>") == std::string::npos)
            thinking = true;
         if (thinking && out.find("</think>") != std::string::npos)
            thinking = false;
         std::lock_guard<std::mutex> l(mtx);
         reply = out;
      }
      batch = llama_batch_get_one(&tok, 1);
   }
   {
      std::string kept = out;
      size_t e = kept.find("</think>");
      if (e != std::string::npos)
         kept = kept.substr(e + 8);
      else if (kept.find("<think>") != std::string::npos)
         kept = "";   /* still thinking when it stopped */
      b->msgs.push_back({ "assistant", strdup(kept.c_str()) });
   }
   b->prev_len = b->apply(false);
   if (b->prev_len < 0)
      b->prev_len = 0;
   {
      std::lock_guard<std::mutex> l(mtx);
      reply = out;
   }
   busy = false;
}

/* ---------------------------------------------------------------- the keyboard */
#define KB_SPACE 1
#define KB_DEL   2
#define KB_SEND  3
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

/* ---------------------------------------------------------------- autocomplete */
#define NSUGG 4
static std::vector<std::string> suggest(const std::string &typed, const std::vector<std::string> &chatwords)
{
   std::vector<std::string> out;
   size_t sp = typed.rfind(' ');
   std::string part = sp == std::string::npos ? typed : typed.substr(sp + 1);
   if (part.empty())
      return out;
   auto add = [&](const std::string &w) {
      if (w.size() <= part.size() || w.compare(0, part.size(), part) || out.size() >= NSUGG)
         return;
      for (auto &o : out)
         if (o == w)
            return;
      out.push_back(w);
   };
   for (auto it = chatwords.rbegin(); it != chatwords.rend(); ++it)
      add(*it);
   for (auto &w : words)
   {
      if (out.size() >= NSUGG)
         break;
      add(w);
   }
   return out;
}

/* the words used so far in the chat (lower case letters only) */
static void chat_words(const std::string &t, std::vector<std::string> &v)
{
   std::string w;
   for (size_t i = 0; i <= t.size(); i++)
   {
      char ch = i < t.size() ? (char)tolower((unsigned char)t[i]) : ' ';
      if ((ch >= 'a' && ch <= 'z') || ch == '\'')
         w += ch;
      else
      {
         if (w.size() >= 3)
            v.push_back(w);
         w.clear();
      }
   }
}

static std::string accept(const std::string &typed, const std::string &w)
{
   size_t sp = typed.rfind(' ');
   return (sp == std::string::npos ? "" : typed.substr(0, sp + 1)) + w + " ";
}

/* ---------------------------------------------------------------- the app */
struct Line { bool user; std::string text; };

/* the models: chatbot/*.gguf (chat), then chatbot/reasoning/*.gguf (paths inside chatbot/) */
static std::vector<std::string> list_dir(const std::string &sub)
{
   std::vector<std::string> v;
   DIR *d = opendir((std::string(DIR_MODELS) + (sub.empty() ? "" : "/" + sub)).c_str());
   if (!d)
      return v;
   while (dirent *e = readdir(d))
   {
      std::string n = e->d_name;
      if (n[0] != '.' && n.size() > 5 && n.substr(n.size() - 5) == ".gguf")
         v.push_back(sub.empty() ? n : sub + "/" + n);
   }
   closedir(d);
   std::sort(v.begin(), v.end());
   return v;
}

static int nchat;   /* the first nchat models are chat models, the rest reasoning */
static std::vector<std::string> list_models()
{
   std::vector<std::string> v = list_dir(""), r = list_dir("reasoning");
   nchat = (int)v.size();
   v.insert(v.end(), r.begin(), r.end());
   return v;
}

static bool is_reasoning(const std::string &m)
{
   return m.compare(0, 10, "reasoning/") == 0;
}

/* "Falcon-H1-Tiny-90M-Instruct-Q4_K_M.gguf" -> "Falcon-H1-Tiny 90M (Q4_K_M)" */
static std::string model_label(const std::string &f)
{
   std::string s = f.substr(0, f.size() - 5);
   size_t slash = s.rfind('/');
   if (slash != std::string::npos)
      s = s.substr(slash + 1);
   size_t q = s.rfind('-');
   std::string quant = q == std::string::npos ? "" : s.substr(q + 1);
   std::string base = q == std::string::npos ? s : s.substr(0, q);
   size_t ins = base.find("-Instruct");
   if (ins != std::string::npos)
      base.erase(ins);
   return base + (quant.empty() ? "" : " (" + quant + ")");
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
   llama_log_set([](enum ggml_log_level, const char *, void *) {}, nullptr);
   ggml_backend_load_all();

   std::vector<std::string> models = list_models();
   int mi = 0;
   {
      FILE *f = fopen(CFG, "r");
      char line[300] = "";
      if (f && fgets(line, sizeof(line), f))
      {
         line[strcspn(line, "\n")] = 0;
         for (size_t i = 0; i < models.size(); i++)
            if (models[i] == line)
               mi = (int)i;
      }
      if (f)
         fclose(f);
   }

   Bot bot;
   std::vector<Line> chat;
   std::string typed;
   int kr = 0, kc = 0, scroll = 0, quit = 0;
   std::thread worker;
   bool loaded = false, need_load = !models.empty();
   std::thread loader;
   std::atomic<int> load_state{0};   /* 0 idle, 1 loading, 2 done */
   bool load_ok = false;
   double power_down = 0, held_at = 0;
   int held = 0;
   int picking = 0, pick = 0;   /* the model list is open; the one highlighted */
   int sel_down = 0, sel_used = 0;   /* Select held, and used for Select + B */
   std::vector<std::string> cwords, sugg;   /* words from this chat; the current suggestions */
   int sg = 0;                       /* the suggestion the cursor is on (kr == -1) */

   auto draw = [&](const char *status) {
      background(c);
      gfx_fill_rect(c, 0, 0, c->w, 46, 0x60000000);
      gfx_draw_text(c, f_big, "Chatbot", 18, (46 - font_height(f_big)) / 2, C_TEXT, 0);
      {
         int kw = glyphs(c, "Select + B", 0, 0, 0), w = kw + 8 + font_text_width(f_small, "Leave") + 14;
         hint_box(c, "Select + B", "Leave", c->w / 2 - w / 2, 23);
      }
      std::string ml = models.empty() ? "no model" : model_label(models[mi]);
      gfx_draw_text(c, f_small, ml.c_str(), c->w - 18 - font_text_width(f_small, ml.c_str()),
            (46 - font_height(f_small)) / 2, C_DIM, 0);

      /* the conversation, newest at the bottom */
      int top = 54, bottom = 216, lh = font_height(f_mid) + 2, tw = c->w - 70;
      std::vector<std::pair<int, std::string>> lines;   /* 1 user, 0 bot, -1 gap */
      std::string live;
      {
         std::lock_guard<std::mutex> l(mtx);
         live = reply;
      }
      auto add = [&](bool user, const std::string &t0) {
         std::string t = t0;
         if (!user)
         {
            size_t a = t.find("<think>");
            if (a != std::string::npos)
            {
               size_t e = t.find("</think>");
               if (e == std::string::npos)
               {
                  /* still thinking: its last few lines, dimmed */
                  auto th = wrap(f_small, t.substr(a + 7), tw - 20);
                  lines.push_back({ 2, "Thinking..." });
                  for (size_t i = th.size() > 3 ? th.size() - 3 : 0; i < th.size(); i++)
                     lines.push_back({ 3, th[i] });
                  lines.push_back({ -1, "" });
                  return;
               }
               lines.push_back({ 2, "(thought it over)" });
               t = t.substr(e + 8);
               while (!t.empty() && (t[0] == '\n' || t[0] == ' '))
                  t.erase(0, 1);
            }
         }
         for (auto &s : wrap(f_mid, t, tw - 20))
            lines.push_back({ user ? 1 : 0, s });
         lines.push_back({ -1, "" });
      };
      for (auto &m : chat)
         add(m.user, m.text);
      if (busy)
         add(false, live.empty() ? "..." : live);
      if (chat.empty() && !busy)
      {
         centered(c, f_mid, models.empty() ? "No model found" : "Say something to the world's worst chatbot.",
               c->w / 2, 120, C_DIM);
         centered(c, f_small, models.empty() ? "Put a .gguf model in chatbot/ on G36-GAMES."
               : "It's tiny, it runs on the handheld itself, and it makes things up.", c->w / 2, 156, C_FAINT);
      }
      int rows = (bottom - top) / lh, total = (int)lines.size();
      if (scroll > std::max(0, total - rows))
         scroll = std::max(0, total - rows);
      int first = std::max(0, total - rows - scroll);
      for (int i = 0; i < rows && first + i < total; i++)
      {
         auto &ln = lines[first + i];
         if (ln.first < 0)
            continue;
         int y = top + i * lh;
         if (ln.first == 1)
            gfx_draw_text(c, f_mid, ln.second.c_str(), c->w - 30 - font_text_width(f_mid, ln.second.c_str()), y, C_ACCENT, 0);
         else if (ln.first == 2)
            gfx_draw_text(c, f_small, ln.second.c_str(), 30, y + 2, C_ACCENT, 0);
         else if (ln.first == 3)
            gfx_draw_text(c, f_small, ln.second.c_str(), 44, y + 2, C_FAINT, 0);
         else
            gfx_draw_text(c, f_mid, ln.second.c_str(), 30, y, C_TEXT, 0);
      }
      if (total > rows)
      {
         /* where in the conversation this is */
         int track = bottom - top, bar = std::max(16, track * rows / total);
         int pos = (total - rows - scroll) * (track - bar) / std::max(1, total - rows);
         gfx_fill_rect(c, c->w - 12, top, 3, track, C_FAINT);
         gfx_fill_rect(c, c->w - 12, top + pos, 3, bar, C_ACCENT);
      }

      /* what's being typed */
      round_rect(c, 23, 222, c->w - 46, 34, 10, C_ACCENT);
      round_rect(c, 25, 224, c->w - 50, 30, 8, C_PANEL);
      if (!status && load_state && !picking)
         status = "Loading the model...";
      if (status)
         gfx_draw_text(c, f_mid, status, 40, 224 + (30 - font_height(f_mid)) / 2, C_FAINT, 0);
      else if (typed.empty())
         gfx_draw_text(c, f_mid, busy ? "Thinking... (B stops it)" : "Type a message", 40, 224 + (30 - font_height(f_mid)) / 2, C_FAINT, 0);
      else
      {
         int w = gfx_draw_text(c, f_mid, typed.c_str(), 40, 224 + (30 - font_height(f_mid)) / 2, C_TEXT, c->w - 100);
         if (fmod(now_s(), 1.0) < 0.6)
            gfx_fill_rect(c, 42 + w, 230, 2, 18, C_ACCENT);
      }

      /* the keyboard */
      int kw = 55, kh = 26, gap = 4, pad = 14, kx0 = (c->w - (KB_COLS * kw + (KB_COLS - 1) * gap + pad)) / 2, ky0 = 300;
      {
         /* the suggestions: R takes the first */
         int fw = KB_COLS * kw + (KB_COLS - 1) * gap + pad, bw = (fw - 3 * gap) / NSUGG;
         for (int i = 0; i < NSUGG; i++)
         {
            int x = kx0 + i * (bw + gap), y = 264, on = kr == -1 && sg == i;
            if (i >= (int)sugg.size())
            {
               round_rect(c, x, y, bw, 26, 13, C_BAR_BOX);
               continue;
            }
            round_rect(c, x, y, bw, 26, 13, on ? C_ACCENT : C_SEL);
            centered(c, f_small, sugg[i].c_str(), x + bw / 2 + (i == 0 ? 10 : 0), y + (26 - font_height(f_small)) / 2, C_TEXT);
            if (i == 0)
            {
               round_rect(c, x + 4, y + 3, 20, 20, 10, on ? 0xffffffff : C_ACCENT);
               ink_centered(c, f_bold, "R", x + 14, y + 13, on ? C_ACCENT : 0xffffffff);
            }
         }
      }
      for (int r = 0; r < 4; r++)
         for (int k = 0; k < KB_COLS; k++)
         {
            if (kfirst(r, k) != k)
               continue;
            int key = kkey(r, k), span = kspan(r, k), x = kx0 + k * (kw + gap) + (k >= KB_PAD ? pad : 0);
            int y = ky0 + r * (kh + gap), w = span * kw + (span - 1) * gap, sel = r == kr && kfirst(kr, kc) == k;
            char ch[2] = { (char)key, 0 };
            const char *label = key == KB_SPACE ? "Space" : key == KB_DEL ? "Del" : key == KB_SEND ? "Send"
                  : key == KB_SYM ? (kb_sym ? "ABC" : "Symbols") : ch;
            round_rect(c, x - 1, y - 1, w + 2, kh + 2, 8, sel ? 0xffe2a8ff : C_KEY_LINE);
            round_rect(c, x, y, w, kh, 7, sel ? C_ACCENT : C_KEY);
            centered(c, f_small, label, x + w / 2, y + (kh - font_height(f_small)) / 2, C_TEXT);
         }
      if (!picking)
         hints(c, busy ? "B  Stop     L / R  Scroll"
               : "A  Type     B  Delete     R  Finish word     Start  Send     L  Scroll     X  New chat     Select  Model");
      if (picking)
      {
         /* the model list, over everything */
         int n = (int)models.size(), rh = 32, heads = (nchat > 0) + (n > nchat), w = 540;
         int h = 64 + n * rh + heads * 28 + 12, x0 = (c->w - w) / 2, y0 = std::max(50, (c->h - 40 - h) / 2), y = y0 + 58;
         for (int yy = 0; yy < c->h; yy++)
            for (int x = 0; x < c->w; x++)
            {
               uint32_t &p = c->px[(size_t)yy * c->stride + x];
               p = 0xff000000 | (((p >> 16 & 255) * 2 / 5) << 16) | (((p >> 8 & 255) * 2 / 5) << 8) | ((p & 255) * 2 / 5);
            }
         round_rect(c, x0 - 1, y0 - 1, w + 2, h + 2, 13, C_ACCENT);
         round_rect(c, x0, y0, w, h, 12, C_PANEL);
         gfx_draw_text(c, f_big, "Choose a model", x0 + 20, y0 + 12, C_TEXT, 0);
         for (int i = 0; i < n; i++)
         {
            if (i == 0 || i == nchat)
            {
               gfx_draw_text(c, f_small, i < nchat ? "CHAT MODELS" : "REASONING MODELS (think first, slower)",
                     x0 + 22, y + 6, C_ACCENT, 0);
               y += 28;
            }
            if (i == pick)
            {
               gfx_fill_rect(c, x0 + 12, y, w - 24, rh - 2, C_SEL);
               gfx_fill_rect(c, x0 + 12, y, 4, rh - 2, C_ACCENT);
            }
            std::string l = model_label(models[i]);
            gfx_draw_text(c, f_mid, l.c_str(), x0 + 28, y + (rh - 2 - font_height(f_mid)) / 2, i == pick ? C_TEXT : C_DIM, w - 150);
            if (load_state && i == mi)
            {
               static const char *const spin[4] = { "Loading   ", "Loading.  ", "Loading.. ", "Loading..." };
               const char *t = spin[(int)(now_s() * 3) % 4];
               gfx_draw_text(c, f_small, t, x0 + w - 30 - font_text_width(f_small, "Loading..."),
                     y + (rh - 2 - font_height(f_small)) / 2, C_ACCENT, 0);
            }
            else if (i == mi && loaded)
               gfx_draw_text(c, f_small, "in use", x0 + w - 30 - font_text_width(f_small, "in use"),
                     y + (rh - 2 - font_height(f_small)) / 2, C_ACCENT, 0);
            y += rh;
         }
         hints(c, load_state ? "" : "Dpad  Choose     A  Load     B  Cancel");
      }
      if (want_shot)
      {
         want_shot = 0;
         save_shot(c);
      }
      gfx_present();
   };

   while (!quit)
   {
      if (need_load && load_state == 0)
      {
         /* load on a thread so the screen keeps moving */
         need_load = false;
         loaded = false;
         load_state = 1;
         std::string path = std::string(DIR_MODELS "/") + models[mi];
         loader = std::thread([&bot, &load_ok, &load_state, path]() {
            load_ok = bot.load(path);
            load_state = 2;
         });
      }
      if (load_state == 2)
      {
         loader.join();
         load_state = 0;
         loaded = load_ok;
         bot.reasoning = is_reasoning(models[mi]);
         bot.name = models[mi];
         log_start(model_label(models[mi]));
         picking = 0;   /* the list closes once the new model is ready */
         if (!loaded)
            chat.push_back({ false, "(That model wouldn't load.)" });
      }

      key_event_t ev;
      while (input_next(&ev, busy ? 30 : 60))
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
            {
               sel_down = 1;
               sel_used = 0;
            }
            else if (ev.value == 0)
            {
               /* Select on its own: the model list (or close it) */
               if (sel_down && !sel_used)
               {
                  if (load_state)
                     ;   /* loading: the list stays */
                  else if (picking)
                     picking = 0;
                  else if (!busy && !models.empty())
                  {
                     picking = 1;
                     pick = mi;
                  }
               }
               sel_down = 0;
            }
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
            /* Select + B: back to the menu, like leaving a game */
            sel_used = 1;
            stop_req = true;
            quit = 1;
            continue;
         }
         if (ev.value != 1)
            continue;
         if (ev.code == KEY_UP || ev.code == KEY_DOWN || ev.code == KEY_LEFT || ev.code == KEY_RIGHT
               || ev.code == BTN_TL || ev.code == BTN_TR)
         {
            held = ev.code;
            held_at = t + 0.35;
         }
         if (load_state)
            continue;   /* loading: wait */
         if (picking)
         {
            int n = (int)models.size();
            if (ev.code == KEY_UP)
               pick = (pick + n - 1) % n;
            else if (ev.code == KEY_DOWN)
               pick = (pick + 1) % n;
            else if (ev.code == BTN_EAST)
               picking = 0;
            else if (ev.code == BTN_SOUTH)
            {
               if (pick == mi && loaded)
                  picking = 0;   /* already in use */
               else
               {
                  mi = pick;
                  FILE *f = fopen(CFG, "w");
                  if (f)
                  {
                     fprintf(f, "%s\n", models[mi].c_str());
                     fclose(f);
                  }
                  chat.clear();
                  need_load = true;
               }
            }
            held = 0;
            continue;
         }
         auto send = [&]() {
            if (typed.empty() || busy || !loaded || load_state)
               return;
            chat.push_back({ true, typed });
            log_add("You", typed);
            chat_words(typed, cwords);
            {
               std::lock_guard<std::mutex> l(mtx);
               reply.clear();
            }
            busy = true;
            stop_req = false;
            scroll = 0;
            if (worker.joinable())
               worker.join();
            worker = std::thread(generate, &bot, typed);
            typed.clear();
         };
         switch (ev.code)
         {
         case KEY_UP:
            if (kr == 0 && !sugg.empty())
               kr = -1, sg = 0;
            else
               kr = kr < 0 ? 3 : (kr + 3) % 4;
            break;
         case KEY_DOWN: kr = kr < 0 ? 0 : (kr + 1) % 4; break;
         case KEY_LEFT:
            if (kr < 0)
               sg = (sg + (int)sugg.size() - 1) % std::max(1, (int)sugg.size());
            else
               kc = (kfirst(kr, kc) + KB_COLS - 1) % KB_COLS;
            break;
         case KEY_RIGHT:
            if (kr < 0)
               sg = (sg + 1) % std::max(1, (int)sugg.size());
            else
               kc = (kfirst(kr, kc) + kspan(kr, kc)) % KB_COLS;
            break;
         case BTN_SOUTH:
         {
            if (kr < 0)
            {
               if (sg < (int)sugg.size())
                  typed = accept(typed, sugg[sg]);
               kr = 0;
               break;
            }
            int k = kkey(kr, kc);
            if (k == KB_SYM)
               kb_sym ^= 1;
            else if (k == KB_SEND)
               send();
            else if (k == KB_DEL)
            {
               if (!typed.empty())
                  typed.pop_back();
            }
            else if (typed.size() < 200 && !(k == KB_SPACE && (typed.empty() || typed.back() == ' ')))
               typed += k == KB_SPACE ? ' ' : (char)tolower(k);
            break;
         }
         case BTN_WEST:
            if (!typed.empty() && typed.back() != ' ' && typed.size() < 200)
               typed += ' ';
            break;
         case BTN_EAST:
            if (busy)
               stop_req = true;
            else if (!typed.empty())
               typed.pop_back();
            break;
         case BTN_START: send(); break;
         case BTN_NORTH:
            if (!busy && loaded)
            {
               bot.clear();
               chat.clear();
               log_start(model_label(models[mi]));
               scroll = 0;
            }
            break;
         case BTN_TL: scroll += 3; break;
         case BTN_TR:
            /* R: finish the word with the first suggestion; with none, scroll down */
            if (!sugg.empty())
            {
               typed = accept(typed, sugg[0]);
               held = 0;
            }
            else
               scroll = std::max(0, scroll - 3);
            break;
         }
      }
      sugg = suggest(typed, cwords);
      if (kr < 0 && sugg.empty())
         kr = 0;
      if (sg >= (int)sugg.size())
         sg = 0;
      /* a reply has finished: it joins the conversation */
      if (worker.joinable() && !busy)
      {
         worker.join();
         std::lock_guard<std::mutex> l(mtx);
         chat.push_back({ false, reply.empty() ? "..." : reply });
         log_add("Bot", reply);
         chat_words(reply, cwords);
         reply.clear();
      }
      double t = now_s();
      if (held && !picking && input_held(held) && t >= held_at)
      {
         held_at = t + 0.12;
         if (held == KEY_LEFT && kr >= 0) kc = (kfirst(kr, kc) + KB_COLS - 1) % KB_COLS;
         else if (held == KEY_RIGHT && kr >= 0) kc = (kfirst(kr, kc) + kspan(kr, kc)) % KB_COLS;
         else if (held == KEY_UP && kr > 0) kr--;
         else if (held == KEY_DOWN && kr >= 0) kr = (kr + 1) % 4;
         else if (held == BTN_TL) scroll += 2;
         else if (held == BTN_TR && sugg.empty()) scroll = std::max(0, scroll - 2);
      }
      /* hold Power: save nothing (there's nothing to save) and power off safely */
      if (power_down && t - power_down > 1.5)
      {
         FILE *f = fopen(HALT_FLAG, "w");
         if (f)
            fclose(f);
         stop_req = true;
         quit = 1;
      }
      draw(nullptr);
   }
   stop_req = true;
   if (worker.joinable())
      worker.join();
   if (loader.joinable())
      loader.join();
   bot.unload();
   input_close();
   gfx_shutdown();
   return 0;
}
