#ifndef WSM_TEST_H
#define WSM_TEST_H

enum : unsigned { WSM_TEST_HEADER_BASE = 2 };

enum : uint8_t {
  WSM_TEST_FIN_BIT      = 0x80,
  WSM_TEST_OPCODE_MASK  = 0x0F,
  WSM_TEST_CONTINUATION = 0x0,
};

static inline void wsm_test_assert_message(const struct wsm_message *actual,
                                           enum wsm_message_type type, bool start, bool end,
                                           const uint8_t *payload, size_t payload_length) {
  assert_that(actual->type, is_equal_to(type));
  assert_that(actual->start, is_equal_to(start));
  assert_that(actual->end, is_equal_to(end));
  assert_that(actual->payload_length, is_equal_to(payload_length));
  if (0 < payload_length) {
    assert_that(actual->payload, is_non_null);
    assert_that(memcmp(actual->payload, payload, payload_length), is_equal_to(0));
  }
}

static inline void wsm_test_assert_frame_header(const uint8_t *wire, bool fin, uint8_t opcode) {
  unsigned expected = (unsigned)opcode & (unsigned)WSM_TEST_OPCODE_MASK;
  if (fin) {
    expected |= WSM_TEST_FIN_BIT;
  }
  assert_that(wire[0], is_equal_to(expected));
}

static inline size_t wsm_test_craft(uint8_t *destination, size_t destination_capacity, bool fin,
                                    uint8_t opcode, const uint8_t *payload, size_t payload_length) {
  size_t total = WSM_TEST_HEADER_BASE + payload_length;
  assert_that(total <= destination_capacity, is_true);
  destination[0] = (uint8_t)((fin ? (unsigned)WSM_TEST_FIN_BIT : 0U) |
                             ((unsigned)opcode & (unsigned)WSM_TEST_OPCODE_MASK));
  destination[1] = (uint8_t)payload_length;
  if (0 < payload_length) {
    memcpy(destination + WSM_TEST_HEADER_BASE, payload, payload_length);
  }
  return total;
}

#endif /* WSM_TEST_H */
