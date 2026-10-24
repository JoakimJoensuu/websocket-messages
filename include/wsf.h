#ifndef WSF_H
#define WSF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum wsf_status {
  WSF_INCOMPLETE             = 1,
  WSF_OK                     = 0,
  WSF_ERR_NO_MEMORY          = -1,
  WSF_ERR_LENGTH_NOT_MINIMAL = -2,
  WSF_ERR_LENGTH64_MSB       = -3,
#ifndef WSF_HOSTED
  WSF_ERR_PAYLOAD_EXCEEDS_CAPACITY = -4,
#endif
  WSF_ERR_LENGTH_EXCEEDS_SIZE   = -5,
  WSF_ERR_ORPHAN_CONTINUATION   = -6,
  WSF_ERR_DATA_WHILE_FRAGMENTED = -7,
  WSF_ERR_FRAGMENTED_CONTROL    = -8,
};

enum wsf_opcode : uint8_t {
  WSF_OPCODE_TEXT   = 0x1,
  WSF_OPCODE_BINARY = 0x2,
  WSF_OPCODE_CLOSE  = 0x8,
  WSF_OPCODE_PING   = 0x9,
  WSF_OPCODE_PONG   = 0xA,
};

enum wsf_masking_key : unsigned { WSF_MASKING_KEY_LENGTH = 4 };

/**
 * Inbound message fragment or control frame.
 *
 * Data fragments of one message share @c wsf_event.opcode (TEXT or BINARY).
 * @c wsf_event.start is the first fragment; @c wsf_event.end is the last. A
 * one-frame message has both. Control has both and is never fragmented.
 */
struct wsf_event {
  const uint8_t *payload;
  uint64_t payload_length;
  uint8_t masking_key[WSF_MASKING_KEY_LENGTH];
  uint8_t opcode;
  bool start;
  bool end;
  bool rsv1;
  bool rsv2;
  bool rsv3;
  bool masked;
};

struct wsf_encoding_result {
  enum wsf_status status;
  uint8_t *data;
  size_t data_length;
};

struct wsf_decoding_result {
  enum wsf_status status;
#ifdef WSF_HOSTED
  const struct wsf_event *events;
  size_t events_count;
#elifndef WSF_HOSTED
  struct wsf_event event;
#endif
  size_t source_consumed;
};

static constexpr size_t wsf_encoder_opaque_size = sizeof(uint8_t) + (alignof(uint8_t) - 1);

/**
 * Outbound fragmentation state. Contents are private.
 *
 * Mixing TEXT continue/end with an open BINARY message (and the reverse) is a
 * programming error.
 */
struct wsf_encoder {
  alignas(uint8_t) unsigned char opaque[wsf_encoder_opaque_size];
};

struct wsf_encoder wsf_encoder_create();

#ifdef WSF_HOSTED

struct wsf_decoder;

/** @return Decoder, or nullptr on OOM. */
struct wsf_decoder *wsf_decoder_create();

/**
 * @param decoder May be nullptr.
 */
void wsf_decoder_destroy(struct wsf_decoder *decoder);

/**
 * Parse @p source into message events. Incomplete frames stay in the decoder.
 *
 * Control frames are emitted as themselves, including between data fragments.
 *
 * The caller owns @c wsf_decoding_result.events and each event's payload; free
 * them with wsf_decoding_result_free. A later feed does not invalidate prior
 * results.
 *
 * @c wsf_decoding_result.source_consumed is how many bytes of @p source were
 * accepted; re-feed the remainder after @c WSF_ERR_NO_MEMORY. A later feed may
 * return the recovered events with @c wsf_decoding_result.source_consumed 0;
 * re-feed that same remainder.
 *
 * @c WSF_ERR_NO_MEMORY does not kill the decoder. Length errors and sequencing
 * errors leave the decoder unusable until a new one is created. Events returned
 * in the result are still owned by the caller.
 */
struct wsf_decoding_result wsf_decoder_feed(struct wsf_decoder *decoder, const uint8_t *source,
                                            size_t source_length);

/**
 * Frees @c wsf_decoding_result.events and each event's payload. No-op if there
 * are no events. After return those pointers must not be used. Safe to call
 * once per feed result.
 */
void wsf_decoding_result_free(struct wsf_decoding_result result);

/**
 * Encode functions write one frame.
 *
 * @p masking_key of nullptr means unmasked. Non-null must address
 * @c WSF_MASKING_KEY_LENGTH bytes; this library does not generate keys.
 *
 * @c wsf_text / @c wsf_binary are a one-frame message (FIN=1). @c wsf_start_*
 * is TEXT or BINARY with FIN=0. Later @c wsf_continue_* is CONTINUATION with
 * FIN=0. @c wsf_end_* is CONTINUATION with FIN=1.
 *
 * Control may be encoded while a data message is open. Control is never
 * fragmented.
 *
 * On success the caller frees @c wsf_encoding_result.data. On
 * @c WSF_ERR_NO_MEMORY, @c wsf_encoding_result.data is nullptr and
 * @c wsf_encoding_result.data_length is 0.
 */
struct wsf_encoding_result wsf_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                    uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                      uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_start_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_start_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                            uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_continue_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                             uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_continue_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                               uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_end_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                        uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_end_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key);
struct wsf_encoding_result wsf_ping(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key);
struct wsf_encoding_result wsf_pong(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key);
struct wsf_encoding_result wsf_close(const uint8_t *payload, uint64_t payload_length,
                                     const uint8_t *masking_key);

#elifndef WSF_HOSTED

static constexpr size_t wsf_decoder_opaque_size = 256;

/**
 * Contents are private.
 */
struct wsf_decoder {
  alignas(max_align_t) unsigned char opaque[wsf_decoder_opaque_size];
};

/**
 * Prepare @p decoder to parse. Payload bytes are written into @p payload.
 * @p payload_capacity is the largest payload accepted.
 */
void wsf_decoder_init(struct wsf_decoder *decoder, uint8_t *payload, size_t payload_capacity);

/**
 * Replace the buffer that receives payload bytes. @p payload_capacity is the
 * largest payload accepted. Use this after @c WSF_ERR_PAYLOAD_EXCEEDS_CAPACITY,
 * or before the next frame.
 */
void wsf_decoder_set_payload_buffer(struct wsf_decoder *decoder, uint8_t *payload,
                                    size_t payload_capacity);

/**
 * Parse @p source into at most one event. Incomplete frames stay in the
 * decoder.
 *
 * Payload bytes are written into the buffer installed on @p decoder.
 * @c WSF_OK means @c wsf_decoding_result.event is complete. Its payload
 * addresses that buffer. @c WSF_INCOMPLETE means more source is required.
 * @c wsf_decoding_result.source_consumed is how many bytes of @p source were
 * accepted. Re-feed the unconsumed suffix.
 * On @c WSF_ERR_PAYLOAD_EXCEEDS_CAPACITY the payload was not accepted. Pass a
 * larger buffer to wsf_decoder_set_payload_buffer and re-feed the suffix.
 * Length errors and sequencing errors leave the decoder unusable until
 * wsf_decoder_init.
 */
struct wsf_decoding_result wsf_decoder_feed(struct wsf_decoder *decoder, const uint8_t *source,
                                            size_t source_length);

uint64_t wsf_encoded_length(uint64_t payload_length, bool masked);

/**
 * Write one frame into @p destination.
 *
 * @p destination must have room for wsf_encoded_length(@p payload_length,
 * @p masking_key != nullptr). @p masking_key of nullptr means unmasked.
 * Non-null must address @c WSF_MASKING_KEY_LENGTH bytes; this library does not
 * generate keys.
 *
 * @c wsf_text / @c wsf_binary are a one-frame message (FIN=1). @c wsf_start_*
 * is TEXT or BINARY with FIN=0. Later @c wsf_continue_* is CONTINUATION with
 * FIN=0. @c wsf_end_* is CONTINUATION with FIN=1.
 *
 * Control may be encoded while a data message is open. Control is never
 * fragmented.
 *
 * @return Bytes written (same as wsf_encoded_length).
 */
size_t wsf_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                uint64_t payload_length, const uint8_t *masking_key);
size_t wsf_binary(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                  uint64_t payload_length, const uint8_t *masking_key);
size_t wsf_start_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key);
size_t wsf_start_binary(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                        uint64_t payload_length, const uint8_t *masking_key);
size_t wsf_continue_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                         uint64_t payload_length, const uint8_t *masking_key);
size_t wsf_continue_binary(struct wsf_encoder *encoder, uint8_t *destination,
                           const uint8_t *payload, uint64_t payload_length,
                           const uint8_t *masking_key);
size_t wsf_end_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                    uint64_t payload_length, const uint8_t *masking_key);
size_t wsf_end_binary(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key);
size_t wsf_ping(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key);
size_t wsf_pong(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key);
size_t wsf_close(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                 const uint8_t *masking_key);

#endif

#endif /* WSF_H */
