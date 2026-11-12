import hashlib
from pathlib import Path
import struct
import sys

path = Path(sys.argv[1])
data = path.read_bytes()
magic, cpu, subtype, kind, count, size, flags, reserved = struct.unpack_from('<8I', data)
assert magic == 0xFEEDFACF and cpu == 0x0100000C and kind == 6, 'Expected arm64 Mach-O dylib'
position = 32
signature = None
platform = None
dependencies = []
for _ in range(count):
    command, length = struct.unpack_from('<2I', data, position)
    assert length >= 8 and position + length <= 32 + size
    if command == 0x32:
        platform, minimum, sdk = struct.unpack_from('<3I', data, position + 8)
        assert platform == 2 and minimum == 0x000F0000, 'Expected minimum iOS 15.0'
    elif command == 0xC:
        offset = struct.unpack_from('<I', data, position + 8)[0]
        dependencies.append(data[position + offset:position + length].split(b'\0')[0].decode())
    elif command == 0x1D:
        signature = struct.unpack_from('<2I', data, position + 8)
    position += length
assert platform == 2 and signature is not None
assert dependencies and all(name.startswith(('/System/Library/', '/usr/lib/')) for name in dependencies)
start, signature_size = signature
blob = data[start:start + signature_size]
magic, total, count = struct.unpack_from('>3I', blob)
assert magic == 0xFADE0CC0 and total <= len(blob), 'Missing code-signature superblob'
verified = False
for index in range(count):
    slot, offset = struct.unpack_from('>2I', blob, 12 + index * 8)
    magic, length = struct.unpack_from('>2I', blob, offset)
    if magic != 0xFADE0C02:
        continue
    directory = blob[offset:offset + length]
    _, _, version, flags, hash_offset, identifier, special, pages, limit = struct.unpack_from('>9I', directory)
    hash_size, hash_type, _, page_shift = struct.unpack_from('>4B', directory, 36)
    assert flags & 2, 'Expected ad-hoc signature'
    assert hash_type == 2 and hash_size == 32, 'Expected SHA-256 page hashes'
    assert limit == start, 'Signature does not cover complete executable'
    page_size = 1 << page_shift
    assert pages == (limit + page_size - 1) // page_size
    for page in range(pages):
        content = data[page * page_size:min((page + 1) * page_size, limit)]
        actual = hashlib.sha256(content).digest()
        expected = directory[hash_offset + page * hash_size:hash_offset + (page + 1) * hash_size]
        assert actual == expected, f'Invalid signature at page {page}'
    verified = True
assert verified, 'Missing verified code directory'
print(f'PASS: arm64, minimum iOS 15.0, {len(dependencies)} system-only dependencies, verified ad-hoc signature')
print(f'SHA256 {hashlib.sha256(data).hexdigest()}  {path.name}')
