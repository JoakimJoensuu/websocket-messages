#include "wsf.h"

#include <wsc.h>

#ifdef WSF_HOSTED
#include <stdlib.h>
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum { OPEN_NONE = 0 };

enum : uint8_t { CONTROL_FRAME_BIT = 0x8 };

enum fragment {
  FRAGMENT_COMPLETE = 0,
  FRAGMENT_START,
  FRAGMENT_CONTINUE,
  FRAGMENT_END,
};

struct data_plan {
  uint8_t opcode;
  enum fragment fragment;
};

struct encoder {
  uint8_t open_opcode;
};

static_assert(sizeof(struct encoder) <= sizeof(struct wsf_encoder));
static_assert(alignof(struct encoder) <= alignof(struct wsf_encoder));

[[noreturn]] static void trap() {
#ifdef WSF_HOSTED
  abort();
#else
  unreachable();
#endif
}

static bool is_control(uint8_t opcode) {
  return (opcode & CONTROL_FRAME_BIT) != 0;
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

static enum wsf_status map_status(enum wsc_status status) {
  switch (status) {
  case WSC_INCOMPLETE:
    return WSF_INCOMPLETE;
  case WSC_OK:
    return WSF_OK;
  case WSC_ERR_NO_MEMORY:
    return WSF_ERR_NO_MEMORY;
  case WSC_ERR_LENGTH_NOT_MINIMAL:
    return WSF_ERR_LENGTH_NOT_MINIMAL;
  case WSC_ERR_LENGTH64_MSB:
    return WSF_ERR_LENGTH64_MSB;
#ifndef WSC_HOSTED
  case WSC_ERR_PAYLOAD_EXCEEDS_CAPACITY:
    return WSF_ERR_PAYLOAD_EXCEEDS_CAPACITY;
#endif
  case WSC_ERR_LENGTH_EXCEEDS_SIZE:
    return WSF_ERR_LENGTH_EXCEEDS_SIZE;
  }
  trap();
}

static struct encoder transparent_encoder(const struct wsf_encoder *encoder) {
  struct encoder state;
  memcpy(&state, encoder->opaque, sizeof(state));
  return state;
}

static struct wsf_encoder opaque_encoder(const struct encoder *state) {
  struct wsf_encoder encoder = {0};
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

static uint8_t opcode_for_fragment(struct data_plan plan) {
  if (plan.fragment == FRAGMENT_CONTINUE || plan.fragment == FRAGMENT_END) {
    return WSC_OPCODE_CONTINUATION;
  }
  return plan.opcode;
}

static bool fin_for_fragment(enum fragment fragment) {
  return fragment == FRAGMENT_COMPLETE || fragment == FRAGMENT_END;
}

static void prepare_data(const struct encoder *state, struct data_plan plan) {
  if (plan.fragment == FRAGMENT_COMPLETE || plan.fragment == FRAGMENT_START) {
    if (state->open_opcode != OPEN_NONE) {
      trap();
    }
  } else if (state->open_opcode != plan.opcode) {
    trap();
  }
}

static void commit_data(struct encoder *state, struct data_plan plan) {
  if (plan.fragment == FRAGMENT_START) {
    state->open_opcode = plan.opcode;
  } else if (plan.fragment == FRAGMENT_END) {
    state->open_opcode = OPEN_NONE;
  }
}

static struct wsf_event make_event(const struct wsc_frame *frame, uint8_t opcode, bool start,
                                   bool end) {
  struct wsf_event event = {
      .payload = frame->payload,
      .payload_length = frame->payload_length,
      .opcode = opcode,
      .start = start,
      .end = end,
      .rsv1 = frame->rsv1,
      .rsv2 = frame->rsv2,
      .rsv3 = frame->rsv3,
      .masked = frame->masked,
  };
  memcpy(event.masking_key, frame->masking_key, WSF_MASKING_KEY_LENGTH);
  return event;
}

static enum wsf_status apply_frame(uint8_t *open_opcode, const struct wsc_frame *frame,
                                   struct wsf_event *event) {
  if (frame->opcode == WSC_OPCODE_CONTINUATION) {
    if (*open_opcode == OPEN_NONE) {
      return WSF_ERR_ORPHAN_CONTINUATION;
    }
    *event = make_event(frame, *open_opcode, false, frame->fin);
    if (frame->fin) {
      *open_opcode = OPEN_NONE;
    }
    return WSF_OK;
  }
  if (is_control(frame->opcode)) {
    if (!frame->fin) {
      return WSF_ERR_FRAGMENTED_CONTROL;
    }
    *event = make_event(frame, frame->opcode, true, true);
    return WSF_OK;
  }
  if (*open_opcode != OPEN_NONE) {
    return WSF_ERR_DATA_WHILE_FRAGMENTED;
  }
  *event = make_event(frame, frame->opcode, true, frame->fin);
  if (!frame->fin) {
    *open_opcode = frame->opcode;
  }
  return WSF_OK;
}

struct wsf_encoder wsf_encoder_create() {
  return (struct wsf_encoder){0};
}

#ifdef WSF_HOSTED

struct wsf_decoder {
  struct wsc_decoder *codec;
  struct wsc_decoding_result held;
  uint8_t open_opcode;
  bool dead;
  enum wsf_status last_status;
};

static struct wsf_encoding_result encoding_from_codec(struct wsc_encoding_result encoded) {
  return (struct wsf_encoding_result){
      .status = map_status(encoded.status),
      .data = encoded.data,
      .data_length = encoded.data_length,
  };
}

static struct wsf_encoding_result encode_data(struct wsf_encoder *encoder, struct data_plan plan,
                                              const uint8_t *payload, uint64_t payload_length,
                                              const uint8_t *masking_key) {
  if (encoder == nullptr) {
    trap();
  }
  struct encoder state = transparent_encoder(encoder);
  prepare_data(&state, plan);
  struct wsc_frame frame = make_frame(opcode_for_fragment(plan), fin_for_fragment(plan.fragment),
                                      payload, payload_length, masking_key);
  struct wsf_encoding_result result = encoding_from_codec(wsc_encode(&frame));
  if (result.status == WSF_OK) {
    commit_data(&state, plan);
    *encoder = opaque_encoder(&state);
  }
  return result;
}

static struct wsf_encoding_result encode_control(uint8_t opcode, const uint8_t *payload,
                                                 uint64_t payload_length,
                                                 const uint8_t *masking_key) {
  struct wsc_frame frame = make_frame(opcode, true, payload, payload_length, masking_key);
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

struct wsf_decoder *wsf_decoder_create() {
  struct wsf_decoder *decoder = malloc(sizeof(*decoder));
  if (decoder == nullptr) {
    return nullptr;
  }
  *decoder = (struct wsf_decoder){};
  decoder->codec = wsc_decoder_create();
  if (decoder->codec == nullptr) {
    free(decoder);
    return nullptr;
  }
  return decoder;
}

void wsf_decoder_destroy(struct wsf_decoder *decoder) {
  if (decoder == nullptr) {
    return;
  }
  wsc_decoding_result_free(decoder->held);
  wsc_decoder_destroy(decoder->codec);
  free(decoder);
}

static struct wsf_decoding_result take_codec_frames(struct wsf_decoder *decoder,
                                                    struct wsc_decoding_result codec,
                                                    size_t source_consumed) {
  struct wsf_event *events = nullptr;
  size_t events_count = 0;

  if (0 < codec.frames_count) {
    if (SIZE_MAX / sizeof(*events) < codec.frames_count) {
      wsc_decoding_result_free(codec);
      return (struct wsf_decoding_result){
          .status = WSF_ERR_NO_MEMORY,
          .source_consumed = source_consumed,
      };
    }
    events = malloc(codec.frames_count * sizeof(*events));
    if (events == nullptr) {
      decoder->held = codec;
      return (struct wsf_decoding_result){
          .status = WSF_ERR_NO_MEMORY,
          .source_consumed = source_consumed,
      };
    }
    for (size_t i = 0; i < codec.frames_count; i++) {
      enum wsf_status status =
          apply_frame(&decoder->open_opcode, &codec.frames[i], &events[events_count]);
      if (status != WSF_OK) {
        release_codec_frames(codec, events_count);
        decoder->dead = true;
        decoder->last_status = status;
        if (events_count == 0) {
          free(events);
          events = nullptr;
        }
        return (struct wsf_decoding_result){
            .status = status,
            .events = events,
            .events_count = events_count,
            .source_consumed = source_consumed,
        };
      }
      events_count++;
    }
    release_codec_frames(codec, events_count);
  }

  enum wsf_status status = map_status(codec.status);
  if (is_deadly(codec.status)) {
    decoder->dead = true;
    decoder->last_status = status;
  }
  return (struct wsf_decoding_result){
      .status = status,
      .events = events,
      .events_count = events_count,
      .source_consumed = source_consumed,
  };
}

struct wsf_decoding_result wsf_decoder_feed(struct wsf_decoder *decoder, const uint8_t *source,
                                            size_t source_length) {
  if (decoder == nullptr) {
    trap();
  }
  if (0 < source_length && source == nullptr) {
    trap();
  }
  if (decoder->dead) {
    return (struct wsf_decoding_result){.status = decoder->last_status};
  }
  if (decoder->held.frames != nullptr) {
    struct wsc_decoding_result held = decoder->held;
    decoder->held = (struct wsc_decoding_result){};
    return take_codec_frames(decoder, held, 0);
  }

  struct wsc_decoding_result codec = wsc_decoder_feed(decoder->codec, source, source_length);
  return take_codec_frames(decoder, codec, codec.source_consumed);
}

void wsf_decoding_result_free(struct wsf_decoding_result result) {
  if (result.events == nullptr) {
    return;
  }
  for (size_t i = 0; i < result.events_count; i++) {
    free((void *)result.events[i].payload);
  }
  free((void *)result.events);
}

struct wsf_encoding_result wsf_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                    uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_COMPLETE},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                      uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_COMPLETE},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_start_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_START},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_start_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                            uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_START},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_continue_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                             uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_CONTINUE},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_continue_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                               uint64_t payload_length,
                                               const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_CONTINUE},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_end_text(struct wsf_encoder *encoder, const uint8_t *payload,
                                        uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_END},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_end_binary(struct wsf_encoder *encoder, const uint8_t *payload,
                                          uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_END},
                     payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_ping(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key) {
  return encode_control(WSC_OPCODE_PING, payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_pong(const uint8_t *payload, uint64_t payload_length,
                                    const uint8_t *masking_key) {
  return encode_control(WSC_OPCODE_PONG, payload, payload_length, masking_key);
}

struct wsf_encoding_result wsf_close(const uint8_t *payload, uint64_t payload_length,
                                     const uint8_t *masking_key) {
  return encode_control(WSC_OPCODE_CLOSE, payload, payload_length, masking_key);
}

#else

struct decoder {
  struct wsc_decoder codec;
  uint8_t open_opcode;
  bool dead;
  enum wsf_status last_status;
};

static_assert(sizeof(struct decoder) <= sizeof(struct wsf_decoder));
static_assert(alignof(struct decoder) <= alignof(struct wsf_decoder));

static struct decoder transparent_decoder(const struct wsf_decoder *decoder) {
  struct decoder state;
  memcpy(&state, decoder->opaque, sizeof(state));
  return state;
}

static struct wsf_decoder opaque_decoder(const struct decoder *state) {
  struct wsf_decoder decoder = {0};
  memcpy(decoder.opaque, state, sizeof(*state));
  return decoder;
}

static size_t encode_data(struct wsf_encoder *encoder, uint8_t *destination, struct data_plan plan,
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
  struct wsc_frame frame = make_frame(opcode_for_fragment(plan), fin_for_fragment(plan.fragment),
                                      payload, payload_length, masking_key);
  size_t length = wsc_encode(destination, &frame);
  commit_data(&state, plan);
  *encoder = opaque_encoder(&state);
  return length;
}

static size_t encode_control(uint8_t *destination, uint8_t opcode, const uint8_t *payload,
                             uint64_t payload_length, const uint8_t *masking_key) {
  if (destination == nullptr) {
    trap();
  }
  struct wsc_frame frame = make_frame(opcode, true, payload, payload_length, masking_key);
  return wsc_encode(destination, &frame);
}

void wsf_decoder_init(struct wsf_decoder *decoder, uint8_t *payload, size_t payload_capacity) {
  if (decoder == nullptr) {
    trap();
  }
  struct decoder state = {0};
  wsc_decoder_init(&state.codec, payload, payload_capacity);
  *decoder = opaque_decoder(&state);
}

void wsf_decoder_set_payload_buffer(struct wsf_decoder *decoder, uint8_t *payload,
                                    size_t payload_capacity) {
  if (decoder == nullptr) {
    trap();
  }
  struct decoder state = transparent_decoder(decoder);
  wsc_decoder_set_payload_buffer(&state.codec, payload, payload_capacity);
  *decoder = opaque_decoder(&state);
}

struct wsf_decoding_result wsf_decoder_feed(struct wsf_decoder *decoder, const uint8_t *source,
                                            size_t source_length) {
  if (decoder == nullptr) {
    trap();
  }
  if (0 < source_length && source == nullptr) {
    trap();
  }
  struct decoder state = transparent_decoder(decoder);
  if (state.dead) {
    return (struct wsf_decoding_result){.status = state.last_status};
  }

  struct wsc_decoding_result codec = wsc_decoder_feed(&state.codec, source, source_length);
  if (codec.status != WSC_OK) {
    enum wsf_status status = map_status(codec.status);
    if (is_deadly(codec.status)) {
      state.dead = true;
      state.last_status = status;
    }
    *decoder = opaque_decoder(&state);
    return (struct wsf_decoding_result){
        .status = status,
        .source_consumed = codec.source_consumed,
    };
  }

  struct wsf_event event;
  enum wsf_status status = apply_frame(&state.open_opcode, &codec.frame, &event);
  if (status != WSF_OK) {
    state.dead = true;
    state.last_status = status;
    *decoder = opaque_decoder(&state);
    return (struct wsf_decoding_result){
        .status = status,
        .source_consumed = codec.source_consumed,
    };
  }
  *decoder = opaque_decoder(&state);
  return (struct wsf_decoding_result){
      .status = WSF_OK,
      .event = event,
      .source_consumed = codec.source_consumed,
  };
}

uint64_t wsf_encoded_length(uint64_t payload_length, bool masked) {
  uint8_t dummy = 0;
  struct wsc_frame frame = {
      .payload = 0 < payload_length ? &dummy : nullptr,
      .payload_length = payload_length,
      .masked = masked,
  };
  return wsc_encoded_frame_length(&frame);
}

size_t wsf_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_COMPLETE},
                     payload, payload_length, masking_key);
}

size_t wsf_binary(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                  uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_COMPLETE},
                     payload, payload_length, masking_key);
}

size_t wsf_start_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_START},
                     payload, payload_length, masking_key);
}

size_t wsf_start_binary(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                        uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_START},
                     payload, payload_length, masking_key);
}

size_t wsf_continue_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                         uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_CONTINUE},
                     payload, payload_length, masking_key);
}

size_t wsf_continue_binary(struct wsf_encoder *encoder, uint8_t *destination,
                           const uint8_t *payload, uint64_t payload_length,
                           const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_CONTINUE},
                     payload, payload_length, masking_key);
}

size_t wsf_end_text(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                    uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_TEXT, .fragment = FRAGMENT_END},
                     payload, payload_length, masking_key);
}

size_t wsf_end_binary(struct wsf_encoder *encoder, uint8_t *destination, const uint8_t *payload,
                      uint64_t payload_length, const uint8_t *masking_key) {
  return encode_data(encoder, destination,
                     (struct data_plan){.opcode = WSC_OPCODE_BINARY, .fragment = FRAGMENT_END},
                     payload, payload_length, masking_key);
}

size_t wsf_ping(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key) {
  return encode_control(destination, WSC_OPCODE_PING, payload, payload_length, masking_key);
}

size_t wsf_pong(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                const uint8_t *masking_key) {
  return encode_control(destination, WSC_OPCODE_PONG, payload, payload_length, masking_key);
}

size_t wsf_close(uint8_t *destination, const uint8_t *payload, uint64_t payload_length,
                 const uint8_t *masking_key) {
  return encode_control(destination, WSC_OPCODE_CLOSE, payload, payload_length, masking_key);
}

#endif
