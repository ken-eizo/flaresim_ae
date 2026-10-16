"""Private build-only Adobe SDK transport; never prints SDK bytes or credentials.

Provision uses authenticated gh CLI encryption and stdin, not command arguments.
Restore validates the complete archive before creating a destination. Neither
operation uploads SDK files as artifacts or stores them in the Git repository.
"""
import argparse
import base64
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import stat
import subprocess
import zipfile

CHUNK = 32000
MAX_CHUNKS = 24
MAX_ARCHIVE = CHUNK * MAX_CHUNKS * 3 // 4
MAX_EXPANDED = 32 * 1024 * 1024
REQUIRED = ('Examples/Headers/AE_Effect.h', 'Examples/Util/entry.h',
            'Examples/Resources/AE_General.r')


def package_sdk(root):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for directory in ('Headers', 'Resources', 'Util'):
            for file in sorted((root / 'Examples' / directory).rglob('*')):
                if file.is_file() and file.name != '.DS_Store':
                    archive.write(file, file.relative_to(root).as_posix())
    data = stream.getvalue()
    validate_archive(data)
    return data


def validate_archive(data):
    if len(data) > MAX_ARCHIVE:
        raise ValueError('SDK subset exceeds the bounded private transport')
    archive = zipfile.ZipFile(io.BytesIO(data))
    total = 0
    names = set()
    for entry in archive.infolist():
        # zipfile normalizes backslashes on Windows. Audit the original
        # spelling, not a sanitized path that could differ between runners.
        name = entry.orig_filename
        parts = PurePosixPath(name).parts
        if (not parts or parts[0] != 'Examples' or len(parts) < 3 or
                parts[1] not in ('Headers', 'Resources', 'Util') or
                '\\' in name or ':' in name or '..' in parts or
                name.startswith('/') or name != entry.filename or name in names or
                stat.S_ISLNK(entry.external_attr >> 16) or entry.flag_bits & 1):
            raise ValueError('Unsafe or duplicate SDK archive member')
        names.add(name)
        total += entry.file_size
        if total > MAX_EXPANDED:
            raise ValueError('SDK expansion exceeds the safety bound')
    if not set(REQUIRED).issubset(names):
        raise ValueError('SDK archive is incomplete')
    if archive.testzip() is not None:
        raise ValueError('SDK archive integrity check failed')
    return archive


def restore(destination, environment):
    manifest = json.loads(environment.get('AE_SDK_MANIFEST', '{}'))
    count = manifest.get('chunks')
    if type(count) is not int or not 1 <= count <= MAX_CHUNKS:
        raise ValueError('Private SDK secrets are not provisioned')
    chunks = [environment.get(f'AE_SDK_CHUNK_{i:02}', '') for i in range(count)]
    if any(not value or len(value) > CHUNK for value in chunks):
        raise ValueError('Private SDK chunk is missing or too large')
    data = base64.b64decode(''.join(chunks), validate=True)
    if (len(data) != manifest.get('bytes') or
            hashlib.sha256(data).hexdigest() != manifest.get('sha256')):
        raise ValueError('Private SDK digest does not match')
    archive = validate_archive(data)
    if destination.exists():
        raise ValueError('SDK destination already exists; refusing to overwrite it')
    destination.mkdir(parents=True)
    archive.extractall(destination)
    return manifest


def export_dotenv(root, destination):
    # For a provider's encrypted app-variable import, never a public artifact.
    private = (Path(__file__).resolve().parents[1] / 'out/private').resolve()
    destination = destination.resolve()
    if not destination.is_relative_to(private) or destination.suffix != '.env':
        raise ValueError('SDK variable export must stay inside out/private as an .env file')
    data = package_sdk(root)
    encoded = base64.b64encode(data).decode('ascii')
    chunks = [encoded[i:i + CHUNK] for i in range(0, len(encoded), CHUNK)]
    if not 1 <= len(chunks) <= MAX_CHUNKS:
        raise ValueError('Too many private SDK chunks')
    manifest = dict(chunks=len(chunks), bytes=len(data),
                    sha256=hashlib.sha256(data).hexdigest(), sdk='25.6_61')
    destination.parent.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w', encoding='utf-8', newline='\n') as file:
        for index, value in enumerate(chunks):
            file.write(f'AE_SDK_CHUNK_{index:02}={value}\n')
        file.write('AE_SDK_MANIFEST=' + json.dumps(manifest, separators=(',', ':')) + '\n')
    return manifest


def provision(root, platform, repository, gh):
    data = package_sdk(root)
    encoded = base64.b64encode(data).decode('ascii')
    chunks = [encoded[i:i + CHUNK] for i in range(0, len(encoded), CHUNK)]
    if not 1 <= len(chunks) <= MAX_CHUNKS:
        raise ValueError('Too many private SDK chunks')
    # All values are encrypted by gh with GitHub's repository public key.
    prefix = f'AE_SDK_{platform.upper()}_'
    listed = subprocess.run([gh, 'secret', 'list', '--repo', repository, '--json', 'name'],
                            check=True, capture_output=True, text=True)
    names = {item['name'] for item in json.loads(listed.stdout)}
    wanted = {prefix + f'{i:02}' for i in range(len(chunks))} | {prefix + 'MANIFEST'}
    if len(names | wanted) > 100:
        raise ValueError('Repository secret capacity is insufficient')
    def secret(name, value):
        subprocess.run([gh, 'secret', 'set', name, '--repo', repository], input=value,
                       text=True, check=True, capture_output=True)
    for index, value in enumerate(chunks):
        secret(prefix + f'{index:02}', value)
    manifest = dict(chunks=len(chunks), bytes=len(data),
                    sha256=hashlib.sha256(data).hexdigest(), sdk='25.6_61')
    # Commit the manifest last: a partial upload cannot pass the digest check.
    secret(prefix + 'MANIFEST', json.dumps(manifest, separators=(',', ':')))
    print(f'Provisioned private {platform} SDK: {len(chunks)} encrypted chunks; no SDK artifact')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    read = commands.add_parser('restore')
    read.add_argument('--destination', required=True, type=Path)
    write = commands.add_parser('provision')
    write.add_argument('--root', required=True, type=Path)
    write.add_argument('--platform', required=True, choices=('win', 'mac'))
    write.add_argument('--repository', required=True)
    write.add_argument('--gh', default='gh')
    export = commands.add_parser('export-dotenv')
    export.add_argument('--root', required=True, type=Path)
    export.add_argument('--destination', required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.command == 'restore':
            manifest = restore(args.destination, os.environ)
            print(f"Restored private SDK {manifest['sdk']} after digest and path validation")
        elif args.command == 'provision':
            provision(args.root, args.platform, args.repository, args.gh)
        else:
            manifest = export_dotenv(args.root, args.destination)
            print(f"Prepared {manifest['chunks'] + 1} private variables in {args.destination}; import ALL as Secret, never share this file")
    except (ValueError, OSError, zipfile.BadZipFile, subprocess.CalledProcessError):
        # Never include subprocess input/output (potential secret material).
        parser.exit(1, 'Private SDK setup failed; verify integrity, authentication, and secret permissions.\n')
