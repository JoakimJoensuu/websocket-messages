#include <cgreen/assertions.h>
#include <cgreen/constraint_syntax_helpers.h>
#include <cgreen/reporter.h>
#include <cgreen/runner.h>
#include <cgreen/suite.h>
#include <cgreen/text_reporter.h>
#include <cgreen/unit.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wsm.h>

#include "wsm_test.h"

static uint8_t *require_encoded(struct wsm_encoding_result encoded, size_t *wire_length) {
  assert_that(encoded.status, is_equal_to(WSM_OK));
  assert_that(encoded.data, is_non_null);
  *wire_length = encoded.data_length;
  return encoded.data;
}

static uint8_t *concat(const uint8_t *left, size_t left_length, const uint8_t *right,
                       size_t right_length, size_t *out_length) {
  uint8_t *both = malloc(left_length + right_length);
  assert_that(both, is_non_null);
  memcpy(both, left, left_length);
  memcpy(both + left_length, right, right_length);
  *out_length = left_length + right_length;
  return both;
}

Ensure(one_frame_message) {
  const uint8_t hello[] = {'H', 'e', 'l', 'l', 'o'};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t wire_length = 0;
  uint8_t *wire = require_encoded(wsm_text(&encoder, hello, sizeof(hello), nullptr), &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  wsm_test_assert_frame_header(wire, true, WSM_TEXT);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.source_consumed, is_equal_to(wire_length));
  assert_that(result.messages_count, is_equal_to(1));
  wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, true, hello, sizeof(hello));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
}

Ensure(one_frame_binary) {
  const uint8_t body[] = {0x00, 0xff};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t wire_length = 0;
  uint8_t *wire = require_encoded(wsm_binary(&encoder, body, sizeof(body), nullptr), &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  wsm_test_assert_frame_header(wire, true, WSM_BINARY);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(1));
  wsm_test_assert_message(&result.messages[0], WSM_BINARY, true, true, body, sizeof(body));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
}

Ensure(two_fragments) {
  const uint8_t first[] = {'H', 'e'};
  const uint8_t second[] = {'l', 'l', 'o'};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t start_length = 0;
  size_t end_length = 0;
  uint8_t *start =
      require_encoded(wsm_start_text(&encoder, first, sizeof(first), nullptr), &start_length);
  uint8_t *end =
      require_encoded(wsm_end_text(&encoder, second, sizeof(second), nullptr), &end_length);
  size_t wire_length = 0;
  uint8_t *wire = concat(start, start_length, end, end_length, &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  wsm_test_assert_frame_header(start, false, WSM_TEXT);
  wsm_test_assert_frame_header(end, true, WSM_TEST_CONTINUATION);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(2));
  wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, false, first, sizeof(first));
  wsm_test_assert_message(&result.messages[1], WSM_TEXT, false, true, second, sizeof(second));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
  free(start);
  free(end);
}

Ensure(three_fragments) {
  const uint8_t first[] = {'a'};
  const uint8_t middle[] = {'b', 'b'};
  const uint8_t last[] = {'c'};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t start_length = 0;
  size_t middle_length = 0;
  size_t end_length = 0;
  uint8_t *start =
      require_encoded(wsm_start_binary(&encoder, first, sizeof(first), nullptr), &start_length);
  uint8_t *cont = require_encoded(wsm_continue_binary(&encoder, middle, sizeof(middle), nullptr),
                                  &middle_length);
  uint8_t *end =
      require_encoded(wsm_end_binary(&encoder, last, sizeof(last), nullptr), &end_length);
  size_t head_length = 0;
  uint8_t *head = concat(start, start_length, cont, middle_length, &head_length);
  size_t wire_length = 0;
  uint8_t *wire = concat(head, head_length, end, end_length, &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  wsm_test_assert_frame_header(cont, false, WSM_TEST_CONTINUATION);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(3));
  wsm_test_assert_message(&result.messages[0], WSM_BINARY, true, false, first, sizeof(first));
  wsm_test_assert_message(&result.messages[1], WSM_BINARY, false, false, middle, sizeof(middle));
  wsm_test_assert_message(&result.messages[2], WSM_BINARY, false, true, last, sizeof(last));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
  free(head);
  free(start);
  free(cont);
  free(end);
}

Ensure(control_between_fragments) {
  const uint8_t first[] = {'H'};
  const uint8_t ping_body[] = {'?'};
  const uint8_t last[] = {'i'};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t start_length = 0;
  size_t ping_length = 0;
  size_t end_length = 0;
  uint8_t *start =
      require_encoded(wsm_start_text(&encoder, first, sizeof(first), nullptr), &start_length);
  uint8_t *ping = require_encoded(wsm_ping(ping_body, sizeof(ping_body), nullptr), &ping_length);
  uint8_t *end = require_encoded(wsm_end_text(&encoder, last, sizeof(last), nullptr), &end_length);
  size_t head_length = 0;
  uint8_t *head = concat(start, start_length, ping, ping_length, &head_length);
  size_t wire_length = 0;
  uint8_t *wire = concat(head, head_length, end, end_length, &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  wsm_test_assert_frame_header(ping, true, WSM_PING);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(3));
  wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, false, first, sizeof(first));
  wsm_test_assert_message(&result.messages[1], WSM_PING, true, true, ping_body, sizeof(ping_body));
  wsm_test_assert_message(&result.messages[2], WSM_TEXT, false, true, last, sizeof(last));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
  free(head);
  free(start);
  free(ping);
  free(end);
}

Ensure(orphan_continuation) {
  const uint8_t first[] = {'a'};
  const uint8_t rest[] = {'b'};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t start_length = 0;
  size_t cont_length = 0;
  uint8_t *start =
      require_encoded(wsm_start_text(&encoder, first, sizeof(first), nullptr), &start_length);
  uint8_t *cont =
      require_encoded(wsm_continue_text(&encoder, rest, sizeof(rest), nullptr), &cont_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  result = wsm_decoder_feed(decoder, cont, cont_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION));
  assert_that(result.messages_count, is_equal_to(0));
  wsm_decoding_result_free(result);
  result = wsm_decoder_feed(decoder, cont, cont_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION));
  assert_that(result.source_consumed, is_equal_to(0));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(start);
  free(cont);
}

Ensure(new_data_while_fragmented) {
  const uint8_t first[] = {'a'};
  const uint8_t next[] = {'b'};
  struct wsm_encoder open_encoder = wsm_encoder_create();
  struct wsm_encoder other_encoder = wsm_encoder_create();
  size_t start_length = 0;
  size_t next_length = 0;
  uint8_t *start =
      require_encoded(wsm_start_text(&open_encoder, first, sizeof(first), nullptr), &start_length);
  uint8_t *other =
      require_encoded(wsm_text(&other_encoder, next, sizeof(next), nullptr), &next_length);
  size_t wire_length = 0;
  uint8_t *wire = concat(start, start_length, other, next_length, &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED));
  assert_that(result.messages_count, is_equal_to(1));
  wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, false, first, sizeof(first));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
  free(start);
  free(other);
}

Ensure(binary_while_text_open) {
  const uint8_t text[] = {'t'};
  const uint8_t binary[] = {0x01};
  struct wsm_encoder text_encoder = wsm_encoder_create();
  struct wsm_encoder binary_encoder = wsm_encoder_create();
  size_t start_length = 0;
  size_t binary_length = 0;
  uint8_t *start =
      require_encoded(wsm_start_text(&text_encoder, text, sizeof(text), nullptr), &start_length);
  uint8_t *bin =
      require_encoded(wsm_binary(&binary_encoder, binary, sizeof(binary), nullptr), &binary_length);
  size_t wire_length = 0;
  uint8_t *wire = concat(start, start_length, bin, binary_length, &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED));
  assert_that(result.messages_count, is_equal_to(1));
  wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, false, text, sizeof(text));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
  free(start);
  free(bin);
}

Ensure(text_while_binary_open) {
  const uint8_t binary[] = {0x02};
  const uint8_t text[] = {'x'};
  struct wsm_encoder binary_encoder = wsm_encoder_create();
  struct wsm_encoder text_encoder = wsm_encoder_create();
  size_t start_length = 0;
  size_t text_length = 0;
  uint8_t *start = require_encoded(
      wsm_start_binary(&binary_encoder, binary, sizeof(binary), nullptr), &start_length);
  uint8_t *msg =
      require_encoded(wsm_text(&text_encoder, text, sizeof(text), nullptr), &text_length);
  size_t wire_length = 0;
  uint8_t *wire = concat(start, start_length, msg, text_length, &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED));
  assert_that(result.messages_count, is_equal_to(1));
  wsm_test_assert_message(&result.messages[0], WSM_BINARY, true, false, binary, sizeof(binary));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
  free(start);
  free(msg);
}

Ensure(split_across_chunks) {
  const uint8_t hello[] = {'h', 'i', '!'};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t wire_length = 0;
  uint8_t *wire = require_encoded(wsm_text(&encoder, hello, sizeof(hello), nullptr), &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  assert_that(1 < wire_length, is_true);
  result = wsm_decoder_feed(decoder, wire, 1);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.source_consumed, is_equal_to(1));
  assert_that(result.messages_count, is_equal_to(0));
  wsm_decoding_result_free(result);
  result = wsm_decoder_feed(decoder, nullptr, 0);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(0));
  wsm_decoding_result_free(result);
  result = wsm_decoder_feed(decoder, wire + 1, wire_length - 1);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(1));
  wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, true, hello, sizeof(hello));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
}

Ensure(byte_at_a_time) {
  const uint8_t hello[] = {'h', 'i'};
  const uint8_t key[] = {0x11, 0x22, 0x33, 0x44};
  struct wsm_encoder encoder = wsm_encoder_create();
  size_t wire_length = 0;
  uint8_t *wire = require_encoded(wsm_text(&encoder, hello, sizeof(hello), key), &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  for (size_t i = 0; i < wire_length; i++) {
    result = wsm_decoder_feed(decoder, wire + i, 1);
    assert_that(result.status, is_equal_to(WSM_OK));
    if (i + 1 < wire_length) {
      assert_that(result.messages_count, is_equal_to(0));
      wsm_decoding_result_free(result);
    } else {
      assert_that(result.messages_count, is_equal_to(1));
      wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, true, hello, sizeof(hello));
      assert_that(result.messages[0].masked, is_true);
      wsm_decoding_result_free(result);
    }
  }
  wsm_decoder_destroy(decoder);
  free(wire);
}

Ensure(rfc_unmasked_hello) {
  const uint8_t wire[] = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
  const uint8_t hello[] = {'H', 'e', 'l', 'l', 'o'};
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  result = wsm_decoder_feed(decoder, wire, sizeof(wire));
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(1));
  wsm_test_assert_message(&result.messages[0], WSM_TEXT, true, true, hello, sizeof(hello));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
}

Ensure(fragmented_control) {
  uint8_t wire[WSM_TEST_HEADER_BASE];
  size_t wire_length = wsm_test_craft(wire, sizeof(wire), false, WSM_PING, nullptr, 0);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_FRAGMENTED_CONTROL));
  assert_that(result.messages_count, is_equal_to(0));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
}

Ensure(pong_and_close) {
  const uint8_t pong_body[] = {0x01};
  const uint8_t close_body[] = {0x03, 0xe8};
  size_t pong_length = 0;
  size_t close_length = 0;
  uint8_t *pong = require_encoded(wsm_pong(pong_body, sizeof(pong_body), nullptr), &pong_length);
  uint8_t *close_wire =
      require_encoded(wsm_close(close_body, sizeof(close_body), nullptr), &close_length);
  size_t wire_length = 0;
  uint8_t *wire = concat(pong, pong_length, close_wire, close_length, &wire_length);
  struct wsm_decoder *decoder = wsm_decoder_create();
  struct wsm_decoding_result result;
  assert_that(decoder, is_non_null);
  result = wsm_decoder_feed(decoder, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.messages_count, is_equal_to(2));
  wsm_test_assert_message(&result.messages[0], WSM_PONG, true, true, pong_body, sizeof(pong_body));
  wsm_test_assert_message(&result.messages[1], WSM_CLOSE, true, true, close_body,
                          sizeof(close_body));
  wsm_decoding_result_free(result);
  wsm_decoder_destroy(decoder);
  free(wire);
  free(pong);
  free(close_wire);
}

int main() {
  auto suite = create_test_suite();
  add_test(suite, one_frame_message);
  add_test(suite, one_frame_binary);
  add_test(suite, two_fragments);
  add_test(suite, three_fragments);
  add_test(suite, control_between_fragments);
  add_test(suite, orphan_continuation);
  add_test(suite, new_data_while_fragmented);
  add_test(suite, binary_while_text_open);
  add_test(suite, text_while_binary_open);
  add_test(suite, split_across_chunks);
  add_test(suite, byte_at_a_time);
  add_test(suite, rfc_unmasked_hello);
  add_test(suite, fragmented_control);
  add_test(suite, pong_and_close);
  auto reporter = create_text_reporter();
  int result = run_test_suite(suite, reporter);
  destroy_test_suite(suite);
  destroy_reporter(reporter);
  return result;
}
