"""Independent libadm-authored inputs and comparison of both importers."""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def write_xml_fixture(path, xml):
    """Wrap authored XML in a tiny PCM WAVE without passing it through either parser."""
    uids = ET.fromstring(xml).findall('.//audioTrackUID')
    channels = len(uids)
    chna = struct.pack('<HH', channels, channels)
    for index, uid in enumerate(uids, 1):
        chna += struct.pack('<H12s14s11sx', index, uid.attrib['UID'].encode(),
                            uid.findtext('audioTrackFormatIDRef').encode(),
                            uid.findtext('audioPackFormatIDRef').encode())

    def chunk(name, data):
        return name + struct.pack('<I', len(data)) + data + b'\0' * (len(data) % 2)

    data = b'WAVE'
    data += chunk(b'fmt ', struct.pack('<HHIIHH', 1, channels, 48000,
                                      48000 * channels * 2, channels * 2, 16))
    data += chunk(b'axml', xml.encode())
    data += chunk(b'chna', chna)
    data += chunk(b'data', b'\0' * (8 * channels * 2))
    path.write_bytes(b'RIFF' + struct.pack('<I', len(data)) + data)


def compatibility_inputs(directory):
    fixtures = pathlib.Path(__file__).resolve().parents[2] / 'fixtures' / 'adm'
    inputs = {path.stem: path.read_text(encoding='utf-8') for path in sorted(fixtures.glob('*.xml'))}
    hoa = inputs['hoa-pack-attributes']
    inputs['hoa-stream-only'] = re.sub(
        r'<audioStreamFormatIDRef>[^<]+</audioStreamFormatIDRef>', '', hoa)
    inputs['hoa-block-overrides'] = hoa.replace(
        '<order>0</order>', '<order>0</order><normalization>SN3D</normalization>'
        '<nfcRefDist>0</nfcRefDist><screenRef>0</screenRef>')
    paths = []
    for name, xml in inputs.items():
        path = directory / (name + '.wav')
        write_xml_fixture(path, xml)
        paths.append(str(path))
    return paths


def main():
    generator, reference = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='mradm-libadm-reference-') as name:
        directory = pathlib.Path(name)
        inputs = []
        for kind in ('objects-point', 'objects-extent', 'objects-extent-multi',
                     'objects-cartesian', 'directspeakers', 'hoa'):
            path = directory / (kind + '.wav')
            subprocess.run([generator, kind, str(path)], check=True)
            inputs.append(str(path))
        inputs.extend(compatibility_inputs(directory))
        subprocess.run([reference, str(directory), *inputs], check=True)


if __name__ == '__main__':
    main()
