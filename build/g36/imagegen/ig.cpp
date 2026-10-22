/* The G36 image generator engine. See ig.h.
 *
 * Supra2-IMG is a rectified-flow DiT: the picture's latent starts as noise at t = 0
 * and moves to the picture at t = 1 in Euler steps, z += dt * v, where v is the model's
 * velocity. With guidance, each step runs the model twice, with the prompt and with the
 * stored "no prompt" text, and pushes away from the second: v = vu + g * (vc - vu).
 * The architecture follows the release's inference.py exactly. */
#include "ig.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"

/* DiT */
#define D_MODEL  576
#define DEPTH    14
#define N_HEADS  9
#define HEAD_DIM 64
#define MLP_DIM  2304
#define PATCH    2
#define N_TOK    ((IG_LAT / PATCH) * (IG_LAT / PATCH))   /* 256 */
#define LAT_CH   4
#define PATCH_IN (LAT_CH * PATCH * PATCH)                /* 16 */
#define FREQ_DIM 256
/* T5 */
#define T5_D     768
#define T5_FF    2048
#define T5_HEADS 12
#define T5_LAYERS 12
#define T5_MAX   128

/* ---------------------------------------------------------------- weights */

struct weights
{
   ggml_context *ctx = nullptr;
   ggml_backend_buffer_t buf = nullptr;
   std::map<std::string, ggml_tensor *> t;
   ggml_tensor *get(const std::string &n)
   {
      auto it = t.find(n);
      if (it == t.end())
      {
         fprintf(stderr, "ig: missing weight %s\n", n.c_str());
         return nullptr;
      }
      return it->second;
   }
   void free_all()
   {
      if (buf)
         ggml_backend_buffer_free(buf);
      if (ctx)
         ggml_free(ctx);
      buf = nullptr;
      ctx = nullptr;
      t.clear();
   }
};

struct entry
{
   std::string name;
   int type, ndim;
   int64_t ne[4];
   uint64_t bytes;
   long offset;
};

static bool read_index(FILE *f, std::vector<entry> &out)
{
   char magic[4];
   uint32_t ver, n;
   if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "G36W", 4) || fread(&ver, 4, 1, f) != 1 ||
         fread(&n, 4, 1, f) != 1)
      return false;
   for (uint32_t i = 0; i < n; i++)
   {
      uint16_t nl;
      uint8_t type, ndim;
      uint32_t ne[4];
      uint64_t bytes;
      entry e;
      if (fread(&nl, 2, 1, f) != 1)
         return false;
      e.name.resize(nl);
      if (fread(&e.name[0], 1, nl, f) != nl || fread(&type, 1, 1, f) != 1 || fread(&ndim, 1, 1, f) != 1 ||
            fread(ne, 4, 4, f) != 4 || fread(&bytes, 8, 1, f) != 1)
         return false;
      long pos = ftell(f);
      pos += (32 - pos % 32) % 32;
      e.type = type;
      e.ndim = ndim;
      for (int k = 0; k < 4; k++)
         e.ne[k] = ne[k];
      e.bytes = bytes;
      e.offset = pos;
      fseek(f, pos + (long)bytes, SEEK_SET);
      out.push_back(e);
   }
   return true;
}

static bool load_weights(weights &w, const char *path, ggml_backend_t backend)
{
   FILE *f = fopen(path, "rb");
   std::vector<entry> idx;
   if (!f)
      return false;
   if (!read_index(f, idx))
   {
      fclose(f);
      return false;
   }
   ggml_init_params p = { ggml_tensor_overhead() * (idx.size() + 8), nullptr, true };
   w.ctx = ggml_init(p);
   for (auto &e : idx)
   {
      ggml_tensor *t = ggml_new_tensor(w.ctx, (ggml_type)e.type, e.ndim ? e.ndim : 1, e.ne);
      ggml_set_name(t, e.name.c_str());
      w.t[e.name] = t;
   }
   w.buf = ggml_backend_alloc_ctx_tensors(w.ctx, backend);
   if (!w.buf)
   {
      fclose(f);
      w.free_all();
      return false;
   }
   std::vector<char> tmp;
   for (auto &e : idx)
   {
      ggml_tensor *t = w.t[e.name];
      if (ggml_nbytes(t) != e.bytes)
      {
         fprintf(stderr, "ig: %s: size %zu != %llu\n", e.name.c_str(), ggml_nbytes(t), (unsigned long long)e.bytes);
         fclose(f);
         w.free_all();
         return false;
      }
      tmp.resize(e.bytes);
      fseek(f, e.offset, SEEK_SET);
      if (fread(tmp.data(), 1, e.bytes, f) != e.bytes)
      {
         fclose(f);
         w.free_all();
         return false;
      }
      ggml_backend_tensor_set(t, tmp.data(), 0, e.bytes);
   }
   fclose(f);
   return true;
}

int ig_pack(const char *in, const char *out)
{
   FILE *f = fopen(in, "rb"), *o;
   std::vector<entry> idx;
   if (!f || !read_index(f, idx))
      return -1;
   if (!(o = fopen(out, "wb")))
      return -1;
   uint32_t ver = 1, n = (uint32_t)idx.size();
   fwrite("G36W", 1, 4, o);
   fwrite(&ver, 4, 1, o);
   fwrite(&n, 4, 1, o);
   std::vector<float> src;
   std::vector<char> dst;
   size_t big = 0, small = 0;
   for (auto &e : idx)
   {
      int64_t count = e.ne[0] * e.ne[1] * e.ne[2] * e.ne[3];
      src.resize(count);
      fseek(f, e.offset, SEEK_SET);
      if (fread(src.data(), 4, count, f) != (size_t)count)
         return -1;
      /* the matrices (most of the size) to 8-bit; biases, norms and the like stay f32 */
      bool q = e.ndim == 2 && e.ne[0] % 32 == 0 && e.ne[1] >= 16 && count >= 65536 &&
            e.name.find("relative_attention_bias") == std::string::npos && e.name != "uncond_text" &&
            e.name != "pos_embed";
      ggml_type type = q ? GGML_TYPE_Q8_0 : GGML_TYPE_F32;
      size_t bytes = q ? ggml_row_size(type, e.ne[0]) * (count / e.ne[0]) : (size_t)count * 4;
      dst.resize(bytes);
      if (q)
         ggml_quantize_chunk(type, src.data(), dst.data(), 0, count / e.ne[0], e.ne[0], nullptr);
      else
         memcpy(dst.data(), src.data(), bytes);
      (q ? big : small) += bytes;
      uint16_t nl = (uint16_t)e.name.size();
      uint8_t t8 = (uint8_t)type, nd = (uint8_t)e.ndim;
      uint32_t ne[4] = { (uint32_t)e.ne[0], (uint32_t)e.ne[1], (uint32_t)e.ne[2], (uint32_t)e.ne[3] };
      uint64_t b64 = bytes;
      fwrite(&nl, 2, 1, o);
      fwrite(e.name.data(), 1, nl, o);
      fwrite(&t8, 1, 1, o);
      fwrite(&nd, 1, 1, o);
      fwrite(ne, 4, 4, o);
      fwrite(&b64, 8, 1, o);
      static const char zero[32] = { 0 };
      long pos = ftell(o);
      fwrite(zero, 1, (32 - pos % 32) % 32, o);
      fwrite(dst.data(), 1, bytes, o);
   }
   fclose(f);
   fclose(o);
   fprintf(stderr, "ig: packed %s: %.1f MB 8-bit + %.1f MB f32\n", out, big / 1e6, small / 1e6);
   return 0;
}

/* ---------------------------------------------------------------- the T5 tokenizer */

/* SentencePiece unigram, read straight from spiece.model (a protobuf): the pieces and
 * their scores; the best split is found with Viterbi. Text is trimmed, runs of spaces
 * become one, and spaces become U+2581 with one at the start, as T5's model does. */
struct tokenizer
{
   std::map<std::string, std::pair<int, float>> pieces;
   size_t max_len = 1;
   bool load(const char *path)
   {
      FILE *f = fopen(path, "rb");
      if (!f)
         return false;
      std::vector<unsigned char> b;
      int ch;
      while ((ch = fgetc(f)) != EOF)
         b.push_back((unsigned char)ch);
      fclose(f);
      size_t i = 0;
      int id = 0;
      auto varint = [&](size_t &p) {
         uint64_t v = 0;
         int s = 0;
         while (p < b.size())
         {
            uint8_t c = b[p++];
            v |= (uint64_t)(c & 0x7f) << s;
            if (!(c & 0x80))
               break;
            s += 7;
         }
         return v;
      };
      while (i < b.size())
      {
         uint64_t key = varint(i), field = key >> 3, wt = key & 7;
         if (wt == 2)
         {
            uint64_t len = varint(i);
            if (field == 1)
            {
               /* a SentencePiece: 1 piece (string), 2 score (float), 3 type (enum) */
               size_t p = i, end = i + len;
               std::string piece;
               float score = 0;
               int type = 1;
               while (p < end)
               {
                  uint64_t k2 = varint(p), f2 = k2 >> 3, w2 = k2 & 7;
                  if (w2 == 2)
                  {
                     uint64_t l2 = varint(p);
                     if (f2 == 1)
                        piece.assign((const char *)&b[p], l2);
                     p += l2;
                  }
                  else if (w2 == 5)
                  {
                     if (f2 == 2)
                        memcpy(&score, &b[p], 4);
                     p += 4;
                  }
                  else if (w2 == 0)
                  {
                     uint64_t v = varint(p);
                     if (f2 == 3)
                        type = (int)v;
                  }
                  else if (w2 == 1)
                     p += 8;
               }
               if (type == 1 || type == 4)   /* normal and user-defined pieces are matched */
               {
                  pieces[piece] = { id, score };
                  if (piece.size() > max_len)
                     max_len = piece.size();
               }
               id++;
            }
            i += len;
         }
         else if (wt == 0)
            varint(i);
         else if (wt == 5)
            i += 4;
         else if (wt == 1)
            i += 8;
         else
            break;
      }
      return !pieces.empty();
   }
   std::vector<int> encode(const char *text) const
   {
      /* normalize */
      std::string s = "\xe2\x96\x81";
      bool space = false;
      for (const char *p = text; *p; p++)
      {
         if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            space = s.size() > 3;
         else
         {
            if (space)
               s += "\xe2\x96\x81";
            space = false;
            s += *p;
         }
      }
      /* Viterbi over UTF-8 character boundaries */
      size_t n = s.size();
      std::vector<float> best(n + 1, -1e30f);
      std::vector<int> from(n + 1, -1), tok(n + 1, -1);
      best[0] = 0;
      for (size_t i = 0; i < n; i++)
      {
         if (best[i] <= -1e29f || ((unsigned char)s[i] & 0xc0) == 0x80)
            continue;
         bool any = false;
         for (size_t l = 1; l <= max_len && i + l <= n; l++)
         {
            if (i + l < n && ((unsigned char)s[i + l] & 0xc0) == 0x80)
               continue;
            auto it = pieces.find(s.substr(i, l));
            if (it == pieces.end())
               continue;
            any = true;
            float sc = best[i] + it->second.second;
            if (sc > best[i + l])
            {
               best[i + l] = sc;
               from[i + l] = (int)i;
               tok[i + l] = it->second.first;
            }
         }
         if (!any)
         {
            /* a character nothing covers: the unknown token */
            size_t l = 1;
            while (i + l < n && ((unsigned char)s[i + l] & 0xc0) == 0x80)
               l++;
            float sc = best[i] - 10;
            if (sc > best[i + l])
            {
               best[i + l] = sc;
               from[i + l] = (int)i;
               tok[i + l] = 2;
            }
         }
      }
      std::vector<int> ids;
      for (int at = (int)n; at > 0 && from[at] >= 0; at = from[at])
         ids.insert(ids.begin(), tok[at]);
      ids.push_back(1);   /* </s> */
      return ids;
   }
};

/* ---------------------------------------------------------------- the engine */

struct ig
{
   std::string dir, err;
   int threads;
   ggml_backend_t backend = nullptr;
   ggml_gallocr_t galloc = nullptr;
   weights t5, dit;
   std::vector<float> ctx_c, ctx_u;    /* T5 outputs: the prompt, and "no prompt" */
   int n_c = 0, n_u = 0;
   std::vector<float> z, v_last;       /* latent [4][32][32] */
   int steps = 0, done = 0;
   float guidance = 3;
};

static std::string path_of(ig_t *ig, const char *name)
{
   std::string p = ig->dir + "/" + name + ".bin";
   FILE *f = fopen(p.c_str(), "rb");
   if (f)
   {
      fclose(f);
      return p;
   }
   return ig->dir + "/" + name + ".f32";
}

ig_t *ig_new(const char *dir, int threads)
{
   ig_t *ig = new struct ig;
   ig->dir = dir;
   ig->threads = threads;
   ig->backend = ggml_backend_cpu_init();
   ggml_backend_cpu_set_n_threads(ig->backend, threads);
   ig->galloc = ggml_gallocr_new(ggml_backend_cpu_buffer_type());
   return ig;
}

void ig_free(ig_t *ig)
{
   if (!ig)
      return;
   ig->t5.free_all();
   ig->dit.free_all();
   ggml_gallocr_free(ig->galloc);
   ggml_backend_free(ig->backend);
   delete ig;
}

const char *ig_error(ig_t *ig)
{
   return ig->err.c_str();
}

/* a graph of up to this many nodes in a context of its own (the tensors' data goes in
 * the allocator's buffer, reused from one graph to the next) */
static ggml_context *graph_ctx(size_t nodes)
{
   ggml_init_params p = { ggml_tensor_overhead() * nodes + ggml_graph_overhead_custom(nodes, false), nullptr, true };
   return ggml_init(p);
}

static ggml_tensor *linear(ggml_context *c, ggml_tensor *x, ggml_tensor *w, ggml_tensor *b)
{
   x = ggml_mul_mat(c, w, x);
   return b ? ggml_add(c, x, b) : x;
}

/* attention: q [hd, nq, H], k [hd, nk, H], v [hd, nk, H] (contiguous rows) -> [hd*H, nq] */
static ggml_tensor *attend(ggml_context *c, ggml_tensor *q, ggml_tensor *k, ggml_tensor *v,
      ggml_tensor *bias, float scale)
{
   ggml_tensor *kq = ggml_mul_mat(c, k, q);   /* [nk, nq, H] */
   if (bias)
      kq = ggml_add(c, kq, bias);
   kq = ggml_soft_max_ext(c, kq, nullptr, scale, 0.0f);
   ggml_tensor *vt = ggml_cont(c, ggml_permute(c, v, 1, 0, 2, 3));   /* [nk, hd, H] */
   ggml_tensor *o = ggml_mul_mat(c, vt, kq);                         /* [hd, nq, H] */
   o = ggml_cont(c, ggml_permute(c, o, 0, 2, 1, 3));                 /* [hd, H, nq] */
   return ggml_reshape_2d(c, o, o->ne[0] * o->ne[1], o->ne[2]);
}

/* [d, n] rows of heads -> [hd, n, H] */
static ggml_tensor *heads(ggml_context *c, ggml_tensor *x, int nh, int hd, size_t offset_elems, size_t row_elems)
{
   int64_t n = x->ne[1];
   ggml_tensor *v = ggml_view_3d(c, x, hd, nh, n, hd * sizeof(float), row_elems * sizeof(float),
         offset_elems * sizeof(float));
   return ggml_cont(c, ggml_permute(c, v, 0, 2, 1, 3));
}

static bool compute(ig_t *ig, ggml_cgraph *gf)
{
   if (!ggml_gallocr_alloc_graph(ig->galloc, gf))
   {
      ig->err = "Not enough memory.";
      return false;
   }
   return ggml_backend_graph_compute(ig->backend, gf) == GGML_STATUS_SUCCESS;
}

/* T5's relative position buckets (bidirectional, 32 buckets, distances up to 128) */
static int t5_bucket(int rel)
{
   int ret = rel > 0 ? 16 : 0, n = rel < 0 ? -rel : rel;
   if (n < 8)
      return ret + n;
   int large = 8 + (int)(logf((float)n / 8) / logf(128.0f / 8) * 8);
   return ret + (large < 15 ? large : 15);
}

static bool t5_run(ig_t *ig, const std::vector<int> &ids, std::vector<float> &out)
{
   weights &w = ig->t5;
   int n = (int)ids.size();
   ggml_context *c = graph_ctx(4096);
   ggml_cgraph *gf = ggml_new_graph_custom(c, 4096, false);
   ggml_tensor *in = ggml_new_tensor_1d(c, GGML_TYPE_I32, n);
   ggml_set_input(in);
   ggml_tensor *pb = ggml_new_tensor_3d(c, GGML_TYPE_F32, n, n, T5_HEADS);
   ggml_set_input(pb);
   ggml_tensor *x = ggml_get_rows(c, w.get("shared.weight"), in);   /* [768, n] */
   for (int l = 0; l < T5_LAYERS; l++)
   {
      char p[96];
      snprintf(p, sizeof(p), "encoder.block.%d.layer.", l);
      std::string P = p;
      ggml_tensor *h = ggml_mul(c, ggml_rms_norm(c, x, 1e-6f), w.get(P + "0.layer_norm.weight"));
      ggml_tensor *q = ggml_mul_mat(c, w.get(P + "0.SelfAttention.q.weight"), h);
      ggml_tensor *k = ggml_mul_mat(c, w.get(P + "0.SelfAttention.k.weight"), h);
      ggml_tensor *v = ggml_mul_mat(c, w.get(P + "0.SelfAttention.v.weight"), h);
      q = heads(c, q, T5_HEADS, 64, 0, T5_D);
      k = heads(c, k, T5_HEADS, 64, 0, T5_D);
      v = heads(c, v, T5_HEADS, 64, 0, T5_D);
      ggml_tensor *a = attend(c, q, k, v, pb, 1.0f);   /* T5 doesn't scale its scores */
      x = ggml_add(c, x, ggml_mul_mat(c, w.get(P + "0.SelfAttention.o.weight"), a));
      h = ggml_mul(c, ggml_rms_norm(c, x, 1e-6f), w.get(P + "1.layer_norm.weight"));
      ggml_tensor *g = ggml_gelu(c, ggml_mul_mat(c, w.get(P + "1.DenseReluDense.wi_0.weight"), h));
      ggml_tensor *u = ggml_mul_mat(c, w.get(P + "1.DenseReluDense.wi_1.weight"), h);
      x = ggml_add(c, x, ggml_mul_mat(c, w.get(P + "1.DenseReluDense.wo.weight"), ggml_mul(c, g, u)));
   }
   x = ggml_mul(c, ggml_rms_norm(c, x, 1e-6f), w.get("encoder.final_layer_norm.weight"));
   ggml_set_output(x);
   ggml_build_forward_expand(gf, x);
   bool ok = ggml_gallocr_alloc_graph(ig->galloc, gf);
   if (ok)
   {
      ggml_backend_tensor_set(in, ids.data(), 0, n * sizeof(int));
      /* the position bias (layer 0's, shared by every layer): [key j, query i, head] */
      ggml_tensor *rb = w.get("encoder.block.0.layer.0.SelfAttention.relative_attention_bias.weight");
      std::vector<float> table(32 * T5_HEADS), bias((size_t)n * n * T5_HEADS);
      ggml_backend_tensor_get(rb, table.data(), 0, table.size() * 4);   /* [bucket][head] */
      for (int hh = 0; hh < T5_HEADS; hh++)
         for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
               bias[((size_t)hh * n + i) * n + j] = table[t5_bucket(j - i) * T5_HEADS + hh];
      ggml_backend_tensor_set(pb, bias.data(), 0, bias.size() * 4);
      ok = ggml_backend_graph_compute(ig->backend, gf) == GGML_STATUS_SUCCESS;
   }
   if (ok)
   {
      out.resize((size_t)n * T5_D);
      ggml_backend_tensor_get(x, out.data(), 0, out.size() * 4);
   }
   ggml_free(c);
   return ok;
}

int ig_encode(ig_t *ig, const char *prompt)
{
   tokenizer tk;
   if (!tk.load((ig->dir + "/spiece.model").c_str()))
   {
      ig->err = "The text encoder's word list (spiece.model) is missing.";
      return -1;
   }
   std::vector<int> ids = tk.encode(prompt);
   if (ids.size() > T5_MAX)
   {
      ids.resize(T5_MAX - 1);
      ids.push_back(1);
   }
   fprintf(stderr, "ig: prompt: %zu tokens:", ids.size());
   for (int id : ids)
      fprintf(stderr, " %d", id);
   fprintf(stderr, "\n");
   ig->dit.free_all();   /* one model at a time */
   if (!load_weights(ig->t5, path_of(ig, "t5").c_str(), ig->backend))
   {
      ig->err = "The text encoder (t5.bin) won't load.";
      return -1;
   }
   bool ok = t5_run(ig, ids, ig->ctx_c);
   ig->t5.free_all();
   if (!ok)
   {
      if (ig->err.empty())
         ig->err = "The text encoder didn't run.";
      return -1;
   }
   ig->n_c = (int)ids.size();
   return 0;
}

/* ---------------------------------------------------------------- the DiT */

static float randn(uint64_t &s)
{
   auto u = [&]() {
      s ^= s << 13;
      s ^= s >> 7;
      s ^= s << 17;
      return ((s >> 11) + 0.5) / 9007199254740992.0;
   };
   double a = u(), b = u();
   return (float)(sqrt(-2 * log(a)) * cos(6.283185307179586 * b));
}

int ig_begin(ig_t *ig, int steps, float guidance, uint32_t seed)
{
   if (!ig->n_c)
   {
      ig->err = "No prompt yet.";
      return -1;
   }
   if (!ig->dit.ctx && !load_weights(ig->dit, path_of(ig, "dit").c_str(), ig->backend))
   {
      ig->err = "The picture model (dit.bin) won't load.";
      return -1;
   }
   ggml_tensor *un = ig->dit.get("uncond_text");
   if (!un)
      return -1;
   ig->n_u = (int)un->ne[1];
   ig->ctx_u.resize((size_t)ig->n_u * T5_D);
   ggml_backend_tensor_get(un, ig->ctx_u.data(), 0, ig->ctx_u.size() * 4);
   ig->steps = steps < 1 ? 1 : steps > 200 ? 200 : steps;
   ig->guidance = guidance;
   ig->done = 0;
   ig->z.resize(LAT_CH * IG_LAT * IG_LAT);
   ig->v_last.assign(ig->z.size(), 0);
   uint64_t s = 0x9e3779b97f4a7c15ull ^ ((uint64_t)seed * 0xbf58476d1ce4e5b9ull);
   if (!s)
      s = 1;
   for (float &v : ig->z)
      v = randn(s);
   return 0;
}

int ig_steps_total(ig_t *ig)
{
   return ig->steps;
}

/* the velocity at time t for one text (ctx [768][n]) */
static bool dit_run(ig_t *ig, float t, const std::vector<float> &ctx, int n, std::vector<float> &v)
{
   weights &w = ig->dit;
   ggml_context *c = graph_ctx(8192);
   ggml_cgraph *gf = ggml_new_graph_custom(c, 8192, false);
   ggml_tensor *xin = ggml_new_tensor_2d(c, GGML_TYPE_F32, PATCH_IN, N_TOK);
   ggml_tensor *tin = ggml_new_tensor_1d(c, GGML_TYPE_F32, FREQ_DIM);
   ggml_tensor *cin = ggml_new_tensor_2d(c, GGML_TYPE_F32, T5_D, n);
   ggml_set_input(xin);
   ggml_set_input(tin);
   ggml_set_input(cin);

   ggml_tensor *x = ggml_add(c, linear(c, xin, w.get("x_embed.weight"), w.get("x_embed.bias")),
         ggml_reshape_2d(c, w.get("pos_embed"), D_MODEL, N_TOK));
   ggml_tensor *te = linear(c, tin, w.get("t_embed.mlp.0.weight"), w.get("t_embed.mlp.0.bias"));
   te = linear(c, ggml_silu(c, te), w.get("t_embed.mlp.2.weight"), w.get("t_embed.mlp.2.bias"));
   ggml_tensor *cs = ggml_silu(c, te);   /* what every adaLN reads */
   ggml_tensor *ctx2 = linear(c, cin, w.get("ctx_proj.weight"), w.get("ctx_proj.bias"));   /* [576, n] */
   const float scale = 1.0f / sqrtf((float)HEAD_DIM);

   for (int b = 0; b < DEPTH; b++)
   {
      char p[48];
      snprintf(p, sizeof(p), "blocks.%d.", b);
      std::string P = p;
      ggml_tensor *mod = linear(c, cs, w.get(P + "adaln.1.weight"), w.get(P + "adaln.1.bias"));   /* [3456] */
      auto part = [&](int i) { return ggml_view_1d(c, mod, D_MODEL, (size_t)i * D_MODEL * sizeof(float)); };
      ggml_tensor *shift_sa = part(0), *scale_sa = part(1), *gate_sa = part(2);
      ggml_tensor *shift_mlp = part(3), *scale_mlp = part(4), *gate_mlp = part(5);

      /* self-attention, modulated and gated */
      ggml_tensor *h = ggml_norm(c, x, 1e-6f);
      h = ggml_add(c, ggml_add(c, h, ggml_mul(c, h, scale_sa)), shift_sa);
      ggml_tensor *qkv = linear(c, h, w.get(P + "self_attn.qkv.weight"), w.get(P + "self_attn.qkv.bias"));
      ggml_tensor *q = heads(c, qkv, N_HEADS, HEAD_DIM, 0, 3 * D_MODEL);
      ggml_tensor *k = heads(c, qkv, N_HEADS, HEAD_DIM, D_MODEL, 3 * D_MODEL);
      ggml_tensor *vv = heads(c, qkv, N_HEADS, HEAD_DIM, 2 * D_MODEL, 3 * D_MODEL);
      ggml_tensor *a = attend(c, q, k, vv, nullptr, scale);
      a = linear(c, a, w.get(P + "self_attn.proj.weight"), w.get(P + "self_attn.proj.bias"));
      x = ggml_add(c, x, ggml_mul(c, a, gate_sa));

      /* cross-attention to the text (no modulation) */
      h = ggml_norm(c, x, 1e-6f);
      q = linear(c, h, w.get(P + "cross_attn.q.weight"), w.get(P + "cross_attn.q.bias"));
      ggml_tensor *kv = linear(c, ctx2, w.get(P + "cross_attn.kv.weight"), w.get(P + "cross_attn.kv.bias"));
      q = heads(c, q, N_HEADS, HEAD_DIM, 0, D_MODEL);
      k = heads(c, kv, N_HEADS, HEAD_DIM, 0, 2 * D_MODEL);
      vv = heads(c, kv, N_HEADS, HEAD_DIM, D_MODEL, 2 * D_MODEL);
      a = attend(c, q, k, vv, nullptr, scale);
      x = ggml_add(c, x, linear(c, a, w.get(P + "cross_attn.proj.weight"), w.get(P + "cross_attn.proj.bias")));

      /* the MLP, modulated and gated */
      h = ggml_norm(c, x, 1e-6f);
      h = ggml_add(c, ggml_add(c, h, ggml_mul(c, h, scale_mlp)), shift_mlp);
      h = linear(c, h, w.get(P + "mlp.0.weight"), w.get(P + "mlp.0.bias"));
      h = linear(c, ggml_gelu(c, h), w.get(P + "mlp.2.weight"), w.get(P + "mlp.2.bias"));
      x = ggml_add(c, x, ggml_mul(c, h, gate_mlp));
   }
   ggml_tensor *fm = linear(c, cs, w.get("final.adaln.1.weight"), w.get("final.adaln.1.bias"));
   ggml_tensor *fshift = ggml_view_1d(c, fm, D_MODEL, 0);
   ggml_tensor *fscale = ggml_view_1d(c, fm, D_MODEL, D_MODEL * sizeof(float));
   ggml_tensor *h = ggml_norm(c, x, 1e-6f);
   h = ggml_add(c, ggml_add(c, h, ggml_mul(c, h, fscale)), fshift);
   ggml_tensor *out = linear(c, h, w.get("final.linear.weight"), w.get("final.linear.bias"));   /* [16, 256] */
   ggml_set_output(out);
   ggml_build_forward_expand(gf, out);

   bool ok = ggml_gallocr_alloc_graph(ig->galloc, gf);
   if (ok)
   {
      /* the latent as patches: token (hh, ww), feature c*4 + p1*2 + p2 */
      std::vector<float> xp(PATCH_IN * N_TOK), tf(FREQ_DIM);
      const int G = IG_LAT / PATCH;
      for (int hh = 0; hh < G; hh++)
         for (int ww = 0; ww < G; ww++)
            for (int ch = 0; ch < LAT_CH; ch++)
               for (int p1 = 0; p1 < PATCH; p1++)
                  for (int p2 = 0; p2 < PATCH; p2++)
                     xp[(hh * G + ww) * PATCH_IN + ch * 4 + p1 * 2 + p2] =
                           ig->z[(ch * IG_LAT + hh * 2 + p1) * IG_LAT + ww * 2 + p2];
      /* the time, as cos then sin of t * 1000 at 128 frequencies */
      for (int i = 0; i < FREQ_DIM / 2; i++)
      {
         double f = exp(-log(10000.0) * i / (FREQ_DIM / 2)), a = t * f * 1000.0;
         tf[i] = (float)cos(a);
         tf[i + FREQ_DIM / 2] = (float)sin(a);
      }
      ggml_backend_tensor_set(xin, xp.data(), 0, xp.size() * 4);
      ggml_backend_tensor_set(tin, tf.data(), 0, tf.size() * 4);
      ggml_backend_tensor_set(cin, ctx.data(), 0, (size_t)n * T5_D * 4);
      ok = ggml_backend_graph_compute(ig->backend, gf) == GGML_STATUS_SUCCESS;
   }
   if (ok)
   {
      std::vector<float> o(PATCH_IN * N_TOK);
      ggml_backend_tensor_get(out, o.data(), 0, o.size() * 4);
      const int G = IG_LAT / PATCH;
      v.resize(ig->z.size());
      for (int hh = 0; hh < G; hh++)
         for (int ww = 0; ww < G; ww++)
            for (int ch = 0; ch < LAT_CH; ch++)
               for (int p1 = 0; p1 < PATCH; p1++)
                  for (int p2 = 0; p2 < PATCH; p2++)
                     v[(ch * IG_LAT + hh * 2 + p1) * IG_LAT + ww * 2 + p2] =
                           o[(hh * G + ww) * PATCH_IN + ch * 4 + p1 * 2 + p2];
   }
   ggml_free(c);
   return ok;
}

int ig_step(ig_t *ig)
{
   if (ig->done >= ig->steps)
      return ig->done;
   float dt = 1.0f / ig->steps, t = ig->done * dt;
   std::vector<float> vc, vu;
   if (!dit_run(ig, t, ig->ctx_c, ig->n_c, vc))
      return -1;
   if (ig->guidance > 1.0f)
   {
      if (!dit_run(ig, t, ig->ctx_u, ig->n_u, vu))
         return -1;
      for (size_t i = 0; i < vc.size(); i++)
         vc[i] = vu[i] + ig->guidance * (vc[i] - vu[i]);
   }
   for (size_t i = 0; i < ig->z.size(); i++)
      ig->z[i] += dt * vc[i];
   ig->v_last = vc;
   return ++ig->done;
}

/* ---------------------------------------------------------------- pictures */

/* SD-style latents to colour, roughly (a 4x3 mix, the same as ComfyUI's previews) */
static const float lat_rgb[4][3] = {
   { 0.3512f, 0.2297f, 0.3227f }, { 0.3250f, 0.4974f, 0.2350f },
   { -0.2829f, 0.1762f, 0.2721f }, { -0.2120f, -0.2616f, -0.7177f } };

void ig_preview(ig_t *ig, uint8_t *rgb)
{
   /* where the steps are heading: the latent at t = 1 along the last velocity */
   const int N = IG_LAT * IG_LAT, S = IG_LAT * 8;
   float t = ig->steps ? (float)ig->done / ig->steps : 0;
   std::vector<float> col(3 * N);
   for (int p = 0; p < N; p++)
      for (int k = 0; k < 3; k++)
      {
         float s = 0;
         for (int ch = 0; ch < 4; ch++)
            s += (ig->z[ch * N + p] + (1 - t) * ig->v_last[ch * N + p]) * lat_rgb[ch][k];
         col[k * N + p] = s;
      }
   for (int y = 0; y < S; y++)
      for (int x = 0; x < S; x++)
      {
         /* smooth (bilinear) up to 8x */
         float fx = (x + 0.5f) / 8 - 0.5f, fy = (y + 0.5f) / 8 - 0.5f;
         int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
         float ax = fx - x0, ay = fy - y0;
         int x1 = x0 + 1, y1 = y0 + 1;
         x0 = x0 < 0 ? 0 : x0;
         y0 = y0 < 0 ? 0 : y0;
         x1 = x1 >= IG_LAT ? IG_LAT - 1 : x1;
         y1 = y1 >= IG_LAT ? IG_LAT - 1 : y1;
         for (int k = 0; k < 3; k++)
         {
            const float *c = &col[k * N];
            float v = (c[y0 * IG_LAT + x0] * (1 - ax) + c[y0 * IG_LAT + x1] * ax) * (1 - ay) +
                  (c[y1 * IG_LAT + x0] * (1 - ax) + c[y1 * IG_LAT + x1] * ax) * ay;
            v = (v + 1) * 127.5f;
            rgb[(y * S + x) * 3 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
         }
      }
}

/* TAESD's decoder (madebyollin/taesd), plain C: 3x3 convolutions with padding 1 */
struct conv_job
{
   const float *in, *w, *b;
   float *out;
   int cin, cout, H, W, o0, o1, relu;
};

static void *conv_worker(void *arg)
{
   conv_job *j = (conv_job *)arg;
   const int H = j->H, W = j->W;
   std::vector<float> acc(W);
   for (int o = j->o0; o < j->o1; o++)
   {
      float *dst = j->out + (size_t)o * H * W;
      for (int y = 0; y < H; y++)
      {
         float bias = j->b ? j->b[o] : 0;
         for (int x = 0; x < W; x++)
            acc[x] = bias;
         for (int i = 0; i < j->cin; i++)
         {
            const float *src = j->in + (size_t)i * H * W, *k = j->w + ((size_t)o * j->cin + i) * 9;
            for (int dy = -1; dy <= 1; dy++)
            {
               int yy = y + dy;
               if (yy < 0 || yy >= H)
                  continue;
               const float *row = src + (size_t)yy * W;
               float k0 = k[(dy + 1) * 3], k1 = k[(dy + 1) * 3 + 1], k2 = k[(dy + 1) * 3 + 2];
               acc[0] += k1 * row[0] + (W > 1 ? k2 * row[1] : 0);
               for (int x = 1; x < W - 1; x++)
                  acc[x] += k0 * row[x - 1] + k1 * row[x] + k2 * row[x + 1];
               if (W > 1)
                  acc[W - 1] += k0 * row[W - 2] + k1 * row[W - 1];
            }
         }
         for (int x = 0; x < W; x++)
            dst[(size_t)y * W + x] = j->relu && acc[x] < 0 ? 0 : acc[x];
      }
   }
   return nullptr;
}

static void conv3(int threads, const float *in, float *out, const float *w, const float *b,
      int cin, int cout, int H, int W, int relu)
{
   std::vector<pthread_t> th(threads);
   std::vector<conv_job> jobs(threads);
   for (int i = 0; i < threads; i++)
   {
      jobs[i] = { in, w, b, out, cin, cout, H, W, cout * i / threads, cout * (i + 1) / threads, relu };
      pthread_create(&th[i], nullptr, conv_worker, &jobs[i]);
   }
   for (int i = 0; i < threads; i++)
      pthread_join(th[i], nullptr);
}

int ig_decode(ig_t *ig, uint8_t *rgb)
{
   ig->dit.free_all();   /* room for the decoder's pictures */
   weights td;
   ggml_backend_t cpu = ig->backend;
   if (!load_weights(td, path_of(ig, "taesd").c_str(), cpu))
   {
      ig->err = "The decoder (taesd.bin) won't load.";
      return -1;
   }
   std::map<std::string, std::vector<float>> W;
   for (auto &kv : td.t)
   {
      std::vector<float> &v = W[kv.first];
      v.resize(ggml_nelements(kv.second));
      ggml_backend_tensor_get(kv.second, v.data(), 0, v.size() * 4);
   }
   td.free_all();
   auto wt = [&](const std::string &n) -> const float * {
      auto it = W.find(n);
      return it == W.end() ? nullptr : it->second.data();
   };

   int H = IG_LAT, Wd = IG_LAT;
   const size_t big = (size_t)64 * IG_SIZE * IG_SIZE;
   std::vector<float> a(big), b(big), c(big);
   /* Clamp: tanh(z / 3) * 3 */
   for (int i = 0; i < LAT_CH * H * Wd; i++)
      c[i] = tanhf(ig->z[i] / 3) * 3;
   conv3(ig->threads, c.data(), a.data(), wt("1.weight"), wt("1.bias"), 4, 64, H, Wd, 1);
   /* layer numbers in the decoder's Sequential: blocks, upsamples, convs */
   static const int layout[] = { 3, 4, 5, -1, 7, 8, 9, 10, -1, 12, 13, 14, 15, -1, 17, 18 };
   for (int L : layout)
   {
      if (L == -1)
      {
         /* nearest-neighbour upsample x2 */
         for (int ch = 0; ch < 64; ch++)
            for (int y = 0; y < H * 2; y++)
               for (int x = 0; x < Wd * 2; x++)
                  b[((size_t)ch * H * 2 + y) * Wd * 2 + x] = a[((size_t)ch * H + y / 2) * Wd + x / 2];
         H *= 2;
         Wd *= 2;
         std::swap(a, b);
         continue;
      }
      char n0[32], n1[32], n2[32], nb0[32], nb1[32], nb2[32], plain[32];
      snprintf(plain, sizeof(plain), "%d.weight", L);
      if (wt(plain))
      {
         /* a plain conv (after each upsample, no bias) */
         conv3(ig->threads, a.data(), b.data(), wt(plain), nullptr, 64, 64, H, Wd, 0);
         std::swap(a, b);
         continue;
      }
      /* a Block: relu(conv(relu(conv(relu(conv(x))))) + x) */
      snprintf(n0, sizeof(n0), "%d.conv.0.weight", L);
      snprintf(nb0, sizeof(nb0), "%d.conv.0.bias", L);
      snprintf(n1, sizeof(n1), "%d.conv.2.weight", L);
      snprintf(nb1, sizeof(nb1), "%d.conv.2.bias", L);
      snprintf(n2, sizeof(n2), "%d.conv.4.weight", L);
      snprintf(nb2, sizeof(nb2), "%d.conv.4.bias", L);
      conv3(ig->threads, a.data(), b.data(), wt(n0), wt(nb0), 64, 64, H, Wd, 1);
      conv3(ig->threads, b.data(), c.data(), wt(n1), wt(nb1), 64, 64, H, Wd, 1);
      conv3(ig->threads, c.data(), b.data(), wt(n2), wt(nb2), 64, 64, H, Wd, 0);
      size_t n = (size_t)64 * H * Wd;
      for (size_t i = 0; i < n; i++)
      {
         float v = b[i] + a[i];
         a[i] = v < 0 ? 0 : v;
      }
   }
   conv3(ig->threads, a.data(), b.data(), wt("19.weight"), wt("19.bias"), 64, 3, H, Wd, 0);
   for (int y = 0; y < IG_SIZE; y++)
      for (int x = 0; x < IG_SIZE; x++)
         for (int k = 0; k < 3; k++)
         {
            float v = b[((size_t)k * IG_SIZE + y) * IG_SIZE + x] * 255.0f;
            rgb[(y * IG_SIZE + x) * 3 + k] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
         }
   return 0;
}
