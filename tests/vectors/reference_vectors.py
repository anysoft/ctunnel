#!/usr/bin/env python3
"""Independent standard-library encoder for ctunnel v3 regression vectors."""

import hashlib
import json
import pathlib
import sys


def kdf(material: bytes, salt: bytes, context: bytes, length: int) -> bytes:
    prk = hashlib.blake2b(material, digest_size=64, key=salt).digest()
    output = bytearray()
    counter = 0
    while len(output) < length:
        output += hashlib.blake2b(
            counter.to_bytes(8, "little") + context, digest_size=64, key=prk
        ).digest()
        counter += 1
    return bytes(output[:length])


material = b"\x01" + bytes(31)
salt = b"\x02" + bytes(31)
context = b"ctunnel-v3/control-c2s-key"
session = 0x1122334455667788
client = b"alice"
work_id = bytes(range(32, 64))
work_input = (
    b"ctunnel-v3/work-bind/client-to-server"
    + session.to_bytes(8, "big")
    + len(client).to_bytes(2, "big")
    + client
    + bytes([21])
    + work_id
)
stream_random = bytes(range(64, 96))
service = b"ssh"
stream_input = (
    b"ctunnel-v3/stream-binding"
    + session.to_bytes(8, "big")
    + len(client).to_bytes(2, "big")
    + client
    + work_id
    + len(service).to_bytes(2, "big")
    + service
    + (42).to_bytes(8, "big")
    + stream_random
    + bytes([0, 22, 0, 1])
)
client_hello = (
    len(client).to_bytes(2, "big")
    + client
    + bytes(range(32))
    + bytes(range(32, 64))
    + (2).to_bytes(4, "big")
)
transcript = (
    b"ctunnel-handshake-v3"
    + b"CTUN"
    + bytes([3])
    + client_hello
    + bytes(range(64, 96))
    + bytes(range(96, 128))
    + bytes([1])
    + hashlib.blake2b(b"C" * 32, digest_size=32).digest()
    + hashlib.blake2b(b"S" * 32, digest_size=32).digest()
)

vectors = {
    "frame_header": "4354554e031e01240000007b112233445566778800000000000000040000000000000009",
    "nonce": (bytes(range(16)) + (1).to_bytes(8, "big")).hex(),
    "handshake_transcript": transcript.hex(),
    "handshake_transcript_hash": hashlib.blake2b(transcript, digest_size=32).hexdigest(),
    "kdf_control_c2s_key": kdf(material, salt, context, 32).hex(),
    "work_auth_input": work_input.hex(),
    "work_auth_mac": hashlib.blake2b(work_input, digest_size=32, key=bytes(range(32))).hexdigest(),
    "stream_auth_input": stream_input.hex(),
    "stream_auth_mac": hashlib.blake2b(
        stream_input, digest_size=32, key=bytes(range(32))
    ).hexdigest(),
}

if len(sys.argv) == 3 and sys.argv[1] == "--check":
    recorded = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8"))
    if recorded != vectors:
        raise SystemExit("recorded protocol vectors do not match independent encoder")
else:
    print(json.dumps(vectors, indent=2, sort_keys=True))
