/* The G36 image generator engine: Supra2-IMG (a 104M diffusion transformer) with the
 * Flan-T5-Base text encoder and the TAESD picture decoder, on ggml. Makes 256x256
 * pictures from a text prompt. Only one part is in memory at a time: the text encoder
 * reads the prompt and is let go, then the DiT runs its steps, then the decoder. */
#ifndef G36_IG_H
#define G36_IG_H

#include <stdint.h>

#define IG_SIZE 256          /* the picture, square */
#define IG_LAT  32           /* the latent, square (4 channels) */

typedef struct ig ig_t;

/* dir holds t5.bin, dit.bin and taesd.bin (or their .f32 versions). */
ig_t *ig_new(const char *dir, int threads);
void  ig_free(ig_t *ig);
const char *ig_error(ig_t *ig);

/* Reads the prompt (loading and then letting go of the text encoder). 0 or -1. */
int   ig_encode(ig_t *ig, const char *prompt);

/* Starts a picture: steps (1..200), guidance (1 = none, 3 is the default), seed. */
int   ig_begin(ig_t *ig, int steps, float guidance, uint32_t seed);
/* One step; returns the steps done, or -1. */
int   ig_step(ig_t *ig);
int   ig_steps_total(ig_t *ig);
/* A rough picture of where it's heading (cheap, from the latent): IG_LAT*8 square RGB. */
void  ig_preview(ig_t *ig, uint8_t *rgb);
/* The finished picture through the decoder: IG_SIZE square RGB (lets the DiT go). */
int   ig_decode(ig_t *ig, uint8_t *rgb);

/* Packs an f32 weight file to 8-bit (the big matrices). 0 or -1. */
int   ig_pack(const char *in, const char *out);

#endif
