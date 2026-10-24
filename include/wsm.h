#ifndef WSM_H
#define WSM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum wsm_status : int8_t {
  WSM_INCOMPLETE                      = 1,
  WSM_OK                              = 0,
  WSM_ERR_NO_MEMORY                   = -1,
  WSM_ERR_PROTOCOL_LENGTH_NOT_MINIMAL = -2,
  WSM_ERR_PROTOCOL_LENGTH64_MSB       = -3,
#ifndef WSM_HOSTED
  WSM_ERR_PAYLOAD_EXCEEDS_CAPACITY = -4,
#endif
  WSM_ERR_LENGTH_EXCEEDS_SIZE            = -5,
  WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION   = -6,
  WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED = -7,
  WSM_ERR_PROTOCOL_FRAGMENTED_CONTROL    = -8,
};

enum wsm_message_type : uint8_t {
  WSM_TEXT   = 0x1,
  WSM_BINARY = 0x2,
  WSM_CLOSE  = 0x8,
  WSM_PING   = 0x9,
  WSM_PONG   = 0xA,
};

enum wsm_masking_key : unsigned { WSM_MASKING_KEY_LENGTH = 4 };

/**
 * Inbound message. Data pieces of one message share @c wsm_message.type (TEXT
 * or BINARY). @c wsm_message.start is the first piece; @c wsm_message.end is
 * the last. A one-frame message has both. Control has both and is never
 * fragmented.
 *
 * @c wsm_message.payload is @c wsm_message.payload_length bytes, not a C
 * string. TEXT is not NUL-terminated.
 */
struct wsm_message {
  const uint8_t *payload;
  uint64_t payload_length;
  uint8_t masking_key[WSM_MASKING_KEY_LENGTH];
  enum wsm_message_type type;
  bool start;
  bool end;
  bool rsv1;
  bool rsv2;
  bool rsv3;
  bool masked;
};

struct wsm_encoding_result {
  enum wsm_status status;
  uint8_t *data;
  size_t data_length;
};

struct wsm_decoding_result {
  enum wsm_status status;
#ifdef WSM_HOSTED
  const struct wsm_message *messages;
  size_t messages_count;
#elifndef WSM_HOSTED
  struct wsm_message message;
#endif
  size_t source_consumed;
};

static constexpr size_t wsm_encoder_opaque_size =
    sizeof(enum wsm_message_type) + (alignof(enum wsm_message_type) - 1);

/**
 * Outbound fragmentation state. Contents are private.
 *
 * Mixing TEXT continue/end with an open BINARY message (and the reverse) is a
 * programming error.
 */
struct wsm_encoder {
  alignas(enum wsm_message_type) unsigned char opaque[wsm_encoder_opaque_size];
};

struct wsm_encoder wsm_encoder_create();

#ifdef WSM_HOSTED

struct wsm_decoder;

/** @return Decoder, or nullptr on OOM. */
struct wsm_decoder *wsm_decoder_create();

/**
 * @param decoder May be nullptr.
 */
void wsm_decoder_destroy(struct wsm_decoder *decoder);

/**
 * Parse @p source into messages. Incomplete frames stay in the decoder.
 *
 * Control messages are emitted as themselves, including between data pieces.
 *
 * The caller owns @c wsm_decoding_result.messages and each message's payload;
 * free them with wsm_decoding_result_free. A later feed does not invalidate
 * prior results.
 *
 * @c wsm_decoding_result.source_consumed is how many bytes of @p source were
 * accepted; re-feed the remainder after @c WSM_ERR_NO_MEMORY. A later feed may
 * return the recovered messages with @c wsm_decoding_result.source_consumed 0;
 * re-feed that same remainder.
 *
 * @c WSM_ERR_NO_MEMORY does not kill the decoder. Length errors and sequencing
 * errors leave the decoder unusable until a new one is created. Messages
 * returned in the result are still owned by the caller.
 */
struct wsm_decoding_result wsm_decoder_feed(struct wsm_decoder *decoder, const uint8_t *source,
                                            size_t source_length);

/**
 * Frees @c wsm_decoding_result.messages and each message's payload. No-op if
 * there are no messages. After return those pointers must not be used. Safe to
 * call once per feed result.
 */
void wsm_decoding_result_free(struct wsm_decoding_result result);

/**
 * Encode functions write one frame.
 *
 * @p masking_key of nullptr means unmasked. Non-null must address
 * @c WSM_MASKING_KEY_LENGTH bytes; this library does not generate keys.
 *
 * @c wsm_text / @c wsm_binary are a one-frame message (FIN=1). @c wsm_start_*
 * is TEXT or BINARY with FIN=0. Later @c wsm_continue_* is CONTINUATION with
 * FIN=0. @c wsm_end_* is CONTINUATION with FIN=1.
 *
 * Control may be encoded while a data message is open. Control is never
 * fragmented.
 *
 * On success the caller frees @c wsm_encoding_result.data. On
 * @c WSM_ERR_NO_MEMORY, @c wsm_encoding_result.data is nullptr and
 * @c wsm_encoding_result.data_length is 0.
 */
struct wsm_encoding_result wsm_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                    uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                      uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_start_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_start_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                            uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_continue_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                             uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_continue_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                               uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_end_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                        uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_end_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key);
struct wsm_encoding_result wsm_ping(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key);
struct wsm_encoding_result wsm_pong(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key);
struct wsm_encoding_result wsm_close(const uint8_t *payload, uint64_t payload_length,
                                     const uint8_t *masking_key);

#elifndef WSM_HOSTED

static constexpr size_t wsm_decoder_opaque_size = 256;

/**
 * Contents are private.
 */
struct wsm_decoder {
  alignas(max_align_t) unsigned char opaque[wsm_decoder_opaque_size];
};

/**
 * Prepare @p decoder to parse. Payload bytes are written into @p payload.
 * @p payload_capacity is the largest payload accepted.
 */
void wsm_decoder_init(struct wsm_decoder *decoder, uint8_t *payload, size_t payload_capacity);

/**
 * Replace the buffer that receives payload bytes. @p payload_capacity is the
 * largest payload accepted. Use this after @c WSM_ERR_PAYLOAD_EXCEEDS_CAPACITY,
 * or before the next frame.
 */
void wsm_decoder_set_payload_buffer(struct wsm_decoder *decoder, uint8_t *payload,
                                    size_t payload_capacity);

/**
 * Parse @p source into at most one message. Incomplete frames stay in the
 * decoder.
 *
 * Payload bytes are written into the buffer installed on @p decoder.
 * @c WSM_OK means @c wsm_decoding_result.message is complete. Its payload
 * addresses that buffer. @c WSM_INCOMPLETE means more source is required.
 * @c wsm_decoding_result.source_consumed is how many bytes of @p source were
 * accepted. Re-feed the unconsumed suffix.
 * On @c WSM_ERR_PAYLOAD_EXCEEDS_CAPACITY the payload was not accepted. Pass a
 * larger buffer to wsm_decoder_set_payload_buffer and re-feed the suffix.
 * Length errors and sequencing errors leave the decoder unusable until
 * wsm_decoder_init.
 */
struct wsm_decoding_result wsm_decoder_feed(struct wsm_decoder *decoder, const uint8_t *source,
                                            size_t source_length);

uint64_t wsm_encoded_length(uint64_t payload_length, bool masked);

/**
 * Write one frame into @p destination.
 *
 * @p destination must have room for wsm_encoded_length(@p payload_length,
 * @p masking_key != nullptr). @p masking_key of nullptr means unmasked.
 * Non-null must address @c WSM_MASKING_KEY_LENGTH bytes; this library does not
 * generate keys.
 *
 * @c wsm_text / @c wsm_binary are a one-frame message (FIN=1). @c wsm_start_*
 * is TEXT or BINARY with FIN=0. Later @c wsm_continue_* is CONTINUATION with
 * FIN=0. @c wsm_end_* is CONTINUATION with FIN=1.
 *
 * Control may be encoded while a data message is open. Control is never
 * fragmented.
 *
 * @return Bytes written (same as wsm_encoded_length).
 */
size_t wsm_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                uint64_t payload_length, const uint8_t *masking_key);
size_t wsm_binary(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                  uint64_t payload_length, const uint8_t *masking_key);
size_t wsm_start_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key);
size_t wsm_start_binary(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                        uint64_t payload_length, const uint8_t *masking_key);
size_t wsm_continue_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                         uint64_t payload_length, const uint8_t *masking_key);
size_t wsm_continue_binary(struct wsm_encoder *encoder, uint8_t *destination,
                           const uint8_t *payload, uint64_t payload_length,
                           const uint8_t *masking_key);
size_t wsm_end_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                    uint64_t payload_length, const uint8_t *masking_key);
size_t wsm_end_binary(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key);
size_t wsm_ping(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key);
size_t wsm_pong(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key);
size_t wsm_close(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                 const uint8_t *masking_key);

#endif

#endif /* WSM_H */
