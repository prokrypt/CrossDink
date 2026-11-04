#!/usr/bin/env python3
"""Publication into a branch checkout: previous checksums carry over, snapshots never change."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('publisher', ROOT / 'scripts/publish_language_assets.py')
publisher = importlib.util.module_from_spec(spec)
spec.loader.exec_module(publisher)


def bundle(revision='c', text=b'French'):
    prefix = f'snapshots/{revision * 40}/'
    notice = b'Notices'
    ref = {'path': prefix + 'source/notice.txt', 'bytes': len(notice), 'sha256': publisher.sha(notice)}
    entry = {'code': 'FR', 'name': 'French', 'bytes': len(text), 'sha256': publisher.sha(text)}
    catalog = {'schema': 1, 'kind': 'translation', 'channel': 'stable', 'revision': revision * 40, 'format': 1,
               'requires': 0, 'notices': 'notices.json', 'sources': 'sources.json', 'source': 'a' * 64,
               'keyset': 'b' * 64, 'entries': [entry]}
    files = {ref['path']: notice, prefix + 'translation/' + entry['sha256'] + '.yaml': text}
    for name in ('notices.json', 'sources.json'):
        files[prefix + name] = publisher.encode({'translation': {'FR': {'files': [ref]}}})
    return catalog, files


class PublicationTest(unittest.TestCase):
    def test_publish_twice(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            key = publisher.publish(*bundle(), output)
            self.assertEqual(key, f"catalogs/stable/translation/{'a' * 64}/{'b' * 64}.json")
            first = json.loads((output / key).read_bytes())
            self.assertNotIn('previous', first['entries'][0])
            # A new snapshot with changed text records the old checksum as previous.
            publisher.publish(*bundle('d', b'Francais'), output)
            second = json.loads((output / key).read_bytes())
            self.assertEqual(second['entries'][0]['previous'], publisher.sha(b'French'))
            self.assertTrue((output / f"snapshots/{'c' * 40}/notices.json").exists())
            # Republishing the same snapshot keeps previous; a changed immutable file is refused.
            publisher.publish(*bundle('d', b'Francais'), output)
            self.assertEqual(json.loads((output / key).read_bytes()), second)
            catalog, files = bundle('d', b'Francais')
            files[f"snapshots/{'d' * 40}/translation/{publisher.sha(b'Francais')}.yaml"] = b'tampered'
            with self.assertRaises(RuntimeError):
                publisher.publish(catalog, files, output)

    def test_real_sources_build(self):
        catalog, files = publisher.build_languages('1.6.0', '0' * 40, 'stable')
        publisher.validate_companions(catalog, files)
        self.assertGreater(len(catalog['entries']), 20)
        self.assertNotIn('EN', {entry['code'] for entry in catalog['entries']})
        self.assertLessEqual(len(publisher.encode(catalog)), 8192)


if __name__ == '__main__':
    unittest.main()
