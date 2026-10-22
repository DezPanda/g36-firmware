/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/*
 * g36-splash - draws the G36 boot splash: "G36" in the menu's lettering, given a deep
 * 3D extrusion, on the menu's lit background.
 *
 *   g36-splash <font.ttf> <out.bmp> [preview.ppm]
 *
 * The BMP is what the boot logo needs: 480x854, 24-bit, top-down, turned to suit the
 * portrait panel (the same turn the menu applies). The PPM is the landscape picture,
 * for looking at on a computer.
 */
#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 854
#define H 480

static float img[H][W][3];   /* the picture, 0..255 per channel */

static void put(int x, int y, float r, float g, float b, float a)
{
   if (x < 0 || y < 0 || x >= W || y >= H || a <= 0)
      return;
   if (a > 1)
      a = 1;
   img[y][x][0] += (r - img[y][x][0]) * a;
   img[y][x][1] += (g - img[y][x][1]) * a;
   img[y][x][2] += (b - img[y][x][2]) * a;
}

/* ---- the menu's background: dark, lit softly from the top and bottom middle */
static void background(void)
{
   const float top[3] = { 0x24, 0x15, 0x2b }, bot[3] = { 0x13, 0x0b, 0x17 };
   const float glow[3] = { 0x56, 0x28, 0x76 }, core[3] = { 0x7c, 0x3a, 0xa8 };
   const float L[2][7] = {
      { -H * 0.08f, W * 0.93f, H * 0.95f, 0.45f, W * 0.45f, H * 0.50f, 0.55f },
      { H - 40.0f, W * 0.825f, H * 0.70f, 0.32f, W * 0.39f, H * 0.34f, 0.45f } };
   int x, y, k, l;
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float col[3];
         for (k = 0; k < 3; k++)
            col[k] = top[k] + (bot[k] - top[k]) * ((float)y / H);
         for (l = 0; l < 2; l++)
         {
            float dx = (x - W * 0.5f) / L[l][1], dy = (y - L[l][0]) / L[l][2];
            float cx = (x - W * 0.5f) / L[l][4], cy = (y - L[l][0]) / L[l][5];
            float a = 1 - (dx * dx + dy * dy), b = 1 - (cx * cx + cy * cy);
            a = a > 0 ? a * a * L[l][3] : 0;
            b = b > 0 ? b * b * L[l][6] : 0;
            for (k = 0; k < 3; k++)
            {
               col[k] += (glow[k] - col[k]) * a;
               col[k] += (core[k] - col[k]) * b;
            }
         }
         for (k = 0; k < 3; k++)
            img[y][x][k] = col[k];
      }
}

/* ---- masks: coverage 0..1, the size of the picture */
typedef float mask_t[H][W];

static float at(mask_t m, int x, int y)
{
   return x < 0 || y < 0 || x >= W || y >= H ? 0 : m[y][x];
}

/* between pixels across (for the extrusion's fractional steps) */
static float atx(mask_t m, float x, int y)
{
   int i = (int)floorf(x);
   float f = x - i;
   return at(m, i, y) * (1 - f) + at(m, i + 1, y) * f;
}

/* thicker by r pixels, with round corners */
static void dilate(mask_t in, mask_t out, int r)
{
   int x, y, i, j;
   memset(out, 0, sizeof(mask_t));
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float a = in[y][x];
         if (a <= 0)
            continue;
         for (j = -r; j <= r; j++)
            for (i = -r; i <= r; i++)
            {
               float d = sqrtf((float)(i * i + j * j)), v;
               if (d > r + 0.5f)
                  continue;
               v = d > r - 0.5f ? a * (r + 0.5f - d) : a;
               if (x + i >= 0 && x + i < W && y + j >= 0 && y + j < H && out[y + j][x + i] < v)
                  out[y + j][x + i] = v;
            }
      }
}

static void blur(mask_t m, int r)
{
   static float tmp[H][W];
   int x, y, k;
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float s = 0;
         for (k = -r; k <= r; k++)
            s += at(m, x + k, y);
         tmp[y][x] = s / (2 * r + 1);
      }
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float s = 0;
         for (k = -r; k <= r; k++)
            s += y + k >= 0 && y + k < H ? tmp[y + k][x] : 0;
         m[y][x] = s / (2 * r + 1);
      }
}

static mask_t face, rim, solid, shadow;
static float ext[H][W][3];   /* the extrusion, drawn apart so it can be smoothed */

/* the gradient down the letters, as on the menu's cards */
static void logo_colour(float t, float c[3])
{
   static const struct { float at; unsigned c; } st[5] = {
      { 0, 0xc07dff }, { .35f, 0xff7fe6 }, { .55f, 0xff5a8f }, { .75f, 0xff944c }, { 1, 0xffd816 } };
   int i;
   if (t < 0) t = 0;
   if (t > 1) t = 1;
   for (i = 0; i < 4 && t > st[i + 1].at; i++)
      ;
   {
      float f = (t - st[i].at) / (st[i + 1].at - st[i].at);
      unsigned a = st[i].c, b = st[i + 1].c;
      c[0] = (a >> 16 & 255) + (((b >> 16 & 255) - (float)(a >> 16 & 255)) * f);
      c[1] = (a >> 8 & 255) + (((b >> 8 & 255) - (float)(a >> 8 & 255)) * f);
      c[2] = (a & 255) + (((b & 255) - (float)(a & 255)) * f);
   }
}

/* a four-pointed sparkle: each ray tapers smoothly from the centre to a fine point,
 * around a small soft glow */
static float ray(float along, float across, float r)
{
   float t = along / r, w;
   if (t >= 1)
      return 0;
   w = r * 0.11f * powf(1 - t, 1.6f) + 0.35f;   /* the ray's half-width here */
   return expf(-(across * across) / (w * w)) * powf(1 - t, 0.8f);
}

static void sparkle(int cx, int cy, int r, float a)
{
   int x, y;
   for (y = -r; y <= r; y++)
      for (x = -r; x <= r; x++)
      {
         float fx = fabsf((float)x), fy = fabsf((float)y);
         float v = ray(fx, fy, r), h = ray(fy, fx, r);
         if (h > v)
            v = h;
         v += expf(-(x * x + y * y) / (r * r * 0.012f)) * 0.8f;   /* the bright middle */
         v += expf(-(x * x + y * y) / (r * r * 0.08f)) * 0.25f;   /* its soft glow */
         put(cx + x, cy + y, 255, 255, 255, v * a);
      }
}

int main(int argc, char **argv)
{
   font_t *f;
   canvas_t m;
   int x, y, k, tw, fh, x0, y0, x1, y1;
   const int depth = 40;                 /* extrusion layers, each one down a pixel */
   const float ex = 0.42f;               /* ...and this far right */
   const char *text = "G36";

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s font.ttf out.bmp [preview.ppm]\n", argv[0]);
      return 2;
   }
   if (!(f = font_load(argv[1], 270)))
   {
      fprintf(stderr, "can't load %s\n", argv[1]);
      return 1;
   }

   /* the letters, white on black, centred a little above the middle (the extrusion
    * hangs below them) */
   m.w = m.stride = W;
   m.h = H;
   m.px = calloc((size_t)W * H, 4);
   tw = font_text_width(f, text);
   fh = font_height(f);
   gfx_fill_rect(&m, 0, 0, W, H, 0xff000000);
   gfx_draw_text(&m, f, text, W / 2 - tw / 2, H / 2 - fh / 2 - 22, 0xffffffff, 0);
   x0 = W; y0 = H; x1 = y1 = -1;
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         face[y][x] = (m.px[(size_t)y * W + x] & 0xff) / 255.0f;
         if (face[y][x] > 0)
         {
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
         }
      }
   {
      /* centre the letters plus their extrusion */
      static mask_t moved;
      int sx = W / 2 - (x0 + x1 + (int)(depth * ex)) / 2, sy = H / 2 - (y0 + y1 + depth) / 2 - 6;
      memset(moved, 0, sizeof(moved));
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
            if (face[y][x] > 0 && x + sx >= 0 && x + sx < W && y + sy >= 0 && y + sy < H)
               moved[y + sy][x + sx] = face[y][x];
      memcpy(face, moved, sizeof(mask_t));
      y0 += sy; y1 += sy;
   }

   background();

   /* the white rim around the face, and the whole 3D shape (rim swept back) */
   dilate(face, rim, 7);
   memset(solid, 0, sizeof(solid));
   for (k = 0; k <= depth; k++)
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
         {
            float a = atx(rim, x - k * ex, y - k);
            if (a > solid[y][x])
               solid[y][x] = a;
         }

   /* a glow behind it all, and a soft shadow on the "floor" */
   memcpy(shadow, solid, sizeof(mask_t));
   blur(shadow, 34);
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
         put(x, y, 0xff, 0x4f, 0xd8, shadow[y][x] * 0.7f);
   memset(shadow, 0, sizeof(shadow));
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
         shadow[y][x] = at(solid, x - 14, y - 18);
   blur(shadow, 14);
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
         put(x, y, 0x08, 0x02, 0x10, shadow[y][x] * 0.75f);

   /* a dark outline around the 3D shape, so it lifts off the background */
   {
      static mask_t edge;
      dilate(solid, edge, 4);
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
            put(x, y, 0x12, 0x05, 0x24, edge[y][x]);
   }

   /* the extrusion, back to front: violet near the face, nearly black at the back,
    * with a lighter band where each side catches the light from above */
   for (k = depth; k >= 1; k--)
   {
      /* bright near the face, falling off quickly into the dark */
      float t = powf((float)k / depth, 0.55f);
      float r = 0x9a + (0x0c - 0x9a) * t, g = 0x50 + (0x02 - 0x50) * t, b = 0xff + (0x22 - 0xff) * t;
      float dx = k * ex;
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
         {
            float a = atx(rim, x - dx, y - k);
            if (a <= 0)
               continue;
            /* the side's top edge (the shape above is empty) is lit; its lower part shaded */
            {
               /* faces: the tops of lower strokes catch the light; sides facing right
                * are in shade, sides facing down in between */
               float up = atx(rim, x - dx, y - k - 2), right = atx(rim, x - dx + 3, y - k);
               float lit = 1.0f + 0.55f * (1 - up) - 0.38f * (1 - right) * up;
               float c3[3] = { r * lit, g * lit, b * lit }, *e = ext[y][x];
               int q;
               for (q = 0; q < 3; q++)
                  e[q] += ((c3[q] > 255 ? 255 : c3[q]) - e[q]) * (a > 1 ? 1 : a);
            }
         }
   }
   {
      /* soften the extrusion's shading a little (the layers' edges leave a grain), then
       * lay it over the shape it fills */
      static float sm[H][W][3];
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
         {
            float sum[3] = { 0, 0, 0 }, n = 0;
            int i, j, q;
            if (solid[y][x] <= 0)
               continue;
            for (j = -1; j <= 1; j++)
               for (i = -2; i <= 2; i++)
                  if (at(solid, x + i, y + j) > 0.5f)
                  {
                     for (q = 0; q < 3; q++)
                        sum[q] += ext[y + j][x + i][q];
                     n++;
                  }
            for (q = 0; q < 3; q++)
               sm[y][x][q] = n ? sum[q] / n : ext[y][x][q];
         }
      for (y = 0; y < H; y++)
         for (x = 0; x < W; x++)
            put(x, y, sm[y][x][0], sm[y][x][1], sm[y][x][2], solid[y][x]);
   }

   /* the seam between the extrusion and the rim: a thin bright line */
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float a = at(rim, x - 1, y - 2) * (1 - rim[y][x]);
         put(x, y, 0xd9, 0xb6, 0xff, a * 0.9f);
      }

   /* the white rim, faintly lavender at the bottom */
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float t = y1 > y0 ? (float)(y - y0) / (y1 - y0) : 0;
         put(x, y, 255 - 22 * t, 255 - 30 * t, 255, rim[y][x]);
      }

   /* the face: the logo gradient, a bevel (lit top-left edge, shaded bottom-right), a
    * gloss across the top half */
   for (y = 0; y < H; y++)
      for (x = 0; x < W; x++)
      {
         float a = face[y][x], c[3], t, hl, sh, gloss;
         int i;
         if (a <= 0)
            continue;
         t = (float)(y - y0) / (y1 - y0);
         logo_colour(t, c);
         hl = 0, sh = 0;
         for (i = 1; i <= 4; i++)
         {
            hl += (1 - at(face, x - i, y - i)) / 4;
            sh += (1 - at(face, x + i, y + i)) / 4;
         }
         gloss = t < 0.42f ? 0.16f * (1 - t / 0.42f) + 0.04f : 0;
         for (i = 0; i < 3; i++)
         {
            float v = c[i];
            v += (255 - v) * (hl * 0.55f + gloss);
            v *= 1 - sh * 0.38f;
            c[i] = v;
         }
         put(x, y, c[0], c[1], c[2], a);
      }

   /* sparkles on two corners of the letters */
   sparkle(x0 + 26, y0 + 18, 26, 0.95f);
   sparkle(x1 - 18, y1 - 30, 16, 0.75f);

   /* ---- write the boot logo: turned onto the portrait panel, top-down, BGR */
   {
      FILE *o = fopen(argv[2], "wb");
      unsigned char hdr[54] = { 'B', 'M' };
      int pw = H, ph = W, row = pw * 3;
      unsigned size = 54 + (unsigned)row * ph;
      unsigned char *line = malloc((size_t)row);
      if (!o || !line)
         return 1;
#define P32(o, v) do { hdr[o] = (v) & 255; hdr[o + 1] = (v) >> 8 & 255; hdr[o + 2] = (v) >> 16 & 255; hdr[o + 3] = (unsigned)(v) >> 24 & 255; } while (0)
      P32(2, size);
      P32(10, 54);
      P32(14, 40);
      P32(18, pw);
      P32(22, -ph);    /* negative: rows top to bottom, like the factory logo */
      hdr[26] = 1;
      hdr[28] = 24;
      P32(34, (unsigned)row * ph);
      P32(38, 2835);
      P32(42, 2835);
      fwrite(hdr, 1, 54, o);
      /* portrait (px, py) shows landscape (W - 1 - py, px) */
      for (y = 0; y < ph; y++)
      {
         for (x = 0; x < pw; x++)
         {
            float *p = img[x][W - 1 - y];
            int k2;
            for (k2 = 0; k2 < 3; k2++)
            {
               float v = p[2 - k2] + ((x * 7 + y * 13) % 16) / 16.0f - 0.5f;   /* a little dither */
               line[x * 3 + k2] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f);
            }
         }
         fwrite(line, 1, (size_t)row, o);
      }
      fclose(o);
   }
   if (argc > 3)
   {
      FILE *o = fopen(argv[3], "wb");
      if (o)
      {
         fprintf(o, "P6\n%d %d\n255\n", W, H);
         for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
               for (k = 0; k < 3; k++)
               {
                  float v = img[y][x][k];
                  fputc(v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f), o);
               }
         fclose(o);
      }
   }
   return 0;
}
