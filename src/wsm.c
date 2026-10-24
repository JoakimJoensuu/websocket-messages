#include "wsm.h"

#include <wsc.h>

#ifdef WSM_HOSTED
#include <stdlib.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum open_state : uint8_t {
  OPEN_NONE   = 0,
  OPEN_TEXT   = WSM_TEXT,
  OPEN_BINARY = WSM_BINARY,
};

enum control_bit : uint8_t { CONTROL_FRAME_BIT = 0x8 };

enum fragment : uint8_t {
  FRAGMENT_COMPLETE = 0,
  FRAGMENT_START,
  FRAGMENT_CONTINUE,
  FRAGMENT_END,
};

struct data_plan {
  enum wsm_type type;
  enum fragment fragment;
};

struct encoder {
  enum open_state open;
};

static_assert(sizeof(struct encoder) <= sizeof(struct wsm_encoder));
static_assert(alignof(struct encoder) <= alignof(struct wsm_encoder));

[[noreturn]] static void trap() {
#ifdef WSM_HOSTED
  abort();
#else
  unreachable();
#endif
}

static bool is_control_opcode(uint8_t opcode) {
  return (opcode & CONTROL_FRAME_BIT) != 0;
}

static enum wsm_type type_from_opcode(uint8_t opcode) {
  enum wsm_type type = WSM_TEXT;
  memcpy(&type, &opcode, sizeof(type));
  return type;
}

static enum open_state open_from_data_opcode(uint8_t opcode) {
  enum open_state open = OPEN_NONE;
  memcpy(&open, &opcode, sizeof(open));
  return open;
}

static bool is_deadly(enum wsc_status status) {
  switch (status) {
  case WSC_ERR_LENGTH_NOT_MINIMAL:
  case WSC_ERR_LENGTH64_MSB:
  case WSC_ERR_LENGTH_EXCEEDS_SIZE:
    return true;
  default:
    return false;
  }
}

static enum wsm_status map_status(enum wsc_status status) {
  switch (status) {
  case WSC_INCOMPLETE:
    return WSM_INCOMPLETE;
  case WSC_OK:
    return WSM_OK;
  case WSC_ERR_NO_MEMORY:
    return WSM_ERR_NO_MEMORY;
  case WSC_ERR_LENGTH_NOT_MINIMAL:
    return WSM_ERR_LENGTH_NOT_MINIMAL;
  case WSC_ERR_LENGTH64_MSB:
    return WSM_ERR_LENGTH64_MSB;
#ifndef WSC_HOSTED
  case WSC_ERR_PAYLOAD_EXCEEDS_CAPACITY:
    return WSM_ERR_PAYLOAD_EXCEEDS_CAPACITY;
#endif
  case WSC_ERR_LENGTH_EXCEEDS_SIZE:
    return WSM_ERR_LENGTH_EXCEEDS_SIZE;
  }
  trap();
}

static struct encoder transparent_encoder(const struct wsm_encoder *encoder) {
  struct encoder state;
  memcpy(&state, encoder->opaque, sizeof(state));
  return state;
}

static struct wsm_encoder opaque_encoder(const struct encoder *state) {
  struct wsm_encoder encoder = {0};
  memcpy(encoder.opaque, state, sizeof(*state));
  return encoder;
}

static struct wsc_frame make_frame(uint8_t opcode, bool fin, const uint8_t *payload,
                                   uint64_t payload_length, const uint8_t *masking_key) {
  struct wsc_frame frame = {
      .payload = payload,
      .payload_length = payload_length,
      .opcode = opcode,
      .fin = fin,
      .masked = masking_key != nullptr,
  };
  if (masking_key != nullptr) {
    memcpy(frame.masking_key, masking_key, WSC_MASKING_KEY_LENGTH);
  }
  return frame;
}

static uint8_t frame_opcode(struct data_plan plan) {
  if (plan.fragment == FRAGMENT_CONTINUE || plan.fragment == FRAGMENT_END) {
    return WSC_OPCODE_CONTINUATION;
  }
  return (uint8_t)plan.type;
}

static bool fin_for_fragment(enum fragment fragment) {
  return fragment == FRAGMENT_COMPLETE || fragment == FRAGMENT_END;
}

static void prepare_data(const struct encoder *state, struct data_plan plan) {
  if (plan.fragment == FRAGMENT_COMPLETE || plan.fragment == FRAGMENT_START) {
    if (state->open != OPEN_NONE) {
      trap();
    }
  } else if (state->open != open_from_data_opcode((uint8_t)plan.type)) {
    trap();
  }
}

static void commit_data(struct encoder *state, struct data_plan plan) {
  if (plan.fragment == FRAGMENT_START) {
    state->open = open_from_data_opcode((uint8_t)plan.type);
  } else if (plan.fragment == FRAGMENT_END) {
    state->open = OPEN_NONE;
  }
}

static struct wsm_message make_message(const struct wsc_frame *frame, enum wsm_type type,
                                       bool start, bool end) {
  struct wsm_message message = {
      .payload = frame->payload,
      .payload_length = frame->payload_length,
      .type = type,
      .start = start,
      .end = end,
      .rsv1 = frame->rsv1,
      .rsv2 = frame->rsv2,
      .rsv3 = frame->rsv3,
      .masked = frame->masked,
  };
  memcpy(message.masking_key, frame->masking_key, WSM_MASKING_KEY_LENGTH);
  return message;
}

static enum wsm_status apply_frame(enum open_state *open, const struct wsc_frame *frame,
                                   struct wsm_message *message) {
  if (frame->opcode == WSC_OPCODE_CONTINUATION) {
    if (*open == OPEN_NONE) {
      return WSM_ERR_ORPHAN_CONTINUATION;
    }
    *message = make_message(frame, type_from_opcode((uint8_t)*open), false, frame->fin);
    if (frame->fin) {
      *open = OPEN_NONE;
    }
    return WSM_OK;
  }
  if (is_control_opcode(frame->opcode)) {
    if (!frame->fin) {
      return WSM_ERR_FRAGMENTED_CONTROL;
    }
    *message = make_message(frame, type_from_opcode(frame->opcode), true, true);
    return WSM_OK;
  }
  if (*open != OPEN_NONE) {
    return WSM_ERR_DATA_WHILE_FRAGMENTED;
  }
  *message = make_message(frame, type_from_opcode(frame->opcode), true, frame->fin);
  if (!frame->fin) {
    *open = open_from_data_opcode(frame->opcode);
  }
  return WSM_OK;
}

struct wsm_encoder wsm_encoder_create() {
  return (struct wsm_encoder){0};
}

#ifdef WSM_HOSTED

struct wsm_decoder {
  struct wsc_decoder *codec;
  struct wsc_decoding_result held;
  enum open_state open;
  bool dead;
  enum wsm_status last_status;
};

static struct wsm_encoding_result encoding_from_codec(struct wsc_encoding_result encoded) {
  return (struct wsm_encoding_result){
      .status = map_status(encoded.status),
      .data = encoded.data,
      .data_length = encoded.data_length,
  };
}

static struct wsm_encoding_result encode_data(struct wsm_encoder *encoder, struct data_plan plan,
                                              const uint8_t *payload, uint64_t payload_length,
                                              const uint8_t *masking_key) {
  if (encoder == nullptr) {
    trap();
  }
  struct encoder state = transparent_encoder(encoder);
  prepare_data(&state, plan);
  struct wsc_frame frame = make_frame(frame_opcode(plan), fin_for_fragment(plan.fragment), payload,
                                      payload_length, masking_key);
  struct wsm_encoding_result result = encoding_from_codec(wsc_encode(&frame));
  if (result.status == WSM_OK) {
    commit_data(&state, plan);
    *encoder = opaque_encoder(&state);
  }
  return result;
}

static struct wsm_encoding_result encode_control(enum wsm_type type, const uint8_t *payload,
                                                 uint64_t payload_length,
                                                 const uint8_t *masking_key) {
  struct wsc_frame frame = make_frame((uint8_t)type, true, payload, payload_length, masking_key);
  return encoding_from_codec(wsc_encode(&frame));
}

static void release_codec_frames(struct wsc_decoding_result codec, size_t taken_count) {
  if (codec.frames == nullptr) {
    return;
  }
  for (size_t i = taken_count; i < codec.frames_count; i++) {
    free((void *)codec.frames[i].payload);
  }
  free((void *)codec.frames);
}

struct wsm_decoder *wsm_decoder_create() {
  struct wsm_decoder *decoder = malloc(sizeof(*decoder));
  if (decoder == nullptr) {
    return nullptr;
  }
  *decoder = (struct wsm_decoder){};
  decoder->codec = wsc_decoder_create();
  if (decoder->codec == nullptr) {
    free(decoder);
    return nullptr;
  }
  return decoder;
}

void wsm_decoder_destroy(struct wsm_decoder *decoder) {
  if (decoder == nullptr) {
    return;
  }
  wsc_decoding_result_free(decoder->held);
  wsc_decoder_destroy(decoder->codec);
  free(decoder);
}

static struct wsm_decoding_result take_codec_frames(struct wsm_decoder *decoder,
                                                    struct wsc_decoding_result codec,
                                                    size_t source_consumed) {
  struct wsm_message *messages = nullptr;
  size_t messages_count = 0;

  if (0 < codec.frames_count) {
    if (SIZE_MAX / sizeof(*messages) < codec.frames_count) {
      wsc_decoding_result_free(codec);
      return (struct wsm_decoding_result){
          .status = WSM_ERR_NO_MEMORY,
          .source_consumed = source_consumed,
      };
    }
    messages = malloc(codec.frames_count * sizeof(*messages));
    if (messages == nullptr) {
      decoder->held = codec;
      return (struct wsm_decoding_result){
          .status = WSM_ERR_NO_MEMORY,
          .source_consumed = source_consumed,
      };
    }
    for (size_t i = 0; i < codec.frames_count; i++) {
      enum wsm_status status =
          apply_frame(&decoder->open, &codec.frames[i], &messages[messages_count]);
      if (status != WSM_OK) {
        release_codec_frames(codec, messages_count);
        decoder->dead = true;
        decoder->last_status = status;
        if (messages_count == 0) {
          free(messages);
          messages = nullptr;
        }
        return (struct wsm_decoding_result){
            .status = status,
            .messages = messages,
            .messages_count = messages_count,
            .source_consumed = source_consumed,
        };
      }
      messages_count++;
    }
    release_codec_frames(codec, messages_count);
  }

  enum wsm_status status = map_status(codec.status);
  if (is_deadly(codec.status)) {
    decoder->dead = true;
    decoder->last_status = status;
  }
  return (struct wsm_decoding_result){
      .status = status,
      .messages = messages,
      .messages_count = messages_count,
      .source_consumed = source_consumed,
  };
}

struct wsm_decoding_result wsm_decoder_feed(struct wsm_decoder *decoder, const uint8_t *source,
                                            size_t source_length) {
  if (decoder == nullptr) {
    trap();
  }
  if (0 < source_length && source == nullptr) {
    trap();
  }
  if (decoder->dead) {
    return (struct wsm_decoding_result){.status = decoder->last_status};
  }
  if (decoder->held.frames != nullptr) {
    struct wsc_decoding_result held = decoder->held;
    decoder->held = (struct wsc_decoding_result){};
    return take_codec_frames(decoder, held, 0);
  }

  struct wsc_decoding_result codec = wsc_decoder_feed(decoder->codec, source, source_length);
  return take_codec_frames(decoder, codec, codec.source_consumed);
}

void wsm_decoding_result_free(struct wsm_decoding_result result) {
  if (result.messages == nullptr) {
    return;
  }
  for (size_t i = 0; i < result.messages_count; i++) {
    free((void *)result.messages[i].payload);
  }
  free((void *)result.messages);
}

struct wsm_encoding_result wsm_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                    uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_COMPLETE},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                      uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_COMPLETE},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_start_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_START},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_start_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                            uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_START},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_continue_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                             uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_CONTINUE},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_continue_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                               uint64_t payload_length,
                                               const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_CONTINUE},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_end_text(struct wsm_encoder *encoder, const uint8_t *payload,
                                        uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_END},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_end_binary(struct wsm_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_END},
                     payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_ping(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key) {
  return encode_control(WSM_PING, payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_pong(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key) {
  return encode_control(WSM_PONG, payload, payload_length, masking_key);
}

struct wsm_encoding_result wsm_close(const uint8_t *payload, uint64_t payload_length,
                                     const uint8_t *masking_key) {
  return encode_control(WSM_CLOSE, payload, payload_length, masking_key);
}

#else

struct decoder {
  struct wsc_decoder codec;
  enum open_state open;
  bool dead;
  enum wsm_status last_status;
};

static_assert(sizeof(struct decoder) <= sizeof(struct wsm_decoder));
static_assert(alignof(struct decoder) <= alignof(struct wsm_decoder));

static struct decoder transparent_decoder(const struct wsm_decoder *decoder) {
  struct decoder state;
  memcpy(&state, decoder->opaque, sizeof(state));
  return state;
}

static struct wsm_decoder opaque_decoder(const struct decoder *state) {
  struct wsm_decoder decoder = {0};
  memcpy(decoder.opaque, state, sizeof(*state));
  return decoder;
}

static size_t encode_data(struct wsm_encoder *encoder, uint8_t *destination, struct data_plan plan,
                          const uint8_t *payload, uint64_t payload_length,
                          const uint8_t *masking_key) {
  if (encoder == nullptr) {
    trap();
  }
  if (destination == nullptr) {
    trap();
  }
  struct encoder state = transparent_encoder(encoder);
  prepare_data(&state, plan);
  struct wsc_frame frame = make_frame(frame_opcode(plan), fin_for_fragment(plan.fragment), payload,
                                      payload_length, masking_key);
  size_t length = wsc_encode(destination, &frame);
  commit_data(&state, plan);
  *encoder = opaque_encoder(&state);
  return length;
}

static size_t encode_control(uint8_t *destination, enum wsm_type type, const uint8_t *payload,
                             uint64_t payload_length, const uint8_t *masking_key) {
  if (destination == nullptr) {
    trap();
  }
  struct wsc_frame frame = make_frame((uint8_t)type, true, payload, payload_length, masking_key);
  return wsc_encode(destination, &frame);
}

void wsm_decoder_init(struct wsm_decoder *decoder, uint8_t *payload, size_t payload_capacity) {
  if (decoder == nullptr) {
    trap();
  }
  struct decoder state = {0};
  wsc_decoder_init(&state.codec, payload, payload_capacity);
  *decoder = opaque_decoder(&state);
}

void wsm_decoder_set_payload_buffer(struct wsm_decoder *decoder, uint8_t *payload,
                                    size_t payload_capacity) {
  if (decoder == nullptr) {
    trap();
  }
  struct decoder state = transparent_decoder(decoder);
  wsc_decoder_set_payload_buffer(&state.codec, payload, payload_capacity);
  *decoder = opaque_decoder(&state);
}

struct wsm_decoding_result wsm_decoder_feed(struct wsm_decoder *decoder, const uint8_t *source,
                                            size_t source_length) {
  if (decoder == nullptr) {
    trap();
  }
  if (0 < source_length && source == nullptr) {
    trap();
  }
  struct decoder state = transparent_decoder(decoder);
  if (state.dead) {
    return (struct wsm_decoding_result){.status = state.last_status};
  }

  struct wsc_decoding_result codec = wsc_decoder_feed(&state.codec, source, source_length);
  if (codec.status != WSC_OK) {
    enum wsm_status status = map_status(codec.status);
    if (is_deadly(codec.status)) {
      state.dead = true;
      state.last_status = status;
    }
    *decoder = opaque_decoder(&state);
    return (struct wsm_decoding_result){
        .status = status,
        .source_consumed = codec.source_consumed,
    };
  }

  struct wsm_message message;
  enum wsm_status status = apply_frame(&state.open, &codec.frame, &message);
  if (status != WSM_OK) {
    state.dead = true;
    state.last_status = status;
    *decoder = opaque_decoder(&state);
    return (struct wsm_decoding_result){
        .status = status,
        .source_consumed = codec.source_consumed,
    };
  }
  *decoder = opaque_decoder(&state);
  return (struct wsm_decoding_result){
      .status = WSM_OK,
      .message = message,
      .source_consumed = codec.source_consumed,
  };
}

uint64_t wsm_encoded_length(uint64_t payload_length, bool masked) {
  uint8_t dummy = 0;
  struct wsc_frame frame = {
      .payload = 0 < payload_length ? &dummy : nullptr,
      .payload_length = payload_length,
      .masked = masked,
  };
  return wsc_encoded_frame_length(&frame);
}

size_t wsm_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_COMPLETE}, payload,
                     payload_length, masking_key);
}

size_t wsm_binary(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                  uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_COMPLETE}, payload,
                     payload_length, masking_key);
}

size_t wsm_start_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_START}, payload,
                     payload_length, masking_key);
}

size_t wsm_start_binary(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                        uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_START}, payload,
                     payload_length, masking_key);
}

size_t wsm_continue_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                         uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_CONTINUE}, payload,
                     payload_length, masking_key);
}

size_t wsm_continue_binary(struct wsm_encoder *encoder, uint8_t *destination,
                           const uint8_t *payload, uint64_t payload_length,
                           const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_CONTINUE}, payload,
                     payload_length, masking_key);
}

size_t wsm_end_text(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                    uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_TEXT, .fragment = FRAGMENT_END}, payload,
                     payload_length, masking_key);
}

size_t wsm_end_binary(struct wsm_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.type = WSM_BINARY, .fragment = FRAGMENT_END}, payload,
                     payload_length, masking_key);
}

size_t wsm_ping(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key) {
  return encode_control(destination, WSM_PING, payload, payload_length, masking_key);
}

size_t wsm_pong(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key) {
  return encode_control(destination, WSM_PONG, payload, payload_length, masking_key);
}

size_t wsm_close(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                 const uint8_t *masking_key) {
  return encode_control(destination, WSM_CLOSE, payload, payload_length, masking_key);
}

#endif
