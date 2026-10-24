#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wsm.h>

int main() {
  const uint8_t hello[] = "hello";
  struct wsm_encoder encoder = wsm_encoder_create();
  struct wsm_decoder *decoder = wsm_decoder_create();

  if (decoder == nullptr) {
    return 1;
  }
  struct wsm_encoding_result encoded = wsm_text(&encoder, hello, sizeof(hello) - 1, nullptr);
  if (encoded.status != WSM_OK) {
    wsm_decoder_destroy(decoder);
    return 1;
  }
  struct wsm_decoding_result result = wsm_decoder_feed(decoder, encoded.data, encoded.data_length);
  if (result.status != WSM_OK || result.messages_count != 1 || !result.messages[0].start ||
      !result.messages[0].end) {
    wsm_decoding_result_free(result);
    free(encoded.data);
    wsm_decoder_destroy(decoder);
    return 1;
  }
  printf("%.*s\n", (int)result.messages[0].payload_length,
         (const char *)result.messages[0].payload);
  wsm_decoding_result_free(result);
  free(encoded.data);
  wsm_decoder_destroy(decoder);
  return 0;
}
