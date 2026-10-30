/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* Drawing core for the G36 menu. See gfx.h. */
#include "gfx.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "../board.h"

#include <rga/RgaApi.h>
#include <rga/rga.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

/* ---------------------------------------------------------------- display */

typedef struct
{
   uint32_t w, h, pitch, handle, fb_id, fb_id_x;
   int prime_fd;
   size_t size;
   uint8_t *map;
} dumb_t;

static struct
{
   int fd;
   uint32_t crtc_id, conn_id;
   drmModeModeInfo mode;
   dumb_t canvas_gpu;   /* landscape copy of the canvas the RGA reads from */
   dumb_t pages[2];     /* portrait scanout buffers */
   int cur;
   canvas_t canvas;     /* landscape canvas in normal RAM (fast CPU reads) */
   int dbg_cpu, dbg_xrgb;
   uint32_t dpms_prop;
   int crtc_set;        /* the first frame sets the mode; later ones page-flip */
   int flip_pending;    /* a flip is queued and the other page may still be on screen */
} d;

static void flip_done(int fd, unsigned int seq, unsigned int sec, unsigned int usec, void *data)
{
   (void)fd; (void)seq; (void)sec; (void)usec; (void)data;
   d.flip_pending = 0;
}

/* wait for the queued flip, so the page about to be drawn is no longer on screen */
static void flip_wait(void)
{
   drmEventContext ev = { 0 };
   ev.version = 2;
   ev.page_flip_handler = flip_done;
   while (d.flip_pending)
   {
      struct pollfd p = { d.fd, POLLIN, 0 };
      if (poll(&p, 1, 100) <= 0)
      {
         d.flip_pending = 0;   /* no answer (screen off?): don't hang */
         break;
      }
      drmHandleEvent(d.fd, &ev);
   }
}

static int dumb_create(dumb_t *b, uint32_t w, uint32_t h, int scanout)
{
   struct drm_mode_create_dumb creq = { 0 };
   struct drm_mode_map_dumb mreq    = { 0 };
   uint32_t handles[4] = { 0 }, pitches[4] = { 0 }, offsets[4] = { 0 };

   creq.width  = w;
   creq.height = h;
   creq.bpp    = 32;
   if (drmIoctl(d.fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
      return -1;

   b->w = w; b->h = h;
   b->pitch = creq.pitch; b->handle = creq.handle; b->size = creq.size;

   mreq.handle = b->handle;
   if (drmIoctl(d.fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
      return -1;
   b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, d.fd, mreq.offset);
   if (b->map == MAP_FAILED)
      return -1;
   memset(b->map, 0, b->size);

   if (drmPrimeHandleToFD(d.fd, b->handle, DRM_CLOEXEC | DRM_RDWR, &b->prime_fd) < 0)
      return -1;

   if (scanout)
   {
      handles[0] = b->handle;
      pitches[0] = b->pitch;
      if (drmModeAddFB2(d.fd, w, h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &b->fb_id, 0) < 0)
         return -1;
      if (drmModeAddFB2(d.fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &b->fb_id_x, 0) < 0)
         fprintf(stderr, "gfx: XRGB8888 framebuffer not supported\n");
   }
   return 0;
}

static void dumb_destroy(dumb_t *b)
{
   struct drm_mode_destroy_dumb dreq = { 0 };
   if (b->fb_id)
      drmModeRmFB(d.fd, b->fb_id);
   if (b->fb_id_x)
      drmModeRmFB(d.fd, b->fb_id_x);
   if (b->map && b->map != MAP_FAILED)
      munmap(b->map, b->size);
   if (b->prime_fd > 0)
      close(b->prime_fd);
   dreq.handle = b->handle;
   if (b->handle)
      drmIoctl(d.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
   memset(b, 0, sizeof(*b));
}

int gfx_init(void)
{
   drmModeRes *res;
   drmModeConnector *conn = NULL;
   int i;

   d.fd = open(BOARD_DRM_DEVICE, O_RDWR | O_CLOEXEC);
   if (d.fd < 0)
      return -1;
   if (!(res = drmModeGetResources(d.fd)))
      return -1;

   for (i = 0; i < res->count_connectors && !conn; i++)
   {
      drmModeConnector *c = drmModeGetConnector(d.fd, res->connectors[i]);
      if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
         conn = c;
      else if (c)
         drmModeFreeConnector(c);
   }
   if (!conn)
      return -1;

   d.conn_id = conn->connector_id;
   d.mode    = conn->modes[0];

   /* The stock menu's screen saver may have left the panel off; switch it on. */
   for (i = 0; i < conn->count_props; i++)
   {
      drmModePropertyRes *prop = drmModeGetProperty(d.fd, conn->props[i]);
      if (prop && !strcmp(prop->name, "DPMS"))
      {
         d.dpms_prop = prop->prop_id;
         drmModeConnectorSetProperty(d.fd, d.conn_id, prop->prop_id, DRM_MODE_DPMS_ON);
      }
      if (prop)
         drmModeFreeProperty(prop);
   }
   {
      drmModeEncoder *enc = drmModeGetEncoder(d.fd, conn->encoder_id);
      d.crtc_id = enc ? enc->crtc_id : 0;
      if (!d.crtc_id)
      {
         for (i = 0; i < res->count_encoders && !d.crtc_id; i++)
         {
            drmModeEncoder *e = drmModeGetEncoder(d.fd, res->encoders[i]);
            int j;
            for (j = 0; e && j < res->count_crtcs; j++)
               if (e->possible_crtcs & (1u << j))
               {
                  d.crtc_id = res->crtcs[j];
                  break;
               }
            if (e)
               drmModeFreeEncoder(e);
         }
      }
      if (enc)
         drmModeFreeEncoder(enc);
   }
   drmModeFreeConnector(conn);
   drmModeFreeResources(res);
   if (!d.crtc_id)
      return -1;

   /* The panel is portrait (hdisplay x vdisplay); the UI is landscape. */
   if (dumb_create(&d.pages[0], d.mode.hdisplay, d.mode.vdisplay, 1) < 0 ||
       dumb_create(&d.pages[1], d.mode.hdisplay, d.mode.vdisplay, 1) < 0 ||
       dumb_create(&d.canvas_gpu, d.mode.vdisplay, d.mode.hdisplay, 0) < 0)
      return -1;

   d.canvas.w      = d.mode.vdisplay;
   d.canvas.h      = d.mode.hdisplay;
   d.canvas.stride = d.canvas_gpu.pitch / 4;
   d.canvas.px     = calloc((size_t)d.canvas.stride * d.canvas.h, 4);
   return d.canvas.px ? 0 : -1;
}

canvas_t *gfx_canvas(void)
{
   return &d.canvas;
}

static void present_gpu(void);
static void present_page(dumb_t *page);

void gfx_present(void)
{
   dumb_t *page = &d.pages[d.cur];

   if (d.dbg_cpu)
   {
      /* CPU rotation (90 deg counter-clockwise), for diagnosing the RGA path */
      int x, y, ps = page->pitch / 4;
      uint32_t *dst_px = (uint32_t *)page->map;
      for (y = 0; y < d.canvas.h; y++)
      {
         const uint32_t *row = d.canvas.px + (size_t)y * d.canvas.stride;
         for (x = 0; x < d.canvas.w; x++)
            dst_px[(size_t)(d.canvas.w - 1 - x) * ps + y] = row[x];
      }
      goto show;
   }

   memcpy(d.canvas_gpu.map, d.canvas.px, (size_t)d.canvas.stride * 4 * d.canvas.h);
   present_gpu();
   return;

show:
   present_page(page);
}

/* rotate the GPU canvas onto the next page and show it */
static void present_gpu(void)
{
   dumb_t *page = &d.pages[d.cur];
   rga_info_t src = { 0 }, dst = { 0 };

   flip_wait();

   src.fd       = d.canvas_gpu.prime_fd;
   src.mmuFlag  = 1;
   src.rotation = HAL_TRANSFORM_ROT_270;
   rga_set_rect(&src.rect, 0, 0, d.canvas.w, d.canvas.h,
         d.canvas_gpu.pitch / 4, d.canvas.h, RK_FORMAT_BGRA_8888);

   dst.fd      = page->prime_fd;
   dst.mmuFlag = 1;
   rga_set_rect(&dst.rect, 0, 0, page->w, page->h, page->pitch / 4, page->h, RK_FORMAT_BGRA_8888);

   {
      int r = c_RkRgaBlit(&src, &dst, NULL);
      if (r != 0)
         fprintf(stderr, "gfx: RGA blit failed (%d)\n", r);
   }

   present_page(page);
}

static void present_page(dumb_t *page)
{
   {
      uint32_t fb = d.dbg_xrgb && page->fb_id_x ? page->fb_id_x : page->fb_id;
      /* queue the page for the next refresh and carry on; fall back to a mode set */
      if (d.crtc_set && drmModePageFlip(d.fd, d.crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL) == 0)
         d.flip_pending = 1;
      else if (drmModeSetCrtc(d.fd, d.crtc_id, fb, 0, 0, &d.conn_id, 1, &d.mode) != 0)
         fprintf(stderr, "gfx: set crtc failed: %s\n", strerror(errno));
      else
         d.crtc_set = 1;
   }

   d.cur ^= 1;
}

void gfx_screen_power(int on)
{
   if (!on && d.canvas.px && d.crtc_set)
   {
      /* a few black frames first, so the panel doesn't keep (and the glass doesn't seem to hold) the last picture while it powers down */
      int i;
      memset(d.canvas.px, 0, (size_t)d.canvas.stride * 4 * d.canvas.h);
      for (i = 0; i < 3; i++)
         gfx_present();
      flip_wait();
   }
   if (d.dpms_prop)
      drmModeConnectorSetProperty(d.fd, d.conn_id, d.dpms_prop, on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF);
}

void gfx_debug_mode(int cpu_rotate, int xrgb)
{
   d.dbg_cpu  = cpu_rotate;
   d.dbg_xrgb = xrgb;
}

/* Prints a few pixels of the canvas, its GPU copy and the last shown page. */
void gfx_debug_sample(void)
{
   dumb_t *page = &d.pages[d.cur ^ 1];
   const uint32_t *pg = (const uint32_t *)page->map, *cg = (const uint32_t *)d.canvas_gpu.map;
   int ps = page->pitch / 4, cs = d.canvas_gpu.pitch / 4;
   fprintf(stderr, "sample: canvas[10,10]=%08x canvas[400,460]=%08x gpu[10,10]=%08x "
         "page[mid]=%08x page[10,10]=%08x page[240,10]=%08x page[240,800]=%08x nonzero=%d\n",
         d.canvas.px[10 * d.canvas.stride + 10], d.canvas.px[460 * d.canvas.stride + 400],
         cg[10 * cs + 10], pg[(page->h / 2) * ps + page->w / 2], pg[10 * ps + 10],
         pg[10 * ps + 240], pg[800 * ps + 240],
         ({ int n = 0; for (int i = 0; i < ps * (int)page->h; i += 997) n += pg[i] != 0; n; }));
}

void gfx_shutdown(void)
{
   free(d.canvas.px);
   dumb_destroy(&d.canvas_gpu);
   dumb_destroy(&d.pages[0]);
   dumb_destroy(&d.pages[1]);
   if (d.fd > 0)
      close(d.fd);
   memset(&d, 0, sizeof(d));
}

/* ---------------------------------------------------------------- video */

#define RGA_MAX 1280   /* the RGA's largest picture, either way (rkrga/hardware) */

struct gfx_video
{
   dumb_t src;          /* the frame, YUV 4:2:0 in three planes, where the RGA reads it */
   int w, h, stride, rows;
   int cx, cy, cw, ch;  /* the part of the frame shown (cw 0 = all of it) */
};

void gfx_video_crop(gfx_video_t *v, int x, int y, int w, int h)
{
   if (!v)
      return;
   v->cx = x & ~1;
   v->cy = y & ~1;
   v->cw = w & ~1;
   v->ch = h & ~1;
}

gfx_video_t *gfx_video_new(int w, int h)
{
   gfx_video_t *v = calloc(1, sizeof(*v));
   int stride = (w + 63) & ~63, rows = (h + 1) & ~1;
   if (!v)
      return NULL;
   /* one buffer: the Y rows, then U and V at half size (stride / 2, rows / 2) */
   if (dumb_create(&v->src, stride / 4, rows * 3 / 2, 0) < 0 || v->src.pitch != (uint32_t)stride)
   {
      gfx_video_free(v);
      return NULL;
   }
   v->w = w;
   v->h = h;
   v->stride = stride;
   v->rows = rows;
   return v;
}

void gfx_video_free(gfx_video_t *v)
{
   if (!v)
      return;
   if (v->src.handle)
      dumb_destroy(&v->src);
   free(v);
}

void gfx_video_planes(gfx_video_t *v, uint8_t *planes[3], int strides[3])
{
   planes[0] = v->src.map;
   planes[1] = planes[0] + (size_t)v->stride * v->rows;
   planes[2] = planes[1] + (size_t)(v->stride / 2) * (v->rows / 2);
   strides[0] = v->stride;
   strides[1] = strides[2] = v->stride / 2;
}

/* The video frame, scaled into (x, y, w, h) of the screen; the rest is black when
 * clear is set. Returns the screen itself to draw controls on, for gfx_video_end(). */
canvas_t *gfx_video_begin(gfx_video_t *v, int x, int y, int w, int h, int clear)
{
   static canvas_t scr;
   rga_info_t src = { 0 }, dst = { 0 };
   int r;

   scr.w = d.canvas.w;
   scr.h = d.canvas.h;
   scr.stride = d.canvas_gpu.pitch / 4;
   scr.px = (uint32_t *)d.canvas_gpu.map;
   if (clear && !v)
      memset(d.canvas_gpu.map, 0, (size_t)d.canvas_gpu.pitch * d.canvas.h);
   else if (clear)
   {
      /* only around the picture: the picture itself is drawn over next */
      int j, x0 = x < 0 ? 0 : x, x1 = x + w > scr.w ? scr.w : x + w;
      for (j = 0; j < scr.h; j++)
      {
         uint32_t *row = scr.px + (size_t)j * scr.stride;
         if (j < y || j >= y + h || x0 >= x1)
            memset(row, 0, (size_t)scr.w * 4);
         else
         {
            if (x0 > 0)
               memset(row, 0, (size_t)x0 * 4);
            if (x1 < scr.w)
               memset(row + x1, 0, (size_t)(scr.w - x1) * 4);
         }
      }
   }
   if (!v)
      return &scr;

   {
      /* The RGA takes pictures up to RGA_MAX on a side: a bigger frame (1080p) goes
       * across in pieces, each scaled into its share of the screen. */
      int ox = v->cw ? v->cx : 0, oy = v->cw ? v->cy : 0;
      int sw = v->cw ? v->cw : v->w & ~1, sh = v->cw ? v->ch : v->h & ~1;
      int nx = (sw + RGA_MAX - 1) / RGA_MAX, ny = (sh + RGA_MAX - 1) / RGA_MAX, i, j;
      for (j = 0; j < ny; j++)
         for (i = 0; i < nx; i++)
         {
            /* even edges, so the half-size colour planes line up */
            int sx0 = sw * i / nx & ~1, sx1 = i == nx - 1 ? sw : sw * (i + 1) / nx & ~1;
            int sy0 = sh * j / ny & ~1, sy1 = j == ny - 1 ? sh : sh * (j + 1) / ny & ~1;
            int dx0 = x + (int)((int64_t)w * sx0 / sw), dx1 = x + (int)((int64_t)w * sx1 / sw);
            int dy0 = y + (int)((int64_t)h * sy0 / sh), dy1 = y + (int)((int64_t)h * sy1 / sh);
            memset(&src, 0, sizeof(src));
            memset(&dst, 0, sizeof(dst));
            src.fd = v->src.prime_fd;
            src.mmuFlag = 1;
            rga_set_rect(&src.rect, ox + sx0, oy + sy0, sx1 - sx0, sy1 - sy0, v->stride, v->rows, RK_FORMAT_YCbCr_420_P);
            dst.fd = d.canvas_gpu.prime_fd;
            dst.mmuFlag = 1;
            rga_set_rect(&dst.rect, dx0, dy0, dx1 - dx0, dy1 - dy0, d.canvas_gpu.pitch / 4, d.canvas.h,
                  RK_FORMAT_BGRA_8888);
            if ((r = c_RkRgaBlit(&src, &dst, NULL)) != 0)
            {
               static int told;
               if (!told++)
                  fprintf(stderr, "gfx: RGA video blit failed (%d), %dx%d piece\n", r, sx1 - sx0, sy1 - sy0);
            }
         }
   }
   return &scr;
}

void gfx_video_end(void)
{
   present_gpu();
}

/* ---------------------------------------------------------------- drawing */

static inline uint32_t blend(uint32_t dst, uint32_t src, unsigned a)
{
   unsigned ia = 255 - a;
   unsigned r = ((src >> 16) & 0xff) * a + ((dst >> 16) & 0xff) * ia + 128;
   unsigned g = ((src >> 8) & 0xff) * a + ((dst >> 8) & 0xff) * ia + 128;
   unsigned b = (src & 0xff) * a + (dst & 0xff) * ia + 128;
   r = (r + (r >> 8)) >> 8;                         /* x / 255, rounded, without a divide */
   g = (g + (g >> 8)) >> 8;
   b = (b + (b >> 8)) >> 8;
   return 0xff000000u | (r << 16) | (g << 8) | b;
}

void gfx_clear(canvas_t *c, uint32_t color)
{
   gfx_fill_rect(c, 0, 0, c->w, c->h, color | 0xff000000u);
}

void gfx_fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t color)
{
   unsigned a = color >> 24;
   int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
   int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
   int i, j;

   for (j = y0; j < y1; j++)
   {
      uint32_t *row = c->px + (size_t)j * c->stride;
      if (a == 255)
         for (i = x0; i < x1; i++)
            row[i] = color;
      else if (a)
         for (i = x0; i < x1; i++)
            row[i] = blend(row[i], color, a);
   }
}

void gfx_draw_image(canvas_t *c, const image_t *img, int x, int y)
{
   gfx_draw_image_scaled(c, img, x, y, img->w, img->h);
}

void gfx_draw_image_scaled(canvas_t *c, const image_t *img, int x, int y, int w, int h)
{
   int i, j;
   if (!img || w <= 0 || h <= 0)
      return;
   for (j = 0; j < h; j++)
   {
      int cy = y + j, sy;
      uint32_t *row;
      if (cy < 0 || cy >= c->h)
         continue;
      sy  = (int)((long long)j * img->h / h);
      row = c->px + (size_t)cy * c->stride;
      for (i = 0; i < w; i++)
      {
         int cx = x + i;
         uint32_t s;
         unsigned a;
         if (cx < 0 || cx >= c->w)
            continue;
         s = img->px[(size_t)sy * img->w + (size_t)i * img->w / w];
         a = s >> 24;
         if (a == 255)
            row[cx] = s;
         else if (a)
            row[cx] = blend(row[cx], s, a);
      }
   }
}

void gfx_draw_image_smooth(canvas_t *c, const image_t *img, int x, int y, int w, int h)
{
   int i, j, x0, x1, y0, y1;
   uint32_t fx, fy;   /* 16.16 source step */

   if (w <= 0 || h <= 0)
      return;
   x0 = x < 0 ? 0 : x;
   y0 = y < 0 ? 0 : y;
   x1 = x + w > c->w ? c->w : x + w;
   y1 = y + h > c->h ? c->h : y + h;
   if (x0 >= x1 || y0 >= y1)
      return;

   /* same size: straight copy */
   if (w == img->w && h == img->h)
   {
      for (j = y0; j < y1; j++)
         memcpy(c->px + (size_t)j * c->stride + x0, img->px + (size_t)(j - y) * img->w + (x0 - x),
               (size_t)(x1 - x0) * 4);
      return;
   }

   fx = ((uint32_t)(img->w - 1) << 16) / (w > 1 ? w - 1 : 1);
   fy = ((uint32_t)(img->h - 1) << 16) / (h > 1 ? h - 1 : 1);
   for (j = y0; j < y1; j++)
   {
      uint32_t sy = (uint32_t)(j - y) * fy, ty = sy >> 16, wy = (sy >> 8) & 0xff;
      const uint32_t *r0 = img->px + (size_t)ty * img->w;
      const uint32_t *r1 = ty + 1 < (uint32_t)img->h ? r0 + img->w : r0;
      uint32_t *out = c->px + (size_t)j * c->stride;
      for (i = x0; i < x1; i++)
      {
         uint32_t sx = (uint32_t)(i - x) * fx, tx = sx >> 16, wx = (sx >> 8) & 0xff;
         uint32_t tx1 = tx + 1 < (uint32_t)img->w ? tx + 1 : tx;
         uint32_t a = r0[tx], b = r0[tx1], cc = r1[tx], d = r1[tx1];
         /* blend red+blue and green in two lanes */
         uint32_t rb0 = ((a & 0xff00ff) * (256 - wx) + (b & 0xff00ff) * wx) >> 8 & 0xff00ff;
         uint32_t g0  = ((a & 0x00ff00) * (256 - wx) + (b & 0x00ff00) * wx) >> 8 & 0x00ff00;
         uint32_t rb1 = ((cc & 0xff00ff) * (256 - wx) + (d & 0xff00ff) * wx) >> 8 & 0xff00ff;
         uint32_t g1  = ((cc & 0x00ff00) * (256 - wx) + (d & 0x00ff00) * wx) >> 8 & 0x00ff00;
         uint32_t rb  = (rb0 * (256 - wy) + rb1 * wy) >> 8 & 0xff00ff;
         uint32_t g   = (g0 * (256 - wy) + g1 * wy) >> 8 & 0x00ff00;
         out[i] = 0xff000000 | rb | g;
      }
   }
}

void gfx_draw_image_fast(canvas_t *c, const image_t *img, int x, int y, int w, int h)
{
   int i, j, x0, x1, y0, y1;
   uint32_t fx, fy;

   if (w <= 0 || h <= 0)
      return;
   x0 = x < 0 ? 0 : x;
   y0 = y < 0 ? 0 : y;
   x1 = x + w > c->w ? c->w : x + w;
   y1 = y + h > c->h ? c->h : y + h;
   if (x0 >= x1 || y0 >= y1)
      return;
   fx = ((uint32_t)img->w << 16) / w;
   fy = ((uint32_t)img->h << 16) / h;
   for (j = y0; j < y1; j++)
   {
      const uint32_t *src = img->px + (size_t)(((uint32_t)(j - y) * fy) >> 16) * img->w;
      uint32_t *out = c->px + (size_t)j * c->stride;
      uint32_t sx = (uint32_t)(x0 - x) * fx;
      for (i = x0; i < x1; i++, sx += fx)
         out[i] = src[sx >> 16];
   }
}

image_t *image_scaled(const image_t *img, int w, int h)
{
   image_t *out = malloc(sizeof(image_t));
   canvas_t cv;
   if (!out)
      return NULL;
   out->w = w;
   out->h = h;
   if (!(out->px = malloc((size_t)w * h * 4)))
   {
      free(out);
      return NULL;
   }
   cv.w = cv.stride = w;
   cv.h = h;
   cv.px = out->px;
   gfx_draw_image_smooth(&cv, img, 0, 0, w, h);
   return out;
}

image_t *image_half(const image_t *img)
{
   image_t *out = malloc(sizeof(image_t));
   int x, y;
   if (!out)
      return NULL;
   out->w = img->w / 2;
   out->h = img->h / 2;
   if (!(out->px = malloc((size_t)out->w * out->h * 4)))
   {
      free(out);
      return NULL;
   }
   for (y = 0; y < out->h; y++)
      for (x = 0; x < out->w; x++)
      {
         const uint32_t *p = img->px + (size_t)(y * 2) * img->w + x * 2;
         uint32_t a = p[0], b = p[1], cc = p[img->w], dd = p[img->w + 1];
         uint32_t rb = ((a & 0xff00ff) + (b & 0xff00ff) + (cc & 0xff00ff) + (dd & 0xff00ff)) >> 2 & 0xff00ff;
         uint32_t g  = ((a & 0x00ff00) + (b & 0x00ff00) + (cc & 0x00ff00) + (dd & 0x00ff00)) >> 2 & 0x00ff00;
         out->px[(size_t)y * out->w + x] = 0xff000000 | rb | g;
      }
   return out;
}

/* ---------------------------------------------------------------- images */

static image_t *image_from_rgba(unsigned char *rgba, int w, int h);

/* A picture bigger than this isn't loaded (a stray photo or a corrupt file's huge size would
 * otherwise take all the handheld's memory and crash whatever was showing it). 12 million pixels
 * is 48 MB once decoded: a 3000 x 3000 album cover still fits. */
#define MAX_IMAGE_PIXELS 12000000L

static int image_size_ok(int w, int h)
{
   return w > 0 && h > 0 && (long long)w * h <= MAX_IMAGE_PIXELS;
}

image_t *image_load(const char *path)
{
   int w, h, n;
   unsigned char *rgba;
   if (!stbi_info(path, &w, &h, &n) || !image_size_ok(w, h))
      return NULL;
   rgba = stbi_load(path, &w, &h, &n, 4);
   return rgba ? image_from_rgba(rgba, w, h) : NULL;
}

image_t *image_load_mem(const void *data, int len)
{
   int w, h, n;
   unsigned char *rgba;
   if (!stbi_info_from_memory(data, len, &w, &h, &n) || !image_size_ok(w, h))
      return NULL;
   rgba = stbi_load_from_memory(data, len, &w, &h, &n, 4);
   return rgba ? image_from_rgba(rgba, w, h) : NULL;
}

/* takes the decoder's buffer over as the picture's pixels (turned into ARGB in place, so a big
 * picture never needs a second copy); NULL if there's no room even for the small header */
static image_t *image_from_rgba(unsigned char *rgba, int w, int h)
{
   long i;
   uint32_t *px = (uint32_t *)rgba;
   image_t *img = malloc(sizeof(*img));

   if (!img)
   {
      stbi_image_free(rgba);
      return NULL;
   }
   for (i = 0; i < (long)w * h; i++)
   {
      const unsigned char *p = rgba + i * 4;
      px[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
   }
   img->w = w;
   img->h = h;
   img->px = px;   /* (stb's buffer comes from malloc, so image_free's free() is right) */
   return img;
}

void image_free(image_t *img)
{
   if (img)
   {
      free(img->px);
      free(img);
   }
}

/* ---------------------------------------------------------------- text */

#define GLYPH_CACHE 512

typedef struct
{
   uint32_t cp;
   int used, w, h, left, top, advance;
   uint8_t *bitmap;
} glyph_t;

struct font
{
   FT_Face face;
   int ascender, height;
   int bold;            /* extra stroke width, 1/64 px (0 = the font as it is) */
   glyph_t cache[GLYPH_CACHE];
};

static FT_Library ft;

font_t *font_load(const char *path, int px_size)
{
   font_t *f;
   if (!ft && FT_Init_FreeType(&ft))
      return NULL;
   if (!(f = calloc(1, sizeof(*f))))
      return NULL;
   if (FT_New_Face(ft, path, 0, &f->face) || FT_Set_Pixel_Sizes(f->face, 0, px_size))
   {
      free(f);
      return NULL;
   }
   f->ascender = f->face->size->metrics.ascender >> 6;
   f->height   = (f->face->size->metrics.ascender - f->face->size->metrics.descender) >> 6;
   return f;
}

font_t *font_load_bold(const char *path, int px_size)
{
   font_t *f = font_load(path, px_size);
   if (f)
      f->bold = px_size * 64 / 28;   /* about two thirds of a pixel at 18 px */
   return f;
}

void font_free(font_t *f)
{
   int i;
   if (!f)
      return;
   for (i = 0; i < GLYPH_CACHE; i++)
      free(f->cache[i].bitmap);
   FT_Done_Face(f->face);
   free(f);
}

int font_height(const font_t *f)
{
   return f->height;
}

static const glyph_t *get_glyph(font_t *f, uint32_t cp)
{
   glyph_t *g = &f->cache[cp % GLYPH_CACHE];
   FT_GlyphSlot slot;

   if (g->used && g->cp == cp)
      return g;
   if (FT_Load_Char(f->face, cp, f->bold ? FT_LOAD_NO_BITMAP : FT_LOAD_RENDER))
      return NULL;
   slot = f->face->glyph;
   if (f->bold && slot->format == FT_GLYPH_FORMAT_OUTLINE)
   {
      /* thicken the outline, then draw it; the letter gets that much wider */
      FT_Outline_Embolden(&slot->outline, f->bold);
      slot->advance.x += f->bold;
      if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
         return NULL;
   }
   free(g->bitmap);
   g->bitmap  = NULL;
   g->used    = 0;
   g->cp      = cp;
   g->w       = slot->bitmap.width;
   g->h       = slot->bitmap.rows;
   g->left    = slot->bitmap_left;
   g->top     = slot->bitmap_top;
   g->advance = slot->advance.x >> 6;
   if (!(g->bitmap = malloc((size_t)g->w * g->h + 1)))
      return NULL;   /* (out of memory: this letter is left out) */
   {
      int r;
      for (r = 0; r < g->h; r++)
         memcpy(g->bitmap + r * g->w, slot->bitmap.buffer + r * slot->bitmap.pitch, g->w);
   }
   g->used = 1;
   return g;
}

/* Decode one UTF-8 code point; advances *s. */
static uint32_t utf8_next(const char **s)
{
   const unsigned char *p = (const unsigned char *)*s;
   uint32_t cp;
   int n;

   if (p[0] < 0x80)      { cp = p[0];        n = 1; }
   else if (p[0] < 0xe0) { cp = p[0] & 0x1f; n = 2; }
   else if (p[0] < 0xf0) { cp = p[0] & 0x0f; n = 3; }
   else                  { cp = p[0] & 0x07; n = 4; }
   for (int i = 1; i < n; i++)
   {
      if ((p[i] & 0xc0) != 0x80)
      {
         *s += i;
         return 0xfffd;
      }
      cp = (cp << 6) | (p[i] & 0x3f);
   }
   *s += n;
   return cp;
}

/* the box the letters' ink covers, relative to drawing at (0, 0) with gfx_draw_text */
void font_ink_box(font_t *f, const char *utf8, int *x0, int *y0, int *x1, int *y1)
{
   int pen = 0, first = 1;
   *x0 = *y0 = *x1 = *y1 = 0;
   while (*utf8)
   {
      const glyph_t *g = get_glyph(f, utf8_next(&utf8));
      if (!g)
         continue;
      if (g->w && g->h)
      {
         int gx0 = pen + g->left, gx1 = gx0 + g->w, gy0 = f->ascender - g->top, gy1 = gy0 + g->h;
         if (first || gx0 < *x0) *x0 = gx0;
         if (first || gx1 > *x1) *x1 = gx1;
         if (first || gy0 < *y0) *y0 = gy0;
         if (first || gy1 > *y1) *y1 = gy1;
         first = 0;
      }
      pen += g->advance;
   }
}

int font_text_width(font_t *f, const char *utf8)
{
   int w = 0;
   while (*utf8)
   {
      const glyph_t *g = get_glyph(f, utf8_next(&utf8));
      if (g)
         w += g->advance;
   }
   return w;
}

static int draw_run(canvas_t *c, font_t *f, const char *s, const char *end, int x, int y, uint32_t color)
{
   int pen = x, baseline = y + f->ascender;

   while (*s && (!end || s < end))
   {
      const glyph_t *g = get_glyph(f, utf8_next(&s));
      int i, j;
      if (!g)
         continue;
      for (j = 0; j < g->h; j++)
      {
         int cy = baseline - g->top + j;
         uint32_t *row;
         if (cy < 0 || cy >= c->h)
            continue;
         row = c->px + (size_t)cy * c->stride;
         for (i = 0; i < g->w; i++)
         {
            int cx = pen + g->left + i;
            unsigned cov = g->bitmap[j * g->w + i];
            if (cov && cx >= 0 && cx < c->w)
            {
               unsigned al = (color >> 24) == 255 ? cov : (cov * (color >> 24) + 127) / 255;
               if (al == 255)
                  row[cx] = 0xff000000u | (color & 0xffffff);      /* full ink: no blend */
               else
                  row[cx] = blend(row[cx], color, al);
            }
         }
      }
      pen += g->advance;
   }
   return pen - x;
}

int gfx_draw_text(canvas_t *c, font_t *f, const char *utf8, int x, int y, uint32_t color, int max_w)
{
   if (max_w > 0 && font_text_width(f, utf8) > max_w)
   {
      int ell = font_text_width(f, "...");
      const char *s = utf8, *cut = utf8;
      int w = 0;
      while (*s)
      {
         const char *prev = s;
         const glyph_t *g = get_glyph(f, utf8_next(&s));
         if (g && w + g->advance + ell > max_w)
         {
            cut = prev;
            break;
         }
         if (g)
            w += g->advance;
         cut = s;
      }
      w = draw_run(c, f, utf8, cut, x, y, color);
      return w + draw_run(c, f, "...", NULL, x + w, y, color);
   }
   return draw_run(c, f, utf8, NULL, x, y, color);
}
