#!/usr/bin/env python3
"""Create small, matched-PCM Logic ADM fixtures; never modify the source file."""

import argparse
import hashlib
import json
import struct
from pathlib import Path
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='Small synthetic RF64 Logic-compatible ADM probe')
    parser.add_argument('directory', type=Path, help='New directory for isolated generated fixtures')
    args = parser.parse_args()
    if args.input.stat().st_size > 64 * 1024 * 1024:
        parser.error('This fixture generator is restricted to synthetic inputs smaller than 64 MiB')
    raw = args.input.read_bytes()
    if raw[:4] != b'RF64' or raw[8:12] != b'WAVE':
        parser.error('Expected RF64/WAVE input')
    chunks, offset, data_size = [], 12, None
    while offset < len(raw):
        tag, count = struct.unpack_from('<4sI', raw, offset)
        actual = data_size if tag == b'data' and count == 0xffffffff else count
        if actual is None or offset + 8 + actual > len(raw):
            raise ValueError('Invalid RF64 chunk')
        payload = raw[offset + 8:offset + 8 + actual]
        if tag == b'ds64':
            if len(payload) != 28 or struct.unpack_from('<I', payload, 24)[0] != 0:
                raise ValueError('This probe generator expects an empty ds64 table')
            data_size = struct.unpack_from('<Q', payload, 8)[0]
        chunks.append((tag, count, payload))
        offset += 8 + actual + (actual & 1)
    if offset != len(raw) or len({tag for tag, _, _ in chunks}) != len(chunks):
        raise ValueError('Unexpected chunk structure')
    by_tag = {tag: payload for tag, _, payload in chunks}
    xml = by_tag[b'axml'].rstrip(b'\0')
    ns = ET.fromstring(xml).tag.split('}')[0].lstrip('{')
    ET.register_namespace('', ns)
    q = lambda name: '{' + ns + '}' + name
    specs = {
        'control': {},
        'diffuse-1': {'diffuse': '1'},
        'divergence-1': {'objectDivergence': '1'},
        'size04-diffuse0': {'width': '0.4', 'depth': '0.4', 'height': '0.4', 'diffuse': '0'},
        'size04-diffuse1': {'width': '0.4', 'depth': '0.4', 'height': '0.4', 'diffuse': '1'},
        'size04-divergence1': {'width': '0.4', 'depth': '0.4', 'height': '0.4',
                               'diffuse': '0', 'objectDivergence': '1'},
    }
    args.directory.mkdir(parents=True, exist_ok=False)
    manifest = {'source': str(args.input.resolve()), 'source_sha256': hashlib.sha256(raw).hexdigest(),
                'pcm_sha256': hashlib.sha256(by_tag[b'data']).hexdigest(),
                'dbmd_sha256': hashlib.sha256(by_tag[b'dbmd']).hexdigest(), 'fixtures': []}
    for name, fields in specs.items():
        root = ET.fromstring(xml)
        blocks = [b for c in root.iter(q('audioChannelFormat')) if c.get('typeLabel') == '0003'
                  for b in c.findall(q('audioBlockFormat'))]
        if not blocks or any(b.findtext(q('cartesian')) != '1' for b in blocks):
            raise ValueError('Expected Cartesian object blocks')
        for block in blocks:
            for tag, value in fields.items():
                existing = block.find(q(tag))
                child = existing if existing is not None else ET.SubElement(block, q(tag))
                child.text = value
                if tag == 'objectDivergence':
                    child.set('positionRange', '1')
        replacement = ET.tostring(root, encoding='utf-8', xml_declaration=True)
        output = bytearray(raw[:12])
        ds64_offset = None
        for tag, count, payload in chunks:
            if tag == b'axml':
                payload, count = replacement, len(replacement)
            if tag == b'ds64':
                ds64_offset = len(output) + 8
            output.extend(struct.pack('<4sI', tag, count))
            output.extend(payload)
            if len(payload) & 1:
                output.append(0)
        struct.pack_into('<Q', output, ds64_offset, len(output) - 8)
        path = args.directory / (name + '.wav')
        path.write_bytes(output)
        manifest['fixtures'].append({'name': name, 'path': str(path.resolve()), 'fields': fields,
                                     'object_blocks': len(blocks),
                                     'sha256': hashlib.sha256(output).hexdigest()})
    if hashlib.sha256(args.input.read_bytes()).hexdigest() != manifest['source_sha256']:
        raise RuntimeError('Source changed during fixture generation')
    (args.directory / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
