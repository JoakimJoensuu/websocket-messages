#ifndef WSF_TEST_H
#define WSF_TEST_H

enum : unsigned { WSF_TEST_HEADER_BASE = 2 };

enum : uint8_t {
  WSF_TEST_FIN_BIT      = 0x80,
  WSF_TEST_OPCODE_MASK  = 0x0F,
  WSF_TEST_CONTINUATION = 0x0,
};

static inline void wsf_test_assert_event(const struct wsf_event *actual, uint8_t opcode, bool start,
                                         bool end, const uint8_t *payload, size_t payload_length) {
  assert_that(actual->opcode, is_equal_to(opcode));
  assert_that(actual->start, is_equal_to(start));
  assert_that(actual->end, is_equal_to(end));
  assert_that(actual->payload_length, is_equal_to(payload_length));
  if (0 < payload_length) {
    assert_that(actual->payload, is_non_null);
    assert_that(memcmp(actual->payload, payload, payload_length), is_equal_to(0));
  }
}

static inline void wsf_test_assert_frame_header(const uint8_t *wire, bool fin, uint8_t opcode) {
  unsigned expected = (unsigned)opcode & (unsigned)WSF_TEST_OPCODE_MASK;
  if (fin) {
    expected |= WSF_TEST_FIN_BIT;
  }
  assert_that(wire[0], is_equal_to(expected));
}

static inline size_t wsf_test_craft(uint8_t *destination, size_t destination_capacity, bool fin,
                                    uint8_t opcode, const uint8_t *payload, size_t payload_length) {
  size_t total = WSF_TEST_HEADER_BASE + payload_length;
  assert_that(total <= destination_capacity, is_true);
  destination[0] = (uint8_t)((fin ? (unsigned)WSF_TEST_FIN_BIT : 0U) |
                             ((unsigned)opcode & (unsigned)WSF_TEST_OPCODE_MASK));
  destination[1] = (uint8_t)payload_length;
  if (0 < payload_length) {
    memcpy(destination + WSF_TEST_HEADER_BASE, payload, payload_length);
  }
  return total;
}

#endif /* WSF_TEST_H */
