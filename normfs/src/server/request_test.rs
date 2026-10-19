use bytes::Bytes;
use prost::Message;

use super::request::{decode_request, RequestError};
use crate::proto::{
    ClientRequest, Id, Offset, PingRequest, ReadRequest, SetupRequest, WriteRequest,
};

fn write(id: u64, queue: &str, packets: usize) -> WriteRequest {
    WriteRequest {
        write_id: id,
        queue_id: queue.to_string(),
        packets: (0..packets)
            .map(|i| Bytes::from(vec![i as u8; i % 300]))
            .collect(),
    }
}

fn read(id: u64, queue: &str, encodings: usize, raw: Option<Vec<u8>>) -> ReadRequest {
    ReadRequest {
        read_id: id,
        queue_id: queue.to_string(),
        step: id % 3,
        accept_encodings: (0..encodings).map(|i| (i % 3) as i32 - 1).collect(),
        offset: raw.map(|raw| Offset {
            id: Some(Id { raw: raw.into() }),
            r#type: 1,
        }),
        limit: id.wrapping_mul(0x9e37_79b9_7f4a_7c15),
    }
}

fn requests() -> Vec<ClientRequest> {
    vec![
        ClientRequest::default(),
        ClientRequest {
            setup: Some(SetupRequest { version: 1 }),
            ..Default::default()
        },
        ClientRequest {
            ping: Some(PingRequest {
                sequence: u64::MAX,
                client_timestamp_ns: 1_760_000_000_000_000_000,
            }),
            ..Default::default()
        },
        ClientRequest {
            write: Some(write(7, "robot/камера", 3)),
            ..Default::default()
        },
        ClientRequest {
            write: Some(write(8, "q", 1000)),
            ..Default::default()
        },
        ClientRequest {
            read: Some(read(9, "q", 0, None)),
            ..Default::default()
        },
        ClientRequest {
            read: Some(read(10, "q", 50, Some(vec![1, 2, 3, 0xff]))),
            ..Default::default()
        },
        ClientRequest {
            read: Some(ReadRequest {
                offset: Some(Offset {
                    id: Some(Id::default()),
                    r#type: 0,
                }),
                ..Default::default()
            }),
            ..Default::default()
        },
        ClientRequest {
            setup: Some(SetupRequest { version: 2 }),
            ping: Some(PingRequest::default()),
            write: Some(write(u64::MAX, "", 65)),
            read: Some(read(1 << 40, "x", 9, Some(vec![0; 16]))),
        },
    ]
}

fn assert_same(buf: &[u8]) {
    let expected = ClientRequest::decode(buf).expect("prost accepts the buffer");
    assert_eq!(decode_request(buf).unwrap(), expected);
}

#[test]
fn test_matches_prost() {
    for request in requests() {
        assert_same(&request.encode_to_vec());
    }
}

#[test]
fn test_repeated_message_fields_merge_like_prost() {
    let all = requests();
    for a in &all {
        for b in &all {
            let mut buf = a.encode_to_vec();
            buf.extend_from_slice(&b.encode_to_vec());
            assert_same(&buf);
        }
    }
}

#[test]
fn test_unknown_fields_are_skipped() {
    let mut buf = requests().last().unwrap().encode_to_vec();
    // field 99 varint, field 98 fixed64, field 97 bytes, field 96 fixed32
    buf.extend_from_slice(&[0x98, 0x06, 0x96, 0x01]);
    buf.extend_from_slice(&[0x91, 0x06, 1, 2, 3, 4, 5, 6, 7, 8]);
    buf.extend_from_slice(&[0x8a, 0x06, 2, 0xaa, 0xbb]);
    buf.extend_from_slice(&[0x85, 0x06, 1, 2, 3, 4]);
    assert_same(&buf);
}

#[test]
fn test_unpacked_encodings_match_prost() {
    // ReadRequest { accept_encodings: [1, 0, 1] } written unpacked, field 4 wire type 0
    let read = [0x20, 0x01, 0x20, 0x00, 0x20, 0x01];
    let mut buf = vec![0x5a, read.len() as u8];
    buf.extend_from_slice(&read);
    assert_same(&buf);
}

#[test]
fn test_rejects_what_prost_rejects() {
    let full = requests().last().unwrap().encode_to_vec();
    for cut in 1..full.len() {
        match ClientRequest::decode(&full[..cut]) {
            Ok(expected) => assert_eq!(decode_request(&full[..cut]), Ok(expected), "cut at {cut}"),
            Err(_) => assert!(decode_request(&full[..cut]).is_err(), "cut at {cut}"),
        }
    }
    // field 0
    assert_eq!(decode_request(&[0x00, 0x00]), Err(RequestError::BadKey));
    // ping sent as a varint
    assert_eq!(
        decode_request(&[0x10, 0x01]),
        Err(RequestError::BadWireType)
    );
    // write queue id that is not UTF-8
    assert_eq!(
        decode_request(&[0x52, 0x03, 0x12, 0x01, 0xff]),
        Err(RequestError::InvalidUtf8)
    );
}

#[test]
fn test_stricter_than_prost() {
    // setup { version: 1 } with the version as a two byte varint
    let non_canonical = [0x0a, 0x03, 0x08, 0x81, 0x00];
    assert!(ClientRequest::decode(&non_canonical[..]).is_ok());
    assert_eq!(
        decode_request(&non_canonical),
        Err(RequestError::NonCanonical)
    );

    // an empty group, field 15
    let group = [0x7b, 0x7c];
    assert!(ClientRequest::decode(&group[..]).is_ok());
    assert_eq!(decode_request(&group), Err(RequestError::Group));
}
