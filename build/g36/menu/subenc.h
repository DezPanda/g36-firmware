/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* Text encodings for subtitle files beside a video.
 *
 * FFmpeg takes subtitle files as UTF-8 unless it is told otherwise, and nothing tells it, so a file
 * saved in the encoding most Windows subtitle tools use (Windows-1252: é ñ ü ß and curly quotes)
 * came out with the accented letters missing or wrong. subenc_to_utf8() looks at the bytes:
 *   - UTF-8 (the byte order mark is fine) and plain ASCII are used as they are (returns NULL);
 *   - UTF-16, little or big endian, with its byte order mark, is turned into UTF-8;
 *   - anything else that isn't valid UTF-8 is taken to be Windows-1252 and turned into UTF-8.
 * Other encodings (Cyrillic, Greek, Hebrew, Arabic, Chinese, Japanese ...) should be saved as UTF-8.
 * A header of static functions so a test program can use it on its own. */
#ifndef G36_SUBENC_H
#define G36_SUBENC_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static int subenc_utf8_valid(const unsigned char *s, size_t n)
{
   size_t i = 0;
   while (i < n)
   {
      unsigned char c = s[i], lo = 0x80, hi = 0xbf;
      int extra;
      if (c < 0x80)
      {
         i++;
         continue;
      }
      if (c >= 0xc2 && c <= 0xdf)
         extra = 1;
      else if (c >= 0xe0 && c <= 0xef)
      {
         extra = 2;
         if (c == 0xe0)
            lo = 0xa0;      /* no overlong forms */
         else if (c == 0xed)
            hi = 0x9f;      /* no surrogates */
      }
      else if (c >= 0xf0 && c <= 0xf4)
      {
         extra = 3;
         if (c == 0xf0)
            lo = 0x90;
         else if (c == 0xf4)
            hi = 0x8f;      /* nothing above U+10FFFF */
      }
      else
         return 0;
      if (i + (size_t)extra >= n)
         return 0;          /* cut off in the middle of a letter */
      if (s[i + 1] < lo || s[i + 1] > hi)
         return 0;
      for (int k = 2; k <= extra; k++)
         if ((s[i + k] & 0xc0) != 0x80)
            return 0;
      i += (size_t)extra + 1;
   }
   return 1;
}

/* one code point as UTF-8; returns the new end of the output */
static unsigned char *subenc_put(unsigned char *o, uint32_t cp)
{
   if (cp < 0x80)
      *o++ = (unsigned char)cp;
   else if (cp < 0x800)
   {
      *o++ = (unsigned char)(0xc0 | cp >> 6);
      *o++ = (unsigned char)(0x80 | (cp & 0x3f));
   }
   else if (cp < 0x10000)
   {
      *o++ = (unsigned char)(0xe0 | cp >> 12);
      *o++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
      *o++ = (unsigned char)(0x80 | (cp & 0x3f));
   }
   else
   {
      *o++ = (unsigned char)(0xf0 | cp >> 18);
      *o++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
      *o++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
      *o++ = (unsigned char)(0x80 | (cp & 0x3f));
   }
   return o;
}

/* Windows-1252 0x80 - 0x9f (the rest of 0x80 - 0xff is the same as Unicode); 0 = unused, shown as it is */
static const uint16_t subenc_cp1252_hi[32] = {
   0x20ac, 0, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017d, 0,
   0, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0, 0x017e, 0x0178
};

/* NULL if the bytes can be used as they are; else a malloc'd UTF-8 copy, its length in *out_len */
static unsigned char *subenc_to_utf8(const unsigned char *s, size_t n, size_t *out_len)
{
   unsigned char *out, *o;
   size_t i;

   if (n >= 2 && ((s[0] == 0xff && s[1] == 0xfe) || (s[0] == 0xfe && s[1] == 0xff)))
   {
      int be = s[0] == 0xfe;
      if (!(out = malloc(n * 2 + 8)))
         return NULL;
      o = out;
      for (i = 2; i + 1 < n; i += 2)
      {
         uint32_t cp = be ? (uint32_t)(s[i] << 8 | s[i + 1]) : (uint32_t)(s[i + 1] << 8 | s[i]);
         if (cp >= 0xd800 && cp < 0xdc00 && i + 3 < n)
         {
            uint32_t lo = be ? (uint32_t)(s[i + 2] << 8 | s[i + 3]) : (uint32_t)(s[i + 3] << 8 | s[i + 2]);
            if (lo >= 0xdc00 && lo < 0xe000)
            {
               cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
               i += 2;
            }
            else
               cp = 0xfffd;
         }
         else if (cp >= 0xd800 && cp < 0xe000)
            cp = 0xfffd;
         o = subenc_put(o, cp);
      }
      *out_len = (size_t)(o - out);
      return out;
   }
   if (subenc_utf8_valid(s, n))
      return NULL;
   if (!(out = malloc(n * 3 + 8)))
      return NULL;
   o = out;
   for (i = 0; i < n; i++)
   {
      uint32_t cp = s[i];
      if (cp >= 0x80 && cp < 0xa0 && subenc_cp1252_hi[cp - 0x80])
         cp = subenc_cp1252_hi[cp - 0x80];
      o = subenc_put(o, cp);
   }
   *out_len = (size_t)(o - out);
   return out;
}

#endif
