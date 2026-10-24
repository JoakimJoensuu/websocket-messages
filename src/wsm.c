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
  enum wsm_message_type type;
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

static enum wsm_message_type type_from_opcode(uint8_t opcode) {
  enum wsm_message_type type = WSM_TEXT;
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
  case WSC_ERR_PROTOCOL_LENGTH_NOT_MINIMAL:
  case WSC_ERR_PROTOCOL_LENGTH64_MSB:
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
  case WSC_ERR_PROTOCOL_LENGTH_NOT_MINIMAL:
    return WSM_ERR_PROTOCOL_LENGTH_NOT_MINIMAL;
  case WSC_ERR_PROTOCOL_LENGTH64_MSB:
    return WSM_ERR_PROTOCOL_LENGTH64_MSB;
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

enum length_field : unsigned {
  HEADER_BASE_LENGTH    = 2,
  LENGTH7_MAX           = 125,
  LENGTH16_FIELD_LENGTH = 2,
  LENGTH64_FIELD_LENGTH = 8,
};

struct payload_budget {
  bool fits;
  uint64_t payload_length;
};

static size_t mask_length(bool masked) {
  return masked ? WSM_MASKING_KEY_LENGTH : 0;
}

static uint64_t encoded_frame_length(uint64_t payload_length, bool masked) {
  uint64_t header = HEADER_BASE_LENGTH + mask_length(masked);
  if (LENGTH7_MAX < payload_length) {
    header += UINT16_MAX < payload_length ? LENGTH64_FIELD_LENGTH : LENGTH16_FIELD_LENGTH;
  }
  return header + payload_length;
}

static struct payload_budget payload_budget(size_t max_frame_length, bool masked) {
  size_t header7 = HEADER_BASE_LENGTH + mask_length(masked);
  size_t header16 = header7 + LENGTH16_FIELD_LENGTH;
  size_t header64 = header7 + LENGTH64_FIELD_LENGTH;
  if (header64 <= max_frame_length) {
    uint64_t room = (uint64_t)max_frame_length - header64;
    if (UINT16_MAX < room) {
      return (struct payload_budget){.fits = true, .payload_length = room};
    }
  }
  if (header16 <= max_frame_length) {
    uint64_t room = (uint64_t)max_frame_length - header16;
    if (LENGTH7_MAX < room) {
      return (struct payload_budget){
          .fits = true,
          .payload_length = room <= UINT16_MAX ? room : UINT16_MAX,
      };
    }
  }
  if (header7 <= max_frame_length) {
    uint64_t room = (uint64_t)max_frame_length - header7;
    return (struct payload_budget){
        .fits = true,
        .payload_length = room <= LENGTH7_MAX ? room : LENGTH7_MAX,
    };
  }
  return (struct payload_budget){.fits = false};
}

static enum fragment fragment_at(size_t index, size_t count) {
  if (count == 1) {
    return FRAGMENT_COMPLETE;
  }
  if (index == 0) {
    return FRAGMENT_START;
  }
  if (index + 1 == count) {
    return FRAGMENT_END;
  }
  return FRAGMENT_CONTINUE;
}

struct wsm_frame_plan wsm_frame_plan(uint64_t payload_length, bool masked,
                                     size_t max_frame_length) {
  struct payload_budget budget = payload_budget(max_frame_length, masked);
  if (!budget.fits || (0 < payload_length && budget.payload_length == 0)) {
    return (struct wsm_frame_plan){.status = WSM_ERR_MAX_FRAME_LENGTH};
  }
  if (payload_length == 0) {
    return (struct wsm_frame_plan){
        .status = WSM_OK,
        .frames_count = 1,
        .data_length = encoded_frame_length(0, masked),
    };
  }
  uint64_t full = payload_length / budget.payload_length;
  uint64_t last = payload_length % budget.payload_length;
  uint64_t count = last == 0 ? full : full + 1;
  if (SIZE_MAX < count) {
    return (struct wsm_frame_plan){.status = WSM_ERR_LENGTH_EXCEEDS_SIZE};
  }
  uint64_t full_length = encoded_frame_length(budget.payload_length, masked);
  if (0 < full && UINT64_MAX / full_length < full) {
    return (struct wsm_frame_plan){.status = WSM_ERR_LENGTH_EXCEEDS_SIZE};
  }
  uint64_t data_length = full * full_length;
  if (0 < last) {
    uint64_t tail = encoded_frame_length(last, masked);
    if (UINT64_MAX - data_length < tail) {
      return (struct wsm_frame_plan){.status = WSM_ERR_LENGTH_EXCEEDS_SIZE};
    }
    data_length += tail;
  }
  return (struct wsm_frame_plan){
      .status = WSM_OK,
      .frames_count = (size_t)count,
      .data_length = data_length,
  };
}

static void require_closed_encoder(const struct wsm_encoder *encoder) {
  if (encoder == nullptr) {
    trap();
  }
  struct encoder state = transparent_encoder(encoder);
  if (state.open != OPEN_NONE) {
    trap();
  }
}

static const uint8_t *frame_key(const uint8_t *masking_keys, size_t index) {
  if (masking_keys == nullptr) {
    return nullptr;
  }
  return masking_keys + (index * WSM_MASKING_KEY_LENGTH);
}

struct payload_span {
  uint64_t offset;
  uint64_t length;
};

static const uint8_t *frame_payload(const uint8_t *payload, struct payload_span span) {
  if (span.length == 0) {
    return nullptr;
  }
  if (SIZE_MAX < span.offset) {
    trap();
  }
  return payload + (size_t)span.offset;
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

static struct wsm_message make_message(const uint8_t *payload, uint64_t payload_length,
                                       const uint8_t *masking_key, enum wsm_message_type type,
                                       bool rsv1, bool rsv2, bool rsv3, bool masked) {
  struct wsm_message message = {
      .payload = payload,
      .payload_length = payload_length,
      .type = type,
      .rsv1 = rsv1,
      .rsv2 = rsv2,
      .rsv3 = rsv3,
      .masked = masked,
  };
  memcpy(message.masking_key, masking_key, WSM_MASKING_KEY_LENGTH);
  return message;
}

static struct wsm_message message_from_frame(const struct wsc_frame *frame,
                                             enum wsm_message_type type) {
  return make_message(frame->payload, frame->payload_length, frame->masking_key, type, frame->rsv1,
                      frame->rsv2, frame->rsv3, frame->masked);
}

struct wsm_encoder wsm_encoder_create() {
  return (struct wsm_encoder){0};
}

#ifdef WSM_HOSTED

struct wsm_decoder {
  struct wsc_decoder *codec;
  struct wsc_decoding_result held;
  uint8_t *data;
  size_t data_length;
  uint8_t data_masking_key[WSM_MASKING_KEY_LENGTH];
  enum open_state open;
  bool data_rsv1;
  bool data_rsv2;
  bool data_rsv3;
  bool data_masked;
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

static struct wsm_encoding_result encode_control(enum wsm_message_type type, const uint8_t *payload,
                                                 uint64_t payload_length,
                                                 const uint8_t *masking_key) {
  struct wsc_frame frame = make_frame((uint8_t)type, true, payload, payload_length, masking_key);
  return encoding_from_codec(wsc_encode(&frame));
}

static void release_codec_frames(struct wsc_decoding_result codec) {
  if (codec.frames == nullptr) {
    return;
  }
  for (size_t i = 0; i < codec.frames_count; i++) {
    free((void *)codec.frames[i].payload);
  }
  free((void *)codec.frames);
}

static void hold_codec_frames(struct wsm_decoder *decoder, struct wsc_frame *frames, size_t index,
                              size_t count) {
  if (0 < index) {
    memmove(frames, frames + index, (count - index) * sizeof(*frames));
  }
  decoder->held = (struct wsc_decoding_result){
      .frames = frames,
      .frames_count = count - index,
  };
}

static void remember_data(struct wsm_decoder *decoder, const struct wsc_frame *frame) {
  decoder->open = open_from_data_opcode(frame->opcode);
  decoder->data_masked = frame->masked;
  decoder->data_rsv1 = frame->rsv1;
  decoder->data_rsv2 = frame->rsv2;
  decoder->data_rsv3 = frame->rsv3;
  memcpy(decoder->data_masking_key, frame->masking_key, WSM_MASKING_KEY_LENGTH);
}

static void take_payload(struct wsc_frame *frame, uint8_t **data, size_t *data_length) {
  *data = (uint8_t *)frame->payload;
  *data_length = (size_t)frame->payload_length;
  frame->payload = nullptr;
}

static enum wsm_status append_data(struct wsm_decoder *decoder, struct wsc_frame *frame) {
  if (SIZE_MAX - decoder->data_length < frame->payload_length) {
    return WSM_ERR_LENGTH_EXCEEDS_SIZE;
  }
  size_t extra = (size_t)frame->payload_length;
  if (extra == 0) {
    return WSM_OK;
  }
  size_t total = decoder->data_length + extra;
  uint8_t *grown = realloc(decoder->data, total);
  if (grown == nullptr) {
    return WSM_ERR_NO_MEMORY;
  }
  memcpy(grown + decoder->data_length, frame->payload, extra);
  free((void *)frame->payload);
  frame->payload = nullptr;
  decoder->data = grown;
  decoder->data_length = total;
  return WSM_OK;
}

static struct wsm_message finish_data(struct wsm_decoder *decoder) {
  struct wsm_message message =
      make_message(decoder->data, decoder->data_length, decoder->data_masking_key,
                   type_from_opcode((uint8_t)decoder->open), decoder->data_rsv1, decoder->data_rsv2,
                   decoder->data_rsv3, decoder->data_masked);
  decoder->data = nullptr;
  decoder->data_length = 0;
  decoder->open = OPEN_NONE;
  return message;
}

static enum wsm_status accept_frame(struct wsm_decoder *decoder, struct wsc_frame *frame,
                                    struct wsm_message *message, bool *emit) {
  *emit = false;
  if (frame->opcode == WSC_OPCODE_CONTINUATION) {
    if (decoder->open == OPEN_NONE) {
      return WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION;
    }
    enum wsm_status status = append_data(decoder, frame);
    if (status != WSM_OK) {
      return status;
    }
    if (frame->fin) {
      *message = finish_data(decoder);
      *emit = true;
    }
    return WSM_OK;
  }
  if (is_control_opcode(frame->opcode)) {
    if (!frame->fin) {
      return WSM_ERR_PROTOCOL_FRAGMENTED_CONTROL;
    }
    *message = message_from_frame(frame, type_from_opcode(frame->opcode));
    frame->payload = nullptr;
    *emit = true;
    return WSM_OK;
  }
  if (decoder->open != OPEN_NONE) {
    return WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED;
  }
  remember_data(decoder, frame);
  take_payload(frame, &decoder->data, &decoder->data_length);
  if (frame->fin) {
    *message = finish_data(decoder);
    *emit = true;
  }
  return WSM_OK;
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
  free(decoder->data);
  wsc_decoding_result_free(decoder->held);
  wsc_decoder_destroy(decoder->codec);
  free(decoder);
}

static struct wsm_message *issued_messages(struct wsm_message *messages, size_t messages_count) {
  if (messages_count == 0) {
    free(messages);
    return nullptr;
  }
  return messages;
}

static struct wsm_decoding_result take_codec_frames(struct wsm_decoder *decoder,
                                                    struct wsc_decoding_result codec,
                                                    size_t source_consumed) {
  struct wsm_message *messages = nullptr;
  size_t messages_count = 0;

  struct wsc_frame *frames = (struct wsc_frame *)codec.frames;
  if (0 < codec.frames_count) {
    if (SIZE_MAX / sizeof(*messages) < codec.frames_count) {
      decoder->held = codec;
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
      bool emit = false;
      enum wsm_status status = accept_frame(decoder, &frames[i], &messages[messages_count], &emit);
      if (status == WSM_ERR_NO_MEMORY) {
        hold_codec_frames(decoder, frames, i, codec.frames_count);
        return (struct wsm_decoding_result){
            .status = status,
            .messages = issued_messages(messages, messages_count),
            .messages_count = messages_count,
            .source_consumed = source_consumed,
        };
      }
      if (status != WSM_OK) {
        release_codec_frames(codec);
        decoder->dead = true;
        decoder->last_status = status;
        return (struct wsm_decoding_result){
            .status = status,
            .messages = issued_messages(messages, messages_count),
            .messages_count = messages_count,
            .source_consumed = source_consumed,
        };
      }
      if (emit) {
        messages_count++;
      }
    }
    release_codec_frames(codec);
    messages = issued_messages(messages, messages_count);
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

static struct wsm_frames_result frames_error(enum wsm_status status) {
  return (struct wsm_frames_result){.status = status};
}

static struct wsm_frames_result encode_frames(struct wsm_encoder *encoder,
                                              enum wsm_message_type type, const uint8_t *payload,
                                              uint64_t payload_length, const uint8_t *masking_keys,
                                              size_t max_frame_length) {
  require_closed_encoder(encoder);
  if (0 < payload_length && payload == nullptr) {
    trap();
  }
  struct wsm_frame_plan plan =
      wsm_frame_plan(payload_length, masking_keys != nullptr, max_frame_length);
  if (plan.status != WSM_OK) {
    return frames_error(plan.status);
  }
  if (SIZE_MAX < plan.data_length) {
    return frames_error(WSM_ERR_LENGTH_EXCEEDS_SIZE);
  }
  if (plan.frames_count == 0) {
    return frames_error(WSM_ERR_MAX_FRAME_LENGTH);
  }
  struct payload_budget budget = payload_budget(max_frame_length, masking_keys != nullptr);
  uint8_t **frames = (uint8_t **)malloc(plan.frames_count * sizeof(*frames));
  size_t *lengths = malloc(plan.frames_count * sizeof(*lengths));
  if (frames == nullptr || lengths == nullptr) {
    free((void *)frames);
    free(lengths);
    return frames_error(WSM_ERR_NO_MEMORY);
  }
  uint64_t offset = 0;
  for (size_t i = 0; i < plan.frames_count; i++) {
    uint64_t remaining = payload_length - offset;
    uint64_t piece = remaining < budget.payload_length ? remaining : budget.payload_length;
    struct data_plan data = {.type = type, .fragment = fragment_at(i, plan.frames_count)};
    struct payload_span span = {.offset = offset, .length = piece};
    struct wsc_frame frame =
        make_frame(frame_opcode(data), fin_for_fragment(data.fragment),
                   frame_payload(payload, span), piece, frame_key(masking_keys, i));
    struct wsc_encoding_result encoded = wsc_encode(&frame);
    if (encoded.status != WSC_OK) {
      for (size_t j = 0; j < i; j++) {
        free(frames[j]);
      }
      free((void *)frames);
      free(lengths);
      return frames_error(map_status(encoded.status));
    }
    frames[i] = encoded.data;
    lengths[i] = encoded.data_length;
    offset += piece;
  }
  return (struct wsm_frames_result){
      .status = WSM_OK,
      .frames = (const uint8_t *const *)frames,
      .frame_lengths = lengths,
      .frames_count = plan.frames_count,
  };
}

struct wsm_frames_result wsm_text_frames(struct wsm_encoder *encoder, const uint8_t *payload,
                                         uint64_t payload_length, const uint8_t *masking_keys,
                                         size_t max_frame_length) {
  return encode_frames(encoder, WSM_TEXT, payload, payload_length, masking_keys, max_frame_length);
}

struct wsm_frames_result wsm_binary_frames(struct wsm_encoder *encoder, const uint8_t *payload,
                                           uint64_t payload_length, const uint8_t *masking_keys,
                                           size_t max_frame_length) {
  return encode_frames(encoder, WSM_BINARY, payload, payload_length, masking_keys,
                       max_frame_length);
}

void wsm_frames_result_free(struct wsm_frames_result result) {
  if (result.frames == nullptr) {
    free((void *)result.frame_lengths);
    return;
  }
  for (size_t i = 0; i < result.frames_count; i++) {
    free((void *)result.frames[i]);
  }
  free((void *)result.frames);
  free((void *)result.frame_lengths);
}

#else

struct decoder {
  struct wsc_decoder codec;
  uint8_t *payload;
  size_t payload_capacity;
  size_t filled;
  uint8_t masking_key[WSM_MASKING_KEY_LENGTH];
  enum open_state open;
  bool frame_open;
  bool rsv1;
  bool rsv2;
  bool rsv3;
  bool masked;
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

static size_t encode_control(uint8_t *destination, enum wsm_message_type type,
                             const uint8_t *payload, uint64_t payload_length,
                             const uint8_t *masking_key) {
  if (destination == nullptr) {
    trap();
  }
  struct wsc_frame frame = make_frame((uint8_t)type, true, payload, payload_length, masking_key);
  return wsc_encode(destination, &frame);
}

static void point_codec_at_suffix(struct decoder *state) {
  uint8_t *suffix = state->filled == 0 ? state->payload : state->payload + state->filled;
  wsc_decoder_set_payload_buffer(&state->codec, suffix, state->payload_capacity - state->filled);
}

void wsm_decoder_init(struct wsm_decoder *decoder, uint8_t *payload, size_t payload_capacity) {
  if (decoder == nullptr) {
    trap();
  }
  struct decoder state = {0};
  state.payload = payload;
  state.payload_capacity = payload_capacity;
  wsc_decoder_init(&state.codec, payload, payload_capacity);
  *decoder = opaque_decoder(&state);
}

void wsm_decoder_set_payload_buffer(struct wsm_decoder *decoder, uint8_t *payload,
                                    size_t payload_capacity) {
  if (decoder == nullptr) {
    trap();
  }
  struct decoder state = transparent_decoder(decoder);
  if (payload_capacity < state.filled) {
    trap();
  }
  if (payload != state.payload && 0 < state.filled) {
    memcpy(payload, state.payload, state.filled);
  }
  state.payload = payload;
  state.payload_capacity = payload_capacity;
  if (!state.frame_open) {
    point_codec_at_suffix(&state);
  }
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
  if (!state.frame_open) {
    point_codec_at_suffix(&state);
  }

  struct wsc_decoding_result codec = wsc_decoder_feed(&state.codec, source, source_length);
  if (codec.status != WSC_OK) {
    enum wsm_status status = map_status(codec.status);
    state.frame_open = codec.status == WSC_INCOMPLETE;
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
  state.frame_open = false;

  struct wsc_frame frame = codec.frame;
  if (frame.opcode == WSC_OPCODE_CONTINUATION) {
    if (state.open == OPEN_NONE) {
      state.dead = true;
      state.last_status = WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION;
      *decoder = opaque_decoder(&state);
      return (struct wsm_decoding_result){
          .status = WSM_ERR_PROTOCOL_ORPHAN_CONTINUATION,
          .source_consumed = codec.source_consumed,
      };
    }
  } else if (is_control_opcode(frame.opcode)) {
    if (!frame.fin) {
      state.dead = true;
      state.last_status = WSM_ERR_PROTOCOL_FRAGMENTED_CONTROL;
      *decoder = opaque_decoder(&state);
      return (struct wsm_decoding_result){
          .status = WSM_ERR_PROTOCOL_FRAGMENTED_CONTROL,
          .source_consumed = codec.source_consumed,
      };
    }
    *decoder = opaque_decoder(&state);
    return (struct wsm_decoding_result){
        .status = WSM_OK,
        .message = message_from_frame(&frame, type_from_opcode(frame.opcode)),
        .source_consumed = codec.source_consumed,
    };
  } else if (state.open != OPEN_NONE) {
    state.dead = true;
    state.last_status = WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED;
    *decoder = opaque_decoder(&state);
    return (struct wsm_decoding_result){
        .status = WSM_ERR_PROTOCOL_DATA_WHILE_FRAGMENTED,
        .source_consumed = codec.source_consumed,
    };
  } else {
    state.open = open_from_data_opcode(frame.opcode);
    state.masked = frame.masked;
    state.rsv1 = frame.rsv1;
    state.rsv2 = frame.rsv2;
    state.rsv3 = frame.rsv3;
    memcpy(state.masking_key, frame.masking_key, WSM_MASKING_KEY_LENGTH);
  }

  if (SIZE_MAX - state.filled < frame.payload_length) {
    state.dead = true;
    state.last_status = WSM_ERR_LENGTH_EXCEEDS_SIZE;
    *decoder = opaque_decoder(&state);
    return (struct wsm_decoding_result){
        .status = WSM_ERR_LENGTH_EXCEEDS_SIZE,
        .source_consumed = codec.source_consumed,
    };
  }
  state.filled += (size_t)frame.payload_length;
  if (!frame.fin) {
    *decoder = opaque_decoder(&state);
    return (struct wsm_decoding_result){
        .status = WSM_INCOMPLETE,
        .source_consumed = codec.source_consumed,
    };
  }

  struct wsm_message message = make_message(state.payload, state.filled, state.masking_key,
                                            type_from_opcode((uint8_t)state.open), state.rsv1,
                                            state.rsv2, state.rsv3, state.masked);
  state.filled = 0;
  state.open = OPEN_NONE;
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

static struct wsm_frames_result encode_frames(struct wsm_encoder *encoder, uint8_t *destination,
                                              enum wsm_message_type type, const uint8_t *payload,
                                              uint64_t payload_length, const uint8_t *masking_keys,
                                              size_t max_frame_length) {
  require_closed_encoder(encoder);
  if (0 < payload_length && payload == nullptr) {
    trap();
  }
  struct wsm_frame_plan plan =
      wsm_frame_plan(payload_length, masking_keys != nullptr, max_frame_length);
  if (plan.status != WSM_OK) {
    return (struct wsm_frames_result){.status = plan.status};
  }
  if (SIZE_MAX < plan.data_length) {
    return (struct wsm_frames_result){.status = WSM_ERR_LENGTH_EXCEEDS_SIZE};
  }
  if (plan.frames_count == 0) {
    return (struct wsm_frames_result){.status = WSM_ERR_MAX_FRAME_LENGTH};
  }
  if (destination == nullptr) {
    trap();
  }
  struct payload_budget budget = payload_budget(max_frame_length, masking_keys != nullptr);
  size_t written = 0;
  uint64_t offset = 0;
  for (size_t i = 0; i < plan.frames_count; i++) {
    uint64_t remaining = payload_length - offset;
    uint64_t piece = remaining < budget.payload_length ? remaining : budget.payload_length;
    struct data_plan data = {.type = type, .fragment = fragment_at(i, plan.frames_count)};
    struct payload_span span = {.offset = offset, .length = piece};
    struct wsc_frame frame =
        make_frame(frame_opcode(data), fin_for_fragment(data.fragment),
                   frame_payload(payload, span), piece, frame_key(masking_keys, i));
    written += wsc_encode(destination + written, &frame);
    offset += piece;
  }
  return (struct wsm_frames_result){
      .status = WSM_OK,
      .frames_count = plan.frames_count,
      .data_length = written,
  };
}

struct wsm_frames_result wsm_text_frames(struct wsm_encoder *encoder, uint8_t *destination,
                                         const uint8_t *payload, uint64_t payload_length,
                                         const uint8_t *masking_keys, size_t max_frame_length) {
  return encode_frames(encoder, destination, WSM_TEXT, payload, payload_length, masking_keys,
                       max_frame_length);
}

struct wsm_frames_result wsm_binary_frames(struct wsm_encoder *encoder, uint8_t *destination,
                                           const uint8_t *payload, uint64_t payload_length,
                                           const uint8_t *masking_keys, size_t max_frame_length) {
  return encode_frames(encoder, destination, WSM_BINARY, payload, payload_length, masking_keys,
                       max_frame_length);
}

#endif
