#ifndef NORMFS_REQUEST_H
#define NORMFS_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#include "uintn/varint.h"

/* Codes 0 .. 3 take their values from enum normfs_uintn_varint_status. */
enum normfs_request_status {
	NORMFS_REQUEST_OK = NORMFS_UINTN_VARINT_OK,
	NORMFS_REQUEST_ERR_TRUNCATED = NORMFS_UINTN_VARINT_ERR_TRUNCATED,
	NORMFS_REQUEST_ERR_OVERFLOW = NORMFS_UINTN_VARINT_ERR_OVERFLOW,
	NORMFS_REQUEST_ERR_NON_CANONICAL = NORMFS_UINTN_VARINT_ERR_NON_CANONICAL,
	NORMFS_REQUEST_ERR_NO_SPACE = 4,
	NORMFS_REQUEST_ERR_BAD_KEY = 5,
	NORMFS_REQUEST_ERR_BAD_WIRE_TYPE = 6,
	NORMFS_REQUEST_ERR_GROUP = 7
};

/* A byte range of the decoded buffer. */
struct normfs_request_span {
	size_t off;
	size_t len;
};

/* ClientRequest from proto/normfs.proto, with bytes and strings left in the buffer. */
struct normfs_request {
	uint64_t setup_version;
	uint64_t ping_sequence;
	uint64_t ping_client_timestamp_ns;
	uint64_t write_id;
	struct normfs_request_span write_queue_id;
	size_t write_packets;
	uint64_t read_id;
	struct normfs_request_span read_queue_id;
	uint64_t read_step;
	size_t read_encodings;
	struct normfs_request_span read_offset_id_raw;
	uint64_t read_offset_type;
	uint64_t read_limit;
	int has_setup;
	int has_ping;
	int has_write;
	int has_read;
	int has_read_offset;
	int has_read_offset_id;
};

/*
 * Decodes a ClientRequest. packets and encodings receive WriteRequest.packets
 * and ReadRequest.accept_encodings; when either has more entries than its
 * capacity the status is NO_SPACE and req still carries the full counts.
 */
int normfs_request_decode(const uint8_t *buf, size_t len,
    struct normfs_request *req,
    struct normfs_request_span *packets, size_t packets_cap,
    uint64_t *encodings, size_t encodings_cap);

#endif /* NORMFS_REQUEST_H */
