"""Preload the S3 cache and attach the matching S2 payload to its OTA manifest."""
import argparse
import base64
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'companion'))
from kms_update.cache import image_blob, validate_receiver


def build(controller, receiver):
    manifest_path = controller / 'kmswitcher_controller.json'
    master = json.loads(manifest_path.read_text(encoding='utf-8'))
    metadata = json.loads((receiver / 'kmswitcher_receiver.json').read_text(encoding='utf-8'))
    image = (receiver / 'kmswitcher_receiver.bin').read_bytes()
    validate_receiver(image, metadata)
    if master['git'] != metadata['git']:
        raise ValueError('Controller and bundled receiver must come from the same source revision')
    (controller / 'receiver-cache.bin').write_bytes(image_blob(image, metadata))
    (controller / 'receiver-cache-empty.bin').write_bytes(b'\xff' * 4096)
    master['receiver_firmware'] = dict(metadata=metadata, image_base64=base64.b64encode(image).decode('ascii'))
    manifest_path.write_text(json.dumps(master, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--controller', type=Path, required=True)
    parser.add_argument('--receiver', type=Path, required=True)
    args = parser.parse_args()
    build(args.controller, args.receiver)
