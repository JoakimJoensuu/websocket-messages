#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wsf.h>

int main() {
  const uint8_t hello[] = "hello";
  struct wsf_encoder encoder = wsf_encoder_create();
  struct wsf_decoder *decoder = wsf_decoder_create();

  if (decoder == nullptr) {
    return 1;
  }
  struct wsf_encoding_result encoded = wsf_text(&encoder, hello, sizeof(hello) - 1, nullptr);
  if (encoded.status != WSF_OK) {
    wsf_decoder_destroy(decoder);
    return 1;
  }
  struct wsf_decoding_result result = wsf_decoder_feed(decoder, encoded.data, encoded.data_length);
  if (result.status != WSF_OK || result.events_count != 1 || !result.events[0].start ||
      !result.events[0].end) {
    wsf_decoding_result_free(result);
    free(encoded.data);
    wsf_decoder_destroy(decoder);
    return 1;
  }
  printf("%.*s\n", (int)result.events[0].payload_length, (const char *)result.events[0].payload);
  wsf_decoding_result_free(result);
  free(encoded.data);
  wsf_decoder_destroy(decoder);
  return 0;
}
