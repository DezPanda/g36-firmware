// raw prompt in, text out: for checking how a model formats things
#include "llama.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
int main(int argc, char **argv)
{
   std::string prompt;
   char buf[4096];
   size_t n;
   while ((n = fread(buf, 1, sizeof(buf), stdin)) > 0)
      prompt.append(buf, n);
   llama_log_set([](enum ggml_log_level, const char *, void *) {}, nullptr);
   ggml_backend_load_all();
   llama_model *m = llama_model_load_from_file(argv[1], llama_model_default_params());
   const llama_vocab *v = llama_model_get_vocab(m);
   llama_context_params cp = llama_context_default_params();
   cp.n_ctx = 2048; cp.n_batch = 2048; cp.n_threads = cp.n_threads_batch = 3;
   llama_context *c = llama_init_from_model(m, cp);
   llama_sampler *s = llama_sampler_chain_init(llama_sampler_chain_default_params());
   llama_sampler_chain_add(s, llama_sampler_init_min_p(0.05f, 1));
   llama_sampler_chain_add(s, llama_sampler_init_temp(0.6f));
   llama_sampler_chain_add(s, llama_sampler_init_dist(1234));
   int nt = -llama_tokenize(v, prompt.c_str(), prompt.size(), NULL, 0, true, true);
   std::vector<llama_token> t(nt);
   llama_tokenize(v, prompt.c_str(), prompt.size(), t.data(), nt, true, true);
   llama_batch b = llama_batch_get_one(t.data(), nt);
   llama_token tok;
   for (int i = 0; i < atoi(argv[2]); i++)
   {
      if (llama_decode(c, b)) break;
      tok = llama_sampler_sample(s, c, -1);
      if (llama_vocab_is_eog(v, tok)) break;
      int len = llama_token_to_piece(v, tok, buf, sizeof(buf), 0, true);
      fwrite(buf, 1, len, stdout); fflush(stdout);
      b = llama_batch_get_one(&tok, 1);
   }
   printf("\n[end]\n");
}
