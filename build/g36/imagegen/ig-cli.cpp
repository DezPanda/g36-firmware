/* Command-line test for the image generator engine:
 *   ig-cli pack IN.f32 OUT.bin
 *   ig-cli DIR "prompt" [steps] [guidance] [seed] [out.ppm]   (also writes out-preview.ppm) */
#include "ig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>

static double now_s()
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void ppm(const char *path, const uint8_t *rgb, int s)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return;
   fprintf(f, "P6\n%d %d\n255\n", s, s);
   fwrite(rgb, 1, (size_t)s * s * 3, f);
   fclose(f);
}

int main(int argc, char **argv)
{
   if (argc >= 4 && !strcmp(argv[1], "pack"))
      return ig_pack(argv[2], argv[3]) ? 1 : 0;
   if (argc < 3)
   {
      fprintf(stderr, "usage: ig-cli DIR prompt [steps] [guidance] [seed] [out.ppm]\n");
      return 1;
   }
   int steps = argc > 3 ? atoi(argv[3]) : 50;
   float g = argc > 4 ? (float)atof(argv[4]) : 3.0f;
   unsigned seed = argc > 5 ? (unsigned)atoi(argv[5]) : 0;
   std::string out = argc > 6 ? argv[6] : "out.ppm";
   int threads = getenv("IG_THREADS") ? atoi(getenv("IG_THREADS")) : 3;
   ig_t *ig = ig_new(argv[1], threads);
   double t0 = now_s();
   if (ig_encode(ig, argv[2]) < 0)
   {
      fprintf(stderr, "error: %s\n", ig_error(ig));
      return 1;
   }
   fprintf(stderr, "text: %.1f s\n", now_s() - t0);
   t0 = now_s();
   if (ig_begin(ig, steps, g, seed) < 0)
   {
      fprintf(stderr, "error: %s\n", ig_error(ig));
      return 1;
   }
   fprintf(stderr, "model loaded: %.1f s\n", now_s() - t0);
   double ts = now_s();
   static uint8_t prev[256 * 256 * 3], pic[256 * 256 * 3];
   for (int i = 0; i < steps; i++)
   {
      double s0 = now_s();
      if (ig_step(ig) < 0)
      {
         fprintf(stderr, "error: step failed\n");
         return 1;
      }
      fprintf(stderr, "step %d/%d: %.2f s\n", i + 1, steps, now_s() - s0);
   }
   fprintf(stderr, "steps: %.1f s\n", now_s() - ts);
   ig_preview(ig, prev);
   ppm((out.substr(0, out.rfind('.')) + "-preview.ppm").c_str(), prev, 256);
   t0 = now_s();
   if (ig_decode(ig, pic) < 0)
   {
      fprintf(stderr, "error: %s\n", ig_error(ig));
      return 1;
   }
   fprintf(stderr, "decode: %.1f s\n", now_s() - t0);
   ppm(out.c_str(), pic, 256);
   ig_free(ig);
   return 0;
}
