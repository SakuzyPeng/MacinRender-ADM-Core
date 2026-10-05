"""Independent libadm-authored inputs and comparison of both importers."""
import pathlib
import subprocess
import sys
import tempfile


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
        subprocess.run([reference, str(directory), *inputs], check=True)


if __name__ == '__main__':
    main()
