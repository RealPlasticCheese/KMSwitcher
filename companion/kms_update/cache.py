"""Portable receiver-cache image format, shared by release packaging and tests."""
import base64
import hashlib
import struct
import zlib

DATA_OFFSET = 4096
SLOT_BYTES = 0x200000
IMAGE_MAX = 0x1e0000
OFFSETS = (0x960000, 0xb60000)
IDENTITY = struct.Struct('<48s24s32sIIII')


def encode_identity(metadata):
    git, utc = metadata['git'].encode('ascii'), metadata['utc'].encode('ascii')
    if len(git) >= 48 or len(utc) >= 24:
        raise ValueError('Firmware identity exceeds its wire capacity')
    return IDENTITY.pack(git, utc, bytes.fromhex(metadata['sha256']), metadata['size'], 2, 3, 1)


def image_blob(image, metadata, generation=1):
    validate_receiver(image, metadata)
    header = b'KMSC' + struct.pack('<III', 1, generation, DATA_OFFSET) + encode_identity(metadata)
    header += struct.pack('<I', zlib.crc32(header)) + b'DONE'
    blob = header.ljust(DATA_OFFSET, b'\xff') + image
    return blob.ljust((len(blob) + 4095) // 4096 * 4096, b'\xff')


def validate_receiver(image, metadata):
    if (metadata.get('schema') != 1 or metadata.get('role') != 'receiver' or metadata.get('chip') != 2 or
            any(metadata.get(key) != 1 for key in ('management_protocol', 'ota_schema')) or
            metadata.get('input_protocol') != 2 or not 320 <= len(image) <= IMAGE_MAX or
            image[0] != 0xe9 or image[12:14] != b'\x02\0' or image[23] != 1 or
            metadata.get('size') != len(image) or hashlib.sha256(image).hexdigest() != metadata.get('sha256') or
            hashlib.sha256(image[:-32]).digest() != image[-32:] or
            image[32:36] != b'\x32\x54\xcd\xab' or image[80:100] != b'kmswitcher_receiver\0'):
        raise ValueError('Invalid cached receiver image, project or manifest')
    encode_identity(metadata)
    return image, metadata


def bundled_receiver(metadata):
    entry = metadata.get('receiver_firmware')
    if entry is None:
        return None
    try:
        if metadata.get('role') != 'controller' or len(entry['image_base64']) > 0x280000:
            raise ValueError('Invalid bundled receiver payload')
        image = base64.b64decode(entry['image_base64'], validate=True)
        result = validate_receiver(image, entry['metadata'])
        if metadata['git'] != entry['metadata']['git']:
            raise ValueError('Bundled receiver firmware comes from another source revision')
        return result
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError('Invalid bundled receiver firmware: ' + str(error)) from error


def validate_blob(blob):
    if (len(blob) < DATA_OFFSET + 320 or len(blob) > SLOT_BYTES or len(blob) % 4096 or
            blob[:8] != b'KMSC\x01\0\0\0' or blob[12:16] != struct.pack('<I', DATA_OFFSET) or
            blob[140:144] != b'DONE' or zlib.crc32(blob[:136]) != struct.unpack_from('<I', blob, 136)[0]):
        raise ValueError('Invalid receiver cache header')
    git, utc, digest, size, chip, capabilities, schema = IDENTITY.unpack_from(blob, 16)
    if b'\0' not in git or b'\0' not in utc or chip != 2 or capabilities != 3 or schema != 1:
        raise ValueError('Invalid receiver cache identity')
    metadata = dict(schema=1, role='receiver', chip=chip, git=git.split(b'\0')[0].decode('ascii'),
                    utc=utc.split(b'\0')[0].decode('ascii'), sha256=digest.hex(), size=size,
                    management_protocol=1, input_protocol=2, ota_schema=1)
    image = blob[DATA_OFFSET:DATA_OFFSET + size]
    validate_receiver(image, metadata)
    if blob[144:DATA_OFFSET] != b'\xff' * (DATA_OFFSET - 144) or blob[DATA_OFFSET + size:] != b'\xff' * (len(blob) - DATA_OFFSET - size):
        raise ValueError('Unexpected bytes outside cached image')
    return metadata
