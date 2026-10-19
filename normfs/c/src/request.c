#include "normfs/request.h"

/*@ predicate normfs_request_span_in(struct normfs_request_span s, integer n) =
      s.off <= n && s.len <= n - s.off;

    predicate normfs_request_spans_in{L}(struct normfs_request *req, integer n) =
      normfs_request_span_in(req->write_queue_id, n) &&
      normfs_request_span_in(req->read_queue_id, n) &&
      normfs_request_span_in(req->read_offset_id_raw, n);

    // Every append consumes at least one byte, so the counts never pass the offset.
    predicate normfs_request_counts_in{L}(struct normfs_request *req, integer n) =
      req->write_packets + req->read_encodings <= n;

    predicate normfs_request_packets_in{L}(struct normfs_request_span *p,
                                          integer count, integer cap, integer n) =
      \forall integer i; 0 <= i < count && i < cap ==> normfs_request_span_in(p[i], n);

    predicate normfs_request_varint_status(integer s) =
      s == NORMFS_REQUEST_OK ||
      s == NORMFS_REQUEST_ERR_TRUNCATED ||
      s == NORMFS_REQUEST_ERR_OVERFLOW ||
      s == NORMFS_REQUEST_ERR_NON_CANONICAL;

    // NO_SPACE is decided once the whole buffer is read, never by a nested message.
    predicate normfs_request_message_status(integer s) =
      normfs_request_varint_status(s) ||
      s == NORMFS_REQUEST_ERR_BAD_KEY ||
      s == NORMFS_REQUEST_ERR_BAD_WIRE_TYPE ||
      s == NORMFS_REQUEST_ERR_GROUP;
*/

#define NORMFS_REQUEST_WIRE_VARINT 0u
#define NORMFS_REQUEST_WIRE_I64 1u
#define NORMFS_REQUEST_WIRE_LEN 2u
#define NORMFS_REQUEST_WIRE_START_GROUP 3u
#define NORMFS_REQUEST_WIRE_END_GROUP 4u
#define NORMFS_REQUEST_WIRE_I32 5u

struct normfs_request_value {
	uint64_t value;
	size_t next;
	int status;
};

struct normfs_request_key {
	uint64_t field;
	uint64_t wire;
	size_t next;
	int status;
};

struct normfs_request_field {
	struct normfs_request_span span;
	size_t next;
	int status;
};

/*@ requires off <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    assigns \nothing;
    ensures normfs_request_varint_status(\result.status);
    ensures \result.status == NORMFS_REQUEST_OK ==> off < \result.next <= end;
*/
static struct normfs_request_value
normfs_request_varint(const uint8_t *buf, size_t off, size_t end)
{
	struct normfs_request_value r = {0u, off, NORMFS_REQUEST_ERR_TRUNCATED};
	struct normfs_uintn_varint64_decode_result v;

	if (off == end) return r;
	v = normfs_uintn_varint64_decode(buf + off, end - off);
	if (v.status != NORMFS_UINTN_VARINT_OK) {
		r.status = v.status;
		return r;
	}
	r.value = v.value;
	r.next = off + v.consumed;
	r.status = NORMFS_REQUEST_OK;
	return r;
}

/* The checks and their order follow prost's decode_key. */
/*@ requires off < end;
    requires \valid_read(buf + (0 .. end - 1));
    assigns \nothing;
    ensures normfs_request_varint_status(\result.status) ||
            \result.status == NORMFS_REQUEST_ERR_BAD_KEY;
    ensures \result.status == NORMFS_REQUEST_OK ==>
              off < \result.next <= end &&
              \result.field >= 1 &&
              \result.wire <= NORMFS_REQUEST_WIRE_I32;
*/
static struct normfs_request_key
normfs_request_key(const uint8_t *buf, size_t off, size_t end)
{
	struct normfs_request_key r = {0u, 0u, off, NORMFS_REQUEST_ERR_BAD_KEY};
	struct normfs_request_value v;

	v = normfs_request_varint(buf, off, end);
	if (v.status != NORMFS_REQUEST_OK) {
		r.status = v.status;
		return r;
	}
	if (v.value > 4294967295ull) return r;
	if (v.value % 8u > NORMFS_REQUEST_WIRE_I32) return r;
	if (v.value / 8u == 0u) return r;
	r.field = v.value / 8u;
	r.wire = v.value % 8u;
	r.next = v.next;
	r.status = NORMFS_REQUEST_OK;
	return r;
}

/*@ requires off <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    assigns \nothing;
    ensures normfs_request_varint_status(\result.status);
    ensures \result.status == NORMFS_REQUEST_OK ==>
              off < \result.span.off &&
              \result.span.off + \result.span.len == \result.next &&
              \result.next <= end;
*/
static struct normfs_request_field
normfs_request_len(const uint8_t *buf, size_t off, size_t end)
{
	struct normfs_request_field r = {{0u, 0u}, off, NORMFS_REQUEST_ERR_TRUNCATED};
	struct normfs_request_value v;

	v = normfs_request_varint(buf, off, end);
	if (v.status != NORMFS_REQUEST_OK) {
		r.status = v.status;
		return r;
	}
	if (v.value > end - v.next) return r;
	r.span.off = v.next;
	r.span.len = (size_t)v.value;
	r.next = v.next + (size_t)v.value;
	r.status = NORMFS_REQUEST_OK;
	return r;
}

/* Groups are rejected rather than skipped: no normfs client sends them. */
/*@ requires off <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires wire <= NORMFS_REQUEST_WIRE_I32;
    assigns \nothing;
    ensures normfs_request_varint_status(\result.status) ||
            \result.status == NORMFS_REQUEST_ERR_GROUP;
    ensures \result.status == NORMFS_REQUEST_OK ==> off < \result.next <= end;
*/
static struct normfs_request_value
normfs_request_skip(const uint8_t *buf, size_t off, size_t end, uint64_t wire)
{
	struct normfs_request_value r = {0u, off, NORMFS_REQUEST_ERR_TRUNCATED};
	struct normfs_request_field f;
	size_t width;

	if (wire == NORMFS_REQUEST_WIRE_VARINT)
		return normfs_request_varint(buf, off, end);
	if (wire == NORMFS_REQUEST_WIRE_LEN) {
		f = normfs_request_len(buf, off, end);
		r.next = f.next;
		r.status = f.status;
		return r;
	}
	if (wire == NORMFS_REQUEST_WIRE_START_GROUP ||
	    wire == NORMFS_REQUEST_WIRE_END_GROUP) {
		r.status = NORMFS_REQUEST_ERR_GROUP;
		return r;
	}
	width = wire == NORMFS_REQUEST_WIRE_I64 ? 8u : 4u;
	if (end - off < width) return r;
	r.next = off + width;
	r.status = NORMFS_REQUEST_OK;
	return r;
}

/*@ requires start <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires \valid(req);
    requires normfs_request_span_in(req->read_offset_id_raw, start);
    assigns req->read_offset_id_raw;
    ensures normfs_request_message_status(\result);
    ensures \result == NORMFS_REQUEST_OK ==>
              normfs_request_span_in(req->read_offset_id_raw, end);
*/
static int
normfs_request_decode_id(const uint8_t *buf, size_t start, size_t end,
    struct normfs_request *req)
{
	size_t off = start;
	struct normfs_request_key k;
	struct normfs_request_field f;
	struct normfs_request_value s;

	/*@ loop invariant start <= off <= end;
	    loop invariant normfs_request_span_in(req->read_offset_id_raw, off);
	    loop assigns off, k, f, s, req->read_offset_id_raw;
	    loop variant end - off;
	*/
	while (off < end) {
		k = normfs_request_key(buf, off, end);
		if (k.status != NORMFS_REQUEST_OK) return k.status;
		if (k.field == 1u) {
			if (k.wire != NORMFS_REQUEST_WIRE_LEN)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			f = normfs_request_len(buf, k.next, end);
			if (f.status != NORMFS_REQUEST_OK) return f.status;
			req->read_offset_id_raw = f.span;
			off = f.next;
		} else {
			s = normfs_request_skip(buf, k.next, end, k.wire);
			if (s.status != NORMFS_REQUEST_OK) return s.status;
			off = s.next;
		}
	}
	return NORMFS_REQUEST_OK;
}

/*@ requires start <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires \valid(req);
    requires normfs_request_span_in(req->read_offset_id_raw, start);
    assigns req->read_offset_id_raw, req->has_read_offset_id, req->read_offset_type;
    ensures normfs_request_message_status(\result);
    ensures \result == NORMFS_REQUEST_OK ==>
              normfs_request_span_in(req->read_offset_id_raw, end);
*/
static int
normfs_request_decode_offset(const uint8_t *buf, size_t start, size_t end,
    struct normfs_request *req)
{
	size_t off = start;
	struct normfs_request_key k;
	struct normfs_request_field f;
	struct normfs_request_value v;
	int status;

	/*@ loop invariant start <= off <= end;
	    loop invariant normfs_request_span_in(req->read_offset_id_raw, off);
	    loop assigns off, k, f, v, status, req->read_offset_id_raw,
	                 req->has_read_offset_id, req->read_offset_type;
	    loop variant end - off;
	*/
	while (off < end) {
		k = normfs_request_key(buf, off, end);
		if (k.status != NORMFS_REQUEST_OK) return k.status;
		if (k.field == 1u) {
			if (k.wire != NORMFS_REQUEST_WIRE_LEN)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			f = normfs_request_len(buf, k.next, end);
			if (f.status != NORMFS_REQUEST_OK) return f.status;
			status = normfs_request_decode_id(buf, f.span.off,
			    f.span.off + f.span.len, req);
			if (status != NORMFS_REQUEST_OK) return status;
			req->has_read_offset_id = 1;
			off = f.next;
		} else if (k.field == 2u) {
			if (k.wire != NORMFS_REQUEST_WIRE_VARINT)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			v = normfs_request_varint(buf, k.next, end);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			req->read_offset_type = v.value;
			off = v.next;
		} else {
			v = normfs_request_skip(buf, k.next, end, k.wire);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			off = v.next;
		}
	}
	return NORMFS_REQUEST_OK;
}

/*@ requires start <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires \valid(req);
    assigns req->setup_version;
    ensures normfs_request_message_status(\result);
*/
static int
normfs_request_decode_setup(const uint8_t *buf, size_t start, size_t end,
    struct normfs_request *req)
{
	size_t off = start;
	struct normfs_request_key k;
	struct normfs_request_value v;

	/*@ loop invariant start <= off <= end;
	    loop assigns off, k, v, req->setup_version;
	    loop variant end - off;
	*/
	while (off < end) {
		k = normfs_request_key(buf, off, end);
		if (k.status != NORMFS_REQUEST_OK) return k.status;
		if (k.field == 1u) {
			if (k.wire != NORMFS_REQUEST_WIRE_VARINT)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			v = normfs_request_varint(buf, k.next, end);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			req->setup_version = v.value;
		} else {
			v = normfs_request_skip(buf, k.next, end, k.wire);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
		}
		off = v.next;
	}
	return NORMFS_REQUEST_OK;
}

/*@ requires start <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires \valid(req);
    assigns req->ping_sequence, req->ping_client_timestamp_ns;
    ensures normfs_request_message_status(\result);
*/
static int
normfs_request_decode_ping(const uint8_t *buf, size_t start, size_t end,
    struct normfs_request *req)
{
	size_t off = start;
	struct normfs_request_key k;
	struct normfs_request_value v;

	/*@ loop invariant start <= off <= end;
	    loop assigns off, k, v, req->ping_sequence, req->ping_client_timestamp_ns;
	    loop variant end - off;
	*/
	while (off < end) {
		k = normfs_request_key(buf, off, end);
		if (k.status != NORMFS_REQUEST_OK) return k.status;
		if (k.field == 1u || k.field == 2u) {
			if (k.wire != NORMFS_REQUEST_WIRE_VARINT)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			v = normfs_request_varint(buf, k.next, end);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			if (k.field == 1u)
				req->ping_sequence = v.value;
			else
				req->ping_client_timestamp_ns = v.value;
		} else {
			v = normfs_request_skip(buf, k.next, end, k.wire);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
		}
		off = v.next;
	}
	return NORMFS_REQUEST_OK;
}

/*@ requires start <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires \valid(req);
    requires packets_cap == 0 || \valid(packets + (0 .. packets_cap - 1));
    requires \separated(req, packets + (0 .. packets_cap - 1));
    requires normfs_request_spans_in(req, start);
    requires normfs_request_counts_in(req, start);
    requires normfs_request_packets_in(packets, req->write_packets, packets_cap, start);
    assigns req->write_id, req->write_queue_id, req->write_packets,
            packets[0 .. packets_cap - 1];
    ensures normfs_request_message_status(\result);
    ensures \result == NORMFS_REQUEST_OK ==>
              normfs_request_spans_in(req, end) &&
              normfs_request_counts_in(req, end) &&
              normfs_request_packets_in(packets, req->write_packets, packets_cap, end);
*/
static int
normfs_request_decode_write(const uint8_t *buf, size_t start, size_t end,
    struct normfs_request *req,
    struct normfs_request_span *packets, size_t packets_cap)
{
	size_t off = start;
	struct normfs_request_key k;
	struct normfs_request_field f;
	struct normfs_request_value v;

	/*@ loop invariant start <= off <= end;
	    loop invariant normfs_request_spans_in(req, off);
	    loop invariant normfs_request_counts_in(req, off);
	    loop invariant normfs_request_packets_in(packets, req->write_packets,
	                                             packets_cap, off);
	    loop assigns off, k, f, v, req->write_id, req->write_queue_id,
	                 req->write_packets, packets[0 .. packets_cap - 1];
	    loop variant end - off;
	*/
	while (off < end) {
		k = normfs_request_key(buf, off, end);
		if (k.status != NORMFS_REQUEST_OK) return k.status;
		if (k.field == 1u) {
			if (k.wire != NORMFS_REQUEST_WIRE_VARINT)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			v = normfs_request_varint(buf, k.next, end);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			req->write_id = v.value;
			off = v.next;
		} else if (k.field == 2u || k.field == 10u) {
			if (k.wire != NORMFS_REQUEST_WIRE_LEN)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			f = normfs_request_len(buf, k.next, end);
			if (f.status != NORMFS_REQUEST_OK) return f.status;
			if (k.field == 2u) {
				req->write_queue_id = f.span;
			} else {
				if (req->write_packets < packets_cap)
					packets[req->write_packets] = f.span;
				req->write_packets++;
			}
			off = f.next;
		} else {
			v = normfs_request_skip(buf, k.next, end, k.wire);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			off = v.next;
		}
	}
	return NORMFS_REQUEST_OK;
}

/*@ requires start <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires \valid(req);
    requires encodings_cap == 0 || \valid(encodings + (0 .. encodings_cap - 1));
    requires \separated(req, encodings + (0 .. encodings_cap - 1));
    requires normfs_request_counts_in(req, start);
    assigns req->read_encodings, encodings[0 .. encodings_cap - 1];
    ensures normfs_request_varint_status(\result);
    ensures \result == NORMFS_REQUEST_OK ==> normfs_request_counts_in(req, end);
*/
static int
normfs_request_decode_packed(const uint8_t *buf, size_t start, size_t end,
    struct normfs_request *req, uint64_t *encodings, size_t encodings_cap)
{
	size_t off = start;
	struct normfs_request_value v;

	/*@ loop invariant start <= off <= end;
	    loop invariant normfs_request_counts_in(req, off);
	    loop assigns off, v, req->read_encodings, encodings[0 .. encodings_cap - 1];
	    loop variant end - off;
	*/
	while (off < end) {
		v = normfs_request_varint(buf, off, end);
		if (v.status != NORMFS_REQUEST_OK) return v.status;
		if (req->read_encodings < encodings_cap)
			encodings[req->read_encodings] = v.value;
		req->read_encodings++;
		off = v.next;
	}
	return NORMFS_REQUEST_OK;
}

/*@ requires start <= end;
    requires end == 0 || \valid_read(buf + (0 .. end - 1));
    requires \valid(req);
    requires encodings_cap == 0 || \valid(encodings + (0 .. encodings_cap - 1));
    requires \separated(req, encodings + (0 .. encodings_cap - 1));
    requires normfs_request_spans_in(req, start);
    requires normfs_request_counts_in(req, start);
    assigns req->read_id, req->read_queue_id, req->read_step, req->read_encodings,
            req->has_read_offset, req->has_read_offset_id, req->read_offset_id_raw,
            req->read_offset_type, req->read_limit,
            encodings[0 .. encodings_cap - 1];
    ensures normfs_request_message_status(\result);
    ensures \result == NORMFS_REQUEST_OK ==>
              normfs_request_spans_in(req, end) &&
              normfs_request_counts_in(req, end);
*/
static int
normfs_request_decode_read(const uint8_t *buf, size_t start, size_t end,
    struct normfs_request *req, uint64_t *encodings, size_t encodings_cap)
{
	size_t off = start;
	struct normfs_request_key k;
	struct normfs_request_field f;
	struct normfs_request_value v;
	int status;

	/*@ loop invariant start <= off <= end;
	    loop invariant normfs_request_spans_in(req, off);
	    loop invariant normfs_request_counts_in(req, off);
	    loop assigns off, k, f, v, status, req->read_id, req->read_queue_id,
	                 req->read_step, req->read_encodings, req->has_read_offset,
	                 req->has_read_offset_id, req->read_offset_id_raw,
	                 req->read_offset_type, req->read_limit,
	                 encodings[0 .. encodings_cap - 1];
	    loop variant end - off;
	*/
	while (off < end) {
		k = normfs_request_key(buf, off, end);
		if (k.status != NORMFS_REQUEST_OK) return k.status;
		if (k.field == 1u || k.field == 3u || k.field == 11u) {
			if (k.wire != NORMFS_REQUEST_WIRE_VARINT)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			v = normfs_request_varint(buf, k.next, end);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			if (k.field == 1u)
				req->read_id = v.value;
			else if (k.field == 3u)
				req->read_step = v.value;
			else
				req->read_limit = v.value;
			off = v.next;
		} else if (k.field == 4u && k.wire == NORMFS_REQUEST_WIRE_VARINT) {
			v = normfs_request_varint(buf, k.next, end);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			if (req->read_encodings < encodings_cap)
				encodings[req->read_encodings] = v.value;
			req->read_encodings++;
			off = v.next;
		} else if (k.field == 2u || k.field == 4u || k.field == 10u) {
			if (k.wire != NORMFS_REQUEST_WIRE_LEN)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			f = normfs_request_len(buf, k.next, end);
			if (f.status != NORMFS_REQUEST_OK) return f.status;
			if (k.field == 2u) {
				req->read_queue_id = f.span;
			} else if (k.field == 4u) {
				status = normfs_request_decode_packed(buf, f.span.off,
				    f.span.off + f.span.len, req, encodings, encodings_cap);
				if (status != NORMFS_REQUEST_OK) return status;
			} else {
				status = normfs_request_decode_offset(buf, f.span.off,
				    f.span.off + f.span.len, req);
				if (status != NORMFS_REQUEST_OK) return status;
				req->has_read_offset = 1;
			}
			off = f.next;
		} else {
			v = normfs_request_skip(buf, k.next, end, k.wire);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			off = v.next;
		}
	}
	return NORMFS_REQUEST_OK;
}

/*@ requires len == 0 || \valid_read(buf + (0 .. len - 1));
    requires \valid(req);
    requires packets_cap == 0 || \valid(packets + (0 .. packets_cap - 1));
    requires encodings_cap == 0 || \valid(encodings + (0 .. encodings_cap - 1));
    requires \separated(req, packets + (0 .. packets_cap - 1),
                        encodings + (0 .. encodings_cap - 1));
    assigns *req, packets[0 .. packets_cap - 1], encodings[0 .. encodings_cap - 1];
    ensures normfs_request_message_status(\result) ||
            \result == NORMFS_REQUEST_ERR_NO_SPACE;
    ensures \result == NORMFS_REQUEST_OK || \result == NORMFS_REQUEST_ERR_NO_SPACE ==>
              normfs_request_spans_in(req, len) &&
              normfs_request_counts_in(req, len) &&
              normfs_request_packets_in(packets, req->write_packets, packets_cap, len);
    ensures \result == NORMFS_REQUEST_OK ==>
              req->write_packets <= packets_cap &&
              req->read_encodings <= encodings_cap;
    ensures \result == NORMFS_REQUEST_ERR_NO_SPACE ==>
              req->write_packets > packets_cap ||
              req->read_encodings > encodings_cap;
*/
int
normfs_request_decode(const uint8_t *buf, size_t len,
    struct normfs_request *req,
    struct normfs_request_span *packets, size_t packets_cap,
    uint64_t *encodings, size_t encodings_cap)
{
	size_t off = 0u;
	struct normfs_request_key k;
	struct normfs_request_field f;
	struct normfs_request_value v;
	int status;

	req->setup_version = 0u;
	req->ping_sequence = 0u;
	req->ping_client_timestamp_ns = 0u;
	req->write_id = 0u;
	req->write_queue_id.off = 0u;
	req->write_queue_id.len = 0u;
	req->write_packets = 0u;
	req->read_id = 0u;
	req->read_queue_id.off = 0u;
	req->read_queue_id.len = 0u;
	req->read_step = 0u;
	req->read_encodings = 0u;
	req->read_offset_id_raw.off = 0u;
	req->read_offset_id_raw.len = 0u;
	req->read_offset_type = 0u;
	req->read_limit = 0u;
	req->has_setup = 0;
	req->has_ping = 0;
	req->has_write = 0;
	req->has_read = 0;
	req->has_read_offset = 0;
	req->has_read_offset_id = 0;

	/*@ loop invariant 0 <= off <= len;
	    loop invariant normfs_request_spans_in(req, off);
	    loop invariant normfs_request_counts_in(req, off);
	    loop invariant normfs_request_packets_in(packets, req->write_packets,
	                                             packets_cap, off);
	    loop assigns off, k, f, v, status, *req,
	                 packets[0 .. packets_cap - 1], encodings[0 .. encodings_cap - 1];
	    loop variant len - off;
	*/
	while (off < len) {
		k = normfs_request_key(buf, off, len);
		if (k.status != NORMFS_REQUEST_OK) return k.status;
		if (k.field == 1u || k.field == 2u || k.field == 10u || k.field == 11u) {
			if (k.wire != NORMFS_REQUEST_WIRE_LEN)
				return NORMFS_REQUEST_ERR_BAD_WIRE_TYPE;
			f = normfs_request_len(buf, k.next, len);
			if (f.status != NORMFS_REQUEST_OK) return f.status;
			if (k.field == 1u) {
				status = normfs_request_decode_setup(buf, f.span.off,
				    f.span.off + f.span.len, req);
				req->has_setup = 1;
			} else if (k.field == 2u) {
				status = normfs_request_decode_ping(buf, f.span.off,
				    f.span.off + f.span.len, req);
				req->has_ping = 1;
			} else if (k.field == 10u) {
				status = normfs_request_decode_write(buf, f.span.off,
				    f.span.off + f.span.len, req, packets, packets_cap);
				req->has_write = 1;
			} else {
				status = normfs_request_decode_read(buf, f.span.off,
				    f.span.off + f.span.len, req, encodings, encodings_cap);
				req->has_read = 1;
			}
			if (status != NORMFS_REQUEST_OK) return status;
			off = f.next;
		} else {
			v = normfs_request_skip(buf, k.next, len, k.wire);
			if (v.status != NORMFS_REQUEST_OK) return v.status;
			off = v.next;
		}
	}

	if (req->write_packets > packets_cap || req->read_encodings > encodings_cap)
		return NORMFS_REQUEST_ERR_NO_SPACE;
	return NORMFS_REQUEST_OK;
}
