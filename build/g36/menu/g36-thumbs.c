/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/*
 * g36-thumbs - shrink the images waiting in thumbnails/to-resize/ for the G36 menu.
 *
 * Each image (PNG or JPEG, any size and shape) is cropped to a centred square,
 * shrunk to THUMB x THUMB by averaging, saved as a PNG under thumbnails/resized/
 * (same subfolder and name, .png, replacing an older copy) and then deleted from
 * the inbox. The menu starts this whenever the inbox isn't empty.
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define THUMBS   "/mnt/sdcard/thumbnails"
#define INBOX    THUMBS "/to-resize"
#define OUTDIR   THUMBS "/resized"
#define LARGEDIR THUMBS "/large"     /* the details panel's picture: fitted, not cropped */
#define LARGE_W  260
#define LARGE_H  195
#define THUMB    64
#define MAX_PIXELS (25 * 1000 * 1000)   /* bigger pictures are skipped (memory) */

/* ---- a small PNG writer: RGBA, deflate "stored" blocks (thumbnails are tiny) */
static uint32_t crc_table[256];

static void crc_init(void)
{
   uint32_t c, n, k;
   for (n = 0; n < 256; n++)
   {
      for (c = n, k = 0; k < 8; k++)
         c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
      crc_table[n] = c;
   }
}

static uint32_t crc(uint32_t c, const unsigned char *p, size_t n)
{
   c ^= 0xffffffffu;
   while (n--)
      c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
   return c ^ 0xffffffffu;
}

static void put32(unsigned char *p, uint32_t v)
{
   p[0] = v >> 24;
   p[1] = v >> 16;
   p[2] = v >> 8;
   p[3] = v;
}

static int write_chunk(FILE *f, const char *type, const unsigned char *data, uint32_t len)
{
   unsigned char hdr[8], *buf;
   uint32_t c;
   /* the CRC covers the chunk type and its data */
   if (!(buf = malloc(len + 4)))
      return -1;
   memcpy(buf, type, 4);
   if (len)
      memcpy(buf + 4, data, len);
   c = crc(0, buf, len + 4);
   free(buf);
   put32(hdr, len);
   memcpy(hdr + 4, type, 4);
   if (fwrite(hdr, 1, 8, f) != 8 || (len && fwrite(data, 1, len, f) != len))
      return -1;
   put32(hdr, c);
   return fwrite(hdr, 1, 4, f) == 4 ? 0 : -1;
}

static int write_png(const char *path, const uint32_t *px, int w, int h)
{
   static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
   size_t raw_len = (size_t)(w * 4 + 1) * h, nblocks = (raw_len + 65534) / 65535;
   size_t z_len = 2 + raw_len + nblocks * 5 + 4, i, o = 0;
   unsigned char ihdr[13], *raw = malloc(raw_len), *z = malloc(z_len);
   uint32_t a = 1, b = 0;
   char tmp[1100];
   FILE *f;
   int x, y, ok = -1;

   if (!raw || !z)
      goto done;
   for (y = 0; y < h; y++)
   {
      unsigned char *r = raw + (size_t)y * (w * 4 + 1);
      *r++ = 0;   /* filter: none */
      for (x = 0; x < w; x++)
      {
         uint32_t p = px[(size_t)y * w + x];
         *r++ = p >> 16;
         *r++ = p >> 8;
         *r++ = p;
         *r++ = p >> 24;
      }
   }
   /* zlib: header, stored blocks, Adler-32 */
   z[o++] = 0x78;
   z[o++] = 0x01;
   for (i = 0; i < raw_len; i += 65535)
   {
      size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
      z[o++] = i + n >= raw_len;
      z[o++] = n & 0xff;
      z[o++] = n >> 8;
      z[o++] = ~n & 0xff;
      z[o++] = (~n >> 8) & 0xff;
      memcpy(z + o, raw + i, n);
      o += n;
   }
   for (i = 0; i < raw_len; i++)
   {
      a = (a + raw[i]) % 65521;
      b = (b + a) % 65521;
   }
   put32(z + o, b << 16 | a);
   o += 4;

   put32(ihdr, w);
   put32(ihdr + 4, h);
   ihdr[8] = 8;    /* bit depth */
   ihdr[9] = 6;    /* RGBA */
   ihdr[10] = ihdr[11] = ihdr[12] = 0;

   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   if (!(f = fopen(tmp, "wb")))
      goto done;
   if (fwrite(sig, 1, 8, f) == 8 && write_chunk(f, "IHDR", ihdr, 13) == 0 &&
         write_chunk(f, "IDAT", z, (uint32_t)o) == 0 && write_chunk(f, "IEND", NULL, 0) == 0)
      ok = 0;
   if (fclose(f) != 0)
      ok = -1;
   if (ok == 0)
      ok = rename(tmp, path);
   else
      unlink(tmp);
done:
   free(raw);
   free(z);
   return ok;
}

/* centred square crop, averaged down to THUMB x THUMB */
static void shrink(const unsigned char *rgba, int w, int h, uint32_t *out)
{
   int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2, x, y;
   for (y = 0; y < THUMB; y++)
      for (x = 0; x < THUMB; x++)
      {
         int sx0 = x0 + x * side / THUMB, sx1 = x0 + (x + 1) * side / THUMB;
         int sy0 = y0 + y * side / THUMB, sy1 = y0 + (y + 1) * side / THUMB, i, j;
         uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
         if (sx1 <= sx0)
            sx1 = sx0 + 1;
         if (sy1 <= sy0)
            sy1 = sy0 + 1;
         for (j = sy0; j < sy1; j++)
            for (i = sx0; i < sx1; i++)
            {
               const unsigned char *p = rgba + ((size_t)j * w + i) * 4;
               r += p[0];
               g += p[1];
               b += p[2];
               a += p[3];
               n++;
            }
         out[y * THUMB + x] = (uint32_t)(a / n) << 24 | (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
      }
}

/* fitted inside LARGE_W x LARGE_H, keeping its shape (averaged down; never enlarged) */
static uint32_t *fit_large(const unsigned char *rgba, int w, int h, int *ow, int *oh)
{
   double s = (double)LARGE_W / w < (double)LARGE_H / h ? (double)LARGE_W / w : (double)LARGE_H / h;
   int x, y, W, H;
   uint32_t *out;
   if (s > 1)
      s = 1;
   W = (int)(w * s);
   H = (int)(h * s);
   if (W < 1) W = 1;
   if (H < 1) H = 1;
   if (!(out = malloc((size_t)W * H * 4)))
      return NULL;
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         int sx0 = x * w / W, sx1 = (x + 1) * w / W, sy0 = y * h / H, sy1 = (y + 1) * h / H, i, j;
         uint64_t r = 0, g = 0, b = 0, a = 0, n = 0;
         if (sx1 <= sx0) sx1 = sx0 + 1;
         if (sy1 <= sy0) sy1 = sy0 + 1;
         for (j = sy0; j < sy1; j++)
            for (i = sx0; i < sx1; i++)
            {
               const unsigned char *p = rgba + ((size_t)j * w + i) * 4;
               r += p[0]; g += p[1]; b += p[2]; a += p[3]; n++;
            }
         out[y * W + x] = (uint32_t)(a / n) << 24 | (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
      }
   *ow = W;
   *oh = H;
   return out;
}

static int done_count, failed;

static void handle(const char *rel)
{
   char in[1100], out[1100], *dot;
   unsigned char *rgba;
   uint32_t px[THUMB * THUMB];
   int w, h, n;

   snprintf(in, sizeof(in), "%s/%s", INBOX, rel);
   snprintf(out, sizeof(out), "%s/%s", OUTDIR, rel);
   if ((dot = strrchr(out, '.')) && dot > strrchr(out, '/'))
      *dot = 0;
   strncat(out, ".png", sizeof(out) - strlen(out) - 1);

   if (!stbi_info(in, &w, &h, &n) || (long long)w * h > MAX_PIXELS)
   {
      fprintf(stderr, "skipped (not a picture, or too big): %s\n", rel);
      failed++;
      return;
   }
   if (!(rgba = stbi_load(in, &w, &h, &n, 4)))
   {
      fprintf(stderr, "couldn't read: %s\n", rel);
      failed++;
      return;
   }
   shrink(rgba, w, h, px);
   {
      /* and the larger copy for the details panel */
      char large[1100], dir[1100], *slash;
      int lw, lh;
      uint32_t *lp = fit_large(rgba, w, h, &lw, &lh);
      snprintf(large, sizeof(large), "%s/%s", LARGEDIR, out + strlen(OUTDIR) + 1);
      snprintf(dir, sizeof(dir), "%s", large);
      if ((slash = strrchr(dir, '/')))
      {
         *slash = 0;
         mkdir(dir, 0755);
      }
      if (lp)
      {
         write_png(large, lp, lw, lh);
         free(lp);
      }
   }
   stbi_image_free(rgba);
   {
      /* the subfolder in resized/ */
      char dir[1100], *slash;
      snprintf(dir, sizeof(dir), "%s", out);
      if ((slash = strrchr(dir, '/')))
      {
         *slash = 0;
         mkdir(dir, 0755);
      }
   }
   if (write_png(out, px, THUMB, THUMB) == 0)
   {
      unlink(in);
      done_count++;
   }
   else
   {
      fprintf(stderr, "couldn't write: %s\n", out);
      failed++;
   }
}

/* the inbox, and one level of subfolders (per console) */
static void walk(const char *rel, int depth)
{
   char dir[1100];
   DIR *d;
   struct dirent *e;
   snprintf(dir, sizeof(dir), "%s%s%s", INBOX, rel[0] ? "/" : "", rel);
   if (!(d = opendir(dir)))
      return;
   while ((e = readdir(d)))
   {
      char child[1100], path[1300];
      struct stat st;
      if (e->d_name[0] == '.')
         continue;
      snprintf(child, sizeof(child), "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
      snprintf(path, sizeof(path), "%s/%s", INBOX, child);
      if (stat(path, &st) < 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         if (depth < 1)
         {
            char od[1300];
            snprintf(od, sizeof(od), "%s/%s", OUTDIR, child);
            mkdir(od, 0755);
            walk(child, depth + 1);
         }
      }
      else
         handle(child);
   }
   closedir(d);
}

int main(void)
{
   crc_init();
   mkdir(OUTDIR, 0755);
   mkdir(LARGEDIR, 0755);
   walk("", 0);
   sync();
   fprintf(stderr, "%d resized, %d skipped\n", done_count, failed);
   return 0;
}
