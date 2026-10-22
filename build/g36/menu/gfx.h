/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* Drawing core for the G36 menu: a landscape canvas drawn by the CPU and
 * rotated onto the portrait panel by the RGA chip. Pixels are 0xAARRGGBB. */
#ifndef G36_GFX_H
#define G36_GFX_H

#include <stdint.h>

typedef struct
{
   int w, h;            /* pixels */
   int stride;          /* pixels per row */
   uint32_t *px;
} canvas_t;

typedef struct
{
   int w, h;
   uint32_t *px;        /* 0xAARRGGBB, not premultiplied */
} image_t;

typedef struct font font_t;

/* Display */
int       gfx_init(void);
canvas_t *gfx_canvas(void);
void      gfx_present(void);
void      gfx_shutdown(void);
void      gfx_screen_power(int on);
void      gfx_debug_mode(int cpu_rotate, int xrgb);
void      gfx_debug_sample(void);

/* Video: a frame (YUV 4:2:0, three planes) is written into the buffer from
 * gfx_video_planes(), then gfx_video_begin() scales it onto the screen and returns the
 * screen to draw on top of, and gfx_video_end() shows it (instead of gfx_present). */
typedef struct gfx_video gfx_video_t;
gfx_video_t *gfx_video_new(int w, int h);
void      gfx_video_free(gfx_video_t *v);
void      gfx_video_planes(gfx_video_t *v, uint8_t *planes[3], int strides[3]);
/* show only part of the frame (zoomed in); w 0 shows all of it */
void      gfx_video_crop(gfx_video_t *v, int x, int y, int w, int h);
canvas_t *gfx_video_begin(gfx_video_t *v, int x, int y, int w, int h, int clear);
void      gfx_video_end(void);

/* Drawing on a canvas */
void gfx_clear(canvas_t *c, uint32_t color);
void gfx_fill_rect(canvas_t *c, int x, int y, int w, int h, uint32_t color);
void gfx_draw_image(canvas_t *c, const image_t *img, int x, int y);
void gfx_draw_image_scaled(canvas_t *c, const image_t *img, int x, int y, int w, int h);
/* Opaque image scaled smoothly (bilinear); alpha is ignored. */
void gfx_draw_image_smooth(canvas_t *c, const image_t *img, int x, int y, int w, int h);
/* Opaque image scaled quickly (nearest pixel), for things in motion; alpha is ignored. */
void gfx_draw_image_fast(canvas_t *c, const image_t *img, int x, int y, int w, int h);
/* New images: a smooth scaled copy, and a half-size copy (2x2 average). */
image_t *image_scaled(const image_t *img, int w, int h);
image_t *image_half(const image_t *img);

/* Images (PNG/JPG via stb_image) */
image_t *image_load(const char *path);
image_t *image_load_mem(const void *data, int len);
void     image_free(image_t *img);

/* Text (FreeType) */
font_t *font_load(const char *path, int px_size);
font_t *font_load_bold(const char *path, int px_size);   /* the same font, thickened */
void    font_free(font_t *f);
int     font_height(const font_t *f);
int     font_text_width(font_t *f, const char *utf8);
/* the box the ink covers when drawn at (0, 0), for centring letters exactly */
void    font_ink_box(font_t *f, const char *utf8, int *x0, int *y0, int *x1, int *y1);
/* Draws text with its top-left at (x, y); returns the advance width.
 * max_w > 0 truncates with "..." when the text would not fit. */
int     gfx_draw_text(canvas_t *c, font_t *f, const char *utf8, int x, int y, uint32_t color, int max_w);

#endif
