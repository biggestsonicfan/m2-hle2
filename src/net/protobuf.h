/*
 * protobuf.h — just enough protobuf wire format for RPCN's np2_structs.proto.
 *
 * RPCN's room commands carry protobuf payloads (the server uses prost), unlike
 * Login/Create which are plain NUL-terminated strings. Rather than pull a
 * protobuf runtime and a code generator into a tree whose dependencies are all
 * git submodules, this encodes the wire format directly — it is only varints and
 * length-delimited blobs, and the handful of messages involved is listed in
 * rpcn_client.h.
 *
 * ONE TRAP WORTH KNOWING, and it costs an afternoon every time: np2_structs.proto
 * defines `uint8` and `uint16` as MESSAGES (`message uint16 { uint32 value = 1; }`,
 * kept from the flatbuffers port), not scalars. So a field declared
 * `uint16 serverId = 1` is a length-delimited SUBMESSAGE containing a varint, not
 * a bare varint. pb_wrapped() / pb_as_wrapped() exist for exactly that, and
 * getting it wrong reads as an empty message rather than as an error.
 */
#ifndef PROTOBUF_H
#define PROTOBUF_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    PB_WIRE_VARINT = 0,
    PB_WIRE_LEN    = 2,
};

/* ---- Writer -------------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t used;
    bool     ok;
} pb_writer_t;

static inline void pb_writer_init(pb_writer_t *w, uint8_t *buf, uint32_t cap) {
    w->buf = buf; w->cap = cap; w->used = 0; w->ok = true;
}

static inline void pb_put(pb_writer_t *w, uint8_t b) {
    if (!w->ok || w->used >= w->cap) { w->ok = false; return; }
    w->buf[w->used++] = b;
}

static inline void pb_raw_varint(pb_writer_t *w, uint64_t v) {
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        pb_put(w, byte);
    } while (v && w->ok);
}

static inline void pb_varint(pb_writer_t *w, uint32_t field, uint64_t value) {
    pb_raw_varint(w, ((uint64_t)field << 3) | PB_WIRE_VARINT);
    pb_raw_varint(w, value);
}

static inline void pb_bytes(pb_writer_t *w, uint32_t field, const void *data, uint32_t len) {
    pb_raw_varint(w, ((uint64_t)field << 3) | PB_WIRE_LEN);
    pb_raw_varint(w, len);
    if (!w->ok) return;
    if (w->used + len > w->cap) { w->ok = false; return; }
    if (len) memcpy(w->buf + w->used, data, len);
    w->used += len;
}

static inline void pb_string(pb_writer_t *w, uint32_t field, const char *s) {
    pb_bytes(w, field, s, (uint32_t)(s ? strlen(s) : 0));
}

/* Begins a submessage; the token goes to pb_end_sub. The length is back-patched,
 * so nesting needs no second pass. */
static inline uint32_t pb_begin_sub(pb_writer_t *w, uint32_t field) {
    pb_raw_varint(w, ((uint64_t)field << 3) | PB_WIRE_LEN);
    /* Reserve one length byte. Submessages here are far below 128 bytes; if one
     * ever grows past that, pb_end_sub shifts the payload rather than corrupting
     * it. */
    pb_put(w, 0);
    return w->used;   /* token = offset of the first payload byte */
}

static inline void pb_end_sub(pb_writer_t *w, uint32_t token) {
    if (!w->ok) return;
    uint32_t len = w->used - token;

    if (len < 128) { w->buf[token - 1] = (uint8_t)len; return; }

    uint32_t extra = 0;
    for (uint64_t v = len >> 7; v; v >>= 7) extra++;
    if (w->used + extra > w->cap) { w->ok = false; return; }
    memmove(w->buf + token + extra, w->buf + token, len);

    uint32_t at = token - 1;
    uint64_t v = len;
    do {
        uint8_t byte = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) byte |= 0x80;
        w->buf[at++] = byte;
    } while (v);
    w->used += extra;
}

/* A `uint8`/`uint16` wrapper submessage: { uint32 value = 1 }. */
static inline void pb_wrapped(pb_writer_t *w, uint32_t field, uint32_t value) {
    uint32_t token = pb_begin_sub(w, field);
    pb_varint(w, 1, value);
    pb_end_sub(w, token);
}

/* ---- Reader -------------------------------------------------------------- */

typedef struct {
    const uint8_t *data;
    uint32_t       len;
    uint32_t       pos;

    uint32_t       field;
    uint32_t       wire;
    uint64_t       varint;
    const uint8_t *bytes;
    uint32_t       bytes_len;
    bool           ok;
} pb_reader_t;

static inline pb_reader_t pb_reader(const uint8_t *data, uint32_t len) {
    pb_reader_t r;
    memset(&r, 0, sizeof(r));
    r.data = data;
    r.len  = len;
    r.ok   = true;
    return r;
}

static inline bool pb_read_varint(pb_reader_t *r, uint64_t *out) {
    *out = 0;
    uint32_t shift = 0;
    while (r->pos < r->len) {
        uint8_t b = r->data[r->pos++];
        *out |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) return true;
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

/* Advances to the next field. False at the end or on malformed input. */
static inline bool pb_next(pb_reader_t *r) {
    if (!r->ok || r->pos >= r->len) return false;

    uint64_t tag = 0;
    if (!pb_read_varint(r, &tag)) { r->ok = false; return false; }

    r->field     = (uint32_t)(tag >> 3);
    r->wire      = (uint32_t)(tag & 7);
    r->bytes     = NULL;
    r->bytes_len = 0;
    r->varint    = 0;

    switch (r->wire) {
        case PB_WIRE_VARINT:
            if (!pb_read_varint(r, &r->varint)) { r->ok = false; return false; }
            return true;
        case PB_WIRE_LEN: {
            uint64_t len = 0;
            if (!pb_read_varint(r, &len) || r->pos + len > r->len) { r->ok = false; return false; }
            r->bytes     = r->data + r->pos;
            r->bytes_len = (uint32_t)len;
            r->pos      += (uint32_t)len;
            return true;
        }
        case 5:   /* fixed32 */
            if (r->pos + 4 > r->len) { r->ok = false; return false; }
            r->pos += 4;
            return true;
        case 1:   /* fixed64 */
            if (r->pos + 8 > r->len) { r->ok = false; return false; }
            r->pos += 8;
            return true;
        default:
            r->ok = false;
            return false;
    }
}

static inline pb_reader_t pb_sub(const pb_reader_t *r) {
    return pb_reader(r->bytes, r->bytes_len);
}

/* The inner value (field 1) of a uint8/uint16 wrapper submessage. 0 if absent. */
static inline uint32_t pb_as_wrapped(const pb_reader_t *r) {
    if (r->wire != PB_WIRE_LEN || !r->bytes) return 0;
    pb_reader_t sub = pb_reader(r->bytes, r->bytes_len);
    while (pb_next(&sub)) {
        if (sub.field == 1 && sub.wire == PB_WIRE_VARINT) return (uint32_t)sub.varint;
    }
    return 0;
}

static inline void pb_copy_string(const pb_reader_t *r, char *out, uint32_t cap) {
    if (!out || cap == 0) return;
    uint32_t n = r->bytes_len;
    if (n > cap - 1) n = cap - 1;
    if (n && r->bytes) memcpy(out, r->bytes, n);
    out[n] = '\0';
}

#endif /* PROTOBUF_H */
