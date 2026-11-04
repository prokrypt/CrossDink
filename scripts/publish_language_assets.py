#!/usr/bin/env python3
"""Build the language download catalog and snapshot for the language-assets branch.

Writes catalogs/<channel>/translation/<source>/<keyset>.json plus an immutable
snapshots/<revision>/ tree into --output (the checkout of the language-assets
branch). Snapshot files are never rewritten; a differing file is an error.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'scripts'))


def encode(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':')) + '\n').encode()


def sha(data):
    return hashlib.sha256(data).hexdigest()


def validate_companions(catalog, files):
    base = 'snapshots/' + catalog['revision'] + '/'
    for entry in catalog['entries']:
        selected = []
        for index in ('notices.json', 'sources.json'):
            data = files[base + index]
            if len(data) > 2097152:
                raise RuntimeError('companion index exceeds device limit')
            refs = json.loads(data)[catalog['kind']][entry['code']]['files']
            if not 0 < len(refs) <= 16:
                raise RuntimeError('invalid companion reference count')
            selected.extend(refs)
            for ref in refs:
                path = ref['path']
                if (not path.startswith(base + 'source/') or len(path.encode('ascii')) > 240
                        or any(part in ('', '.', '..') or part.endswith(('.', ' ')) for part in path.split('/'))):
                    raise RuntimeError('invalid companion path: ' + path)
                body = files[path]
                if not 0 < len(body) <= 2097152 or len(body) != ref['bytes'] or sha(body) != ref['sha256']:
                    raise RuntimeError('invalid companion file: ' + path)
        if sum(ref['bytes'] for ref in selected) > 8388608:
            raise RuntimeError('companion closure exceeds device limit')


def build_languages(version, commit, channel):
    from gen_i18n import parse_yaml_file
    from generate_language_template import generate
    artifacts = generate(version, commit, 'release-build')
    template = json.loads(artifacts['language-template.json'])
    sources = {str(p.relative_to(root)): p.read_bytes() for p in sorted((root / 'lib/I18n/translations').glob('*.yaml'))}
    for name in ('LICENSE', 'lib/I18n/languages.json', 'scripts/gen_i18n.py', 'scripts/generate_language_template.py'):
        sources[name] = (root / name).read_bytes()
    identity = encode({name: sha(data) for name, data in sorted({**sources, **artifacts}.items())})
    # Content identity: the same sources always land in the same snapshot.
    revision = hashlib.sha256(b'crossdink-languages-v1\n' + identity).hexdigest()[:40]
    base = f'snapshots/{revision}'
    files = {}

    def put(name, data):
        key = base + '/' + name
        files[key] = data
        return {'path': key, 'bytes': len(data), 'sha256': sha(data)}

    refs = {name: put('source/firmware/' + name, data) for name, data in sources.items()}
    for name, data in artifacts.items():
        put('editor/' + name, data)
    notices, companions, entries = {}, {}, []
    common = [refs[name] for name in ('scripts/gen_i18n.py', 'scripts/generate_language_template.py',
                                     'lib/I18n/languages.json', 'lib/I18n/translations/english.yaml')]
    originals = {parse_yaml_file(str(root / name)).get('_language_code'): name
                 for name in sources if name.endswith('.yaml')}
    for item in template['languages']:
        if 'translation' not in item:
            continue
        ref = item['translation']
        data = artifacts[ref['filename']]
        if sha(data) != ref['sha256']:
            raise RuntimeError('translation checksum mismatch')
        put('translation/' + ref['sha256'] + '.yaml', data)
        code = item['code']
        entries.append({'code': code, 'name': item['name'], 'bytes': len(data), 'sha256': sha(data)})
        notices[code] = {'license': 'MIT', 'files': [refs['LICENSE']]}
        companions[code] = {'revision': commit, 'provenance': 'published-release',
                            'files': [refs[originals[code]], *common]}
    put('notices.json', encode({'translation': notices}))
    put('sources.json', encode({'translation': companions}))
    put('snapshot.json', encode({'revision': revision, 'sourceCommit': commit, 'firmwareVersion': version,
                                'files': {k: sha(v) for k, v in files.items()}}))
    catalog = {'schema': 1, 'kind': 'translation', 'channel': channel, 'revision': revision,
               'format': 1, 'requires': 0, 'notices': 'notices.json', 'sources': 'sources.json',
               'source': template['englishSourceSha256'], 'keyset': template['keysetSha256'],
               'entries': sorted(entries, key=lambda e: e['code'])}
    return catalog, files


def catalog_key(catalog):
    if (catalog.get('kind') != 'translation' or catalog.get('channel') not in ('stable', 'rc')
            or catalog.get('schema') != 1 or catalog.get('format') != 1 or catalog.get('requires') != 0
            or not re.fullmatch(r'[0-9a-f]{40}', catalog.get('revision', ''))
            or any(not re.fullmatch(r'[0-9a-f]{64}', catalog.get(k, '')) for k in ('source', 'keyset'))):
        raise RuntimeError('invalid catalog identity')
    return f"catalogs/{catalog['channel']}/translation/{catalog['source']}/{catalog['keyset']}.json"


def previous_entries(catalog, previous):
    """Carry the last checksum per code so devices on it still see Update."""
    if previous is None:
        return
    if catalog_key(previous) != catalog_key(catalog):
        raise RuntimeError('existing catalog has a different identity')
    prior = {entry['code']: entry for entry in previous['entries']}
    for entry in catalog['entries']:
        old = prior.get(entry['code'], {})
        value = old.get('sha256') if old.get('sha256') != entry['sha256'] else old.get('previous')
        if value:
            if not re.fullmatch(r'[0-9a-f]{64}', value) or value == entry['sha256']:
                raise RuntimeError('invalid previous checksum')
            entry['previous'] = value


def publish(catalog, files, output):
    """Write the snapshot, then the catalog, into output. Returns the catalog key."""
    validate_companions(catalog, files)
    key = catalog_key(catalog)
    existing = output / key
    previous = json.loads(existing.read_bytes()) if existing.exists() else None
    if previous is not None and not isinstance(previous, dict):
        raise RuntimeError('invalid existing catalog: ' + key)
    previous_entries(catalog, previous)
    data = encode(catalog)
    if not 0 < len(catalog['entries']) <= 48 or len(data) > 8192:
        raise RuntimeError('catalog exceeds device limits')
    for name, body in files.items():
        dest = output / name
        if dest.exists() and dest.read_bytes() != body:
            raise RuntimeError('immutable snapshot collision: ' + name)
    for name, body in files.items():
        dest = output / name
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(body)
    existing.parent.mkdir(parents=True, exist_ok=True)
    existing.write_bytes(data)
    return key


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path, help='checkout of the language-assets branch')
    parser.add_argument('--channel', choices=['stable', 'rc'], default='stable')
    args = parser.parse_args()
    commit = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    version = re.search(r'^version\s*=\s*(\S+)', (root / 'platformio.ini').read_text(), re.M).group(1)
    catalog, files = build_languages(version, commit, args.channel)
    key = publish(catalog, files, args.output)
    print(f"Published {len(catalog['entries'])} languages for firmware {version} to {args.output / key}")


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f'Error: {exc}')
