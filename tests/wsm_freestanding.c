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

enum { PAYLOAD_CAPACITY = 256, WIRE_CAPACITY = 64, WIRE_PAIR_CAPACITY = 128 };

struct opened {
  struct wsm_decoder decoder;
  uint8_t payload[PAYLOAD_CAPACITY];
};

static size_t encode_into(size_t destination_capacity, size_t encoded_length) {
  assert_that(encoded_length <= destination_capacity, is_true);
  return encoded_length;
}

static void open_decoder(struct opened *opened) {
  wsm_decoder_init(&opened->decoder, opened->payload, sizeof(opened->payload));
}

static struct wsm_decoding_result feed(struct opened *opened, const uint8_t *source,
                                       size_t source_length) {
  return wsm_decoder_feed(&opened->decoder, source, source_length);
}

Ensure(one_frame_message) {
  const uint8_t hello[] = {'H', 'e', 'l', 'l', 'o'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t wire[WIRE_CAPACITY];
  size_t wire_length =
      encode_into(sizeof(wire), wsm_text(&encoder, wire, hello, sizeof(hello), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  wsm_test_assert_frame_header(wire, true, WSM_TEXT);
  result = feed(&opened, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  assert_that(result.source_consumed, is_equal_to(wire_length));
  wsm_test_assert_message(&result.message, WSM_TEXT, hello, sizeof(hello));
}

Ensure(one_frame_binary) {
  const uint8_t body[] = {0x00, 0xff};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t wire[WIRE_CAPACITY];
  size_t wire_length =
      encode_into(sizeof(wire), wsm_binary(&encoder, wire, body, sizeof(body), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  wsm_test_assert_frame_header(wire, true, WSM_BINARY);
  result = feed(&opened, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_BINARY, body, sizeof(body));
}

Ensure(two_fragments) {
  const uint8_t first[] = {'H', 'e'};
  const uint8_t second[] = {'l', 'l', 'o'};
  const uint8_t hello[] = {'H', 'e', 'l', 'l', 'o'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t end[WIRE_CAPACITY];
  size_t start_length =
      encode_into(sizeof(start), wsm_start_text(&encoder, start, first, sizeof(first), nullptr));
  size_t end_length =
      encode_into(sizeof(end), wsm_end_text(&encoder, end, second, sizeof(second), nullptr));
  uint8_t wire[WIRE_PAIR_CAPACITY];
  struct opened opened;
  struct wsm_decoding_result result;
  memcpy(wire, start, start_length);
  memcpy(wire + start_length, end, end_length);
  open_decoder(&opened);
  wsm_test_assert_frame_header(start, false, WSM_TEXT);
  wsm_test_assert_frame_header(end, true, WSM_TEST_CONTINUATION);
  result = feed(&opened, wire, start_length + end_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  assert_that(result.source_consumed, is_equal_to(start_length));
  result = feed(&opened, wire + start_length, end_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_TEXT, hello, sizeof(hello));
}

Ensure(three_fragments) {
  const uint8_t first[] = {'a'};
  const uint8_t middle[] = {'b', 'b'};
  const uint8_t last[] = {'c'};
  const uint8_t body[] = {'a', 'b', 'b', 'c'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t cont[WIRE_CAPACITY];
  uint8_t end[WIRE_CAPACITY];
  size_t start_length =
      encode_into(sizeof(start), wsm_start_binary(&encoder, start, first, sizeof(first), nullptr));
  size_t middle_length = encode_into(
      sizeof(cont), wsm_continue_binary(&encoder, cont, middle, sizeof(middle), nullptr));
  size_t end_length =
      encode_into(sizeof(end), wsm_end_binary(&encoder, end, last, sizeof(last), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  wsm_test_assert_frame_header(cont, false, WSM_TEST_CONTINUATION);
  result = feed(&opened, start, start_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = feed(&opened, cont, middle_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = feed(&opened, end, end_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_BINARY, body, sizeof(body));
}

Ensure(control_between_fragments) {
  const uint8_t first[] = {'H'};
  const uint8_t ping_body[] = {'?'};
  const uint8_t last[] = {'i'};
  const uint8_t text[] = {'H', 'i'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t ping[WIRE_CAPACITY];
  uint8_t end[WIRE_CAPACITY];
  size_t start_length =
      encode_into(sizeof(start), wsm_start_text(&encoder, start, first, sizeof(first), nullptr));
  size_t ping_length =
      encode_into(sizeof(ping), wsm_ping(ping, ping_body, sizeof(ping_body), nullptr));
  size_t end_length =
      encode_into(sizeof(end), wsm_end_text(&encoder, end, last, sizeof(last), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  wsm_test_assert_frame_header(ping, true, WSM_PING);
  result = feed(&opened, start, start_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = feed(&opened, ping, ping_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_PING, ping_body, sizeof(ping_body));
  result = feed(&opened, end, end_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_TEXT, text, sizeof(text));
}

Ensure(orphan_continuation) {
  const uint8_t first[] = {'a'};
  const uint8_t rest[] = {'b'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t cont[WIRE_CAPACITY];
  size_t cont_length = 0;
  struct opened opened;
  struct wsm_decoding_result result;
  encode_into(sizeof(start), wsm_start_text(&encoder, start, first, sizeof(first), nullptr));
  cont_length =
      encode_into(sizeof(cont), wsm_continue_text(&encoder, cont, rest, sizeof(rest), nullptr));
  open_decoder(&opened);
  result = feed(&opened, cont, cont_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION));
  result = feed(&opened, cont, cont_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION));
  assert_that(result.source_consumed, is_equal_to(0));
}

Ensure(new_data_while_fragmented) {
  const uint8_t first[] = {'a'};
  const uint8_t next[] = {'b'};
  struct wsm_encoder open_encoder = wsm_encoder_create();
  struct wsm_encoder other_encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t other[WIRE_CAPACITY];
  size_t start_length = encode_into(
      sizeof(start), wsm_start_text(&open_encoder, start, first, sizeof(first), nullptr));
  size_t next_length =
      encode_into(sizeof(other), wsm_text(&other_encoder, other, next, sizeof(next), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  result = feed(&opened, start, start_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = feed(&opened, other, next_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED));
}

Ensure(binary_while_text_open) {
  const uint8_t text[] = {'t'};
  const uint8_t binary[] = {0x01};
  struct wsm_encoder text_encoder = wsm_encoder_create();
  struct wsm_encoder binary_encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t bin[WIRE_CAPACITY];
  size_t start_length =
      encode_into(sizeof(start), wsm_start_text(&text_encoder, start, text, sizeof(text), nullptr));
  size_t binary_length =
      encode_into(sizeof(bin), wsm_binary(&binary_encoder, bin, binary, sizeof(binary), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  result = feed(&opened, start, start_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = feed(&opened, bin, binary_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED));
}

Ensure(text_while_binary_open) {
  const uint8_t binary[] = {0x02};
  const uint8_t text[] = {'x'};
  struct wsm_encoder binary_encoder = wsm_encoder_create();
  struct wsm_encoder text_encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t msg[WIRE_CAPACITY];
  size_t start_length = encode_into(
      sizeof(start), wsm_start_binary(&binary_encoder, start, binary, sizeof(binary), nullptr));
  size_t text_length =
      encode_into(sizeof(msg), wsm_text(&text_encoder, msg, text, sizeof(text), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  result = feed(&opened, start, start_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = feed(&opened, msg, text_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED));
}

Ensure(split_across_chunks) {
  const uint8_t hello[] = {'h', 'i', '!'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t wire[WIRE_CAPACITY];
  size_t wire_length =
      encode_into(sizeof(wire), wsm_text(&encoder, wire, hello, sizeof(hello), nullptr));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  assert_that(1 < wire_length, is_true);
  result = feed(&opened, wire, 1);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  assert_that(result.source_consumed, is_equal_to(1));
  result = feed(&opened, nullptr, 0);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = feed(&opened, wire + 1, wire_length - 1);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_TEXT, hello, sizeof(hello));
}

Ensure(byte_at_a_time) {
  const uint8_t hello[] = {'h', 'i'};
  const uint8_t key[] = {0x11, 0x22, 0x33, 0x44};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t wire[WIRE_CAPACITY];
  size_t wire_length =
      encode_into(sizeof(wire), wsm_text(&encoder, wire, hello, sizeof(hello), key));
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  for (size_t i = 0; i < wire_length; i++) {
    result = feed(&opened, wire + i, 1);
    assert_that(result.source_consumed, is_equal_to(1));
    if (i + 1 < wire_length) {
      assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
    } else {
      assert_that(result.status, is_equal_to(WSM_OK));
      wsm_test_assert_message(&result.message, WSM_TEXT, hello, sizeof(hello));
      assert_that(result.message.masked, is_true);
    }
  }
}

Ensure(rfc_unmasked_hello) {
  const uint8_t wire[] = {0x81, 0x05, 0x48, 0x65, 0x6c, 0x6c, 0x6f};
  const uint8_t hello[] = {'H', 'e', 'l', 'l', 'o'};
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  result = feed(&opened, wire, sizeof(wire));
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_TEXT, hello, sizeof(hello));
}

Ensure(fragmented_control) {
  uint8_t wire[WSM_TEST_HEADER_BASE];
  size_t wire_length = wsm_test_craft(wire, sizeof(wire), false, WSM_PING, nullptr, 0);
  struct opened opened;
  struct wsm_decoding_result result;
  open_decoder(&opened);
  result = feed(&opened, wire, wire_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PROTOCOL_FRAGMENTED_CONTROL));
}

Ensure(pong_and_close) {
  const uint8_t pong_body[] = {0x01};
  const uint8_t close_body[] = {0x03, 0xe8};
  uint8_t pong[WIRE_CAPACITY];
  uint8_t close_wire[WIRE_CAPACITY];
  struct opened opened;
  struct wsm_decoding_result result;
  size_t pong_length =
      encode_into(sizeof(pong), wsm_pong(pong, pong_body, sizeof(pong_body), nullptr));
  size_t close_length = encode_into(sizeof(close_wire),
                                    wsm_close(close_wire, close_body, sizeof(close_body), nullptr));
  open_decoder(&opened);
  result = feed(&opened, pong, pong_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_PONG, pong_body, sizeof(pong_body));
  result = feed(&opened, close_wire, close_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_CLOSE, close_body, sizeof(close_body));
}

Ensure(payload_exceeds_capacity_while_fragmented) {
  const uint8_t first[] = {'a'};
  const uint8_t middle[] = {'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b'};
  const uint8_t last[] = {'c'};
  const uint8_t body[] = {'a', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'b', 'c'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t start[WIRE_CAPACITY];
  uint8_t cont[WIRE_CAPACITY];
  uint8_t end[WIRE_CAPACITY];
  size_t start_length =
      encode_into(sizeof(start), wsm_start_text(&encoder, start, first, sizeof(first), nullptr));
  size_t middle_length =
      encode_into(sizeof(cont), wsm_continue_text(&encoder, cont, middle, sizeof(middle), nullptr));
  size_t end_length =
      encode_into(sizeof(end), wsm_end_text(&encoder, end, last, sizeof(last), nullptr));
  uint8_t small[sizeof(first)];
  uint8_t large[sizeof(body)];
  struct wsm_decoder decoder;
  struct wsm_decoding_result result;
  size_t header_length = middle_length - sizeof(middle);
  wsm_decoder_init(&decoder, small, sizeof(small));
  result = wsm_decoder_feed(&decoder, start, start_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = wsm_decoder_feed(&decoder, cont, middle_length);
  assert_that(result.status, is_equal_to(WSM_ERR_PAYLOAD_EXCEEDS_CAPACITY));
  assert_that(result.source_consumed, is_equal_to(header_length));
  wsm_decoder_set_payload_buffer(&decoder, large, sizeof(large));
  result = wsm_decoder_feed(&decoder, cont + header_length, middle_length - header_length);
  assert_that(result.status, is_equal_to(WSM_INCOMPLETE));
  result = wsm_decoder_feed(&decoder, end, end_length);
  assert_that(result.status, is_equal_to(WSM_OK));
  wsm_test_assert_message(&result.message, WSM_TEXT, body, sizeof(body));
}

Ensure(encoded_length_matches_write) {
  const uint8_t hello[] = {'h', 'i'};
  struct wsm_encoder encoder = wsm_encoder_create();
  uint8_t wire[WIRE_CAPACITY];
  size_t wire_length = wsm_text(&encoder, wire, hello, sizeof(hello), nullptr);
  assert_that(wire_length, is_equal_to(wsm_encoded_length(sizeof(hello), false)));
  encoder = wsm_encoder_create();
  const uint8_t key[] = {1, 2, 3, 4};
  wire_length = wsm_text(&encoder, wire, hello, sizeof(hello), key);
  assert_that(wire_length, is_equal_to(wsm_encoded_length(sizeof(hello), true)));
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
  add_test(suite, payload_exceeds_capacity_while_fragmented);
  add_test(suite, encoded_length_matches_write);
  auto reporter = create_text_reporter();
  int result = run_test_suite(suite, reporter);
  destroy_test_suite(suite);
  destroy_reporter(reporter);
  return result;
}
