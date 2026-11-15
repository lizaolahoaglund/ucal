"""Verify the public export refuses leaked private implementations and history."""
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('ucal_export', ROOT / 'tools/packaging/Export-UcalSource.py')
exporter = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exporter)


class SourceExportTest(unittest.TestCase):
    def setUp(self):
        (ROOT / 'out').mkdir(exist_ok=True)
        self.fixture = tempfile.TemporaryDirectory(prefix='source-export-contract-', dir=ROOT / 'out')
        self.root = Path(self.fixture.name)
        self.git('init', '-q', '-b', 'main')
        self.write('README.md', 'Original public fixture.\n')
        self.write('frontends/example/private/Fixture.cpp', '// Original private fixture.\n')

    def tearDown(self):
        self.fixture.cleanup()

    def git(self, *arguments):
        return subprocess.check_output(['git', '-C', str(self.root), *arguments], stderr=subprocess.STDOUT)

    def write(self, relative, text):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def commit(self, visibility='public'):
        self.write('frontends/example/plugin-package.json', json.dumps({
            'visibility': visibility, 'private_sources': ['private/**']}))
        self.git('add', '.')
        self.git('-c', 'user.name=UCAL fixture', '-c', 'user.email=fixture@example.invalid',
                 '-c', 'commit.gpgsign=false', 'commit', '-qm', 'Original export fixture')

    def testPublicExportOmitsPrivateSourceAndHistory(self):
        self.write('.gitattributes', '/frontends/example/private/** export-ignore\n')
        self.commit()
        output = self.root / 'out/public'
        result = exporter.export(self.root, output)
        self.assertFalse(result['gitHistoryIncluded'])
        self.assertTrue((output / 'README.md').is_file())
        self.assertFalse((output / '.git').exists())
        self.assertFalse((output / 'frontends/example/private/Fixture.cpp').exists())

    def testMissingAttributeRefusesPrivateImplementation(self):
        self.commit()
        with self.assertRaisesRegex(ValueError, 'Private compatibility implementation'):
            exporter.export(self.root, self.root / 'out/public')

    def testMissingAttributeRefusesWholePrivateModule(self):
        self.commit('private')
        with self.assertRaisesRegex(ValueError, 'Private source group'):
            exporter.export(self.root, self.root / 'out/public')

    def testNestedSourceCannotBorrowParentRepository(self):
        self.commit()
        nested = self.root / 'nested'
        nested.mkdir()
        with self.assertRaisesRegex(ValueError, 'actual repository root'):
            exporter.export(nested, nested / 'out/public')

    def testCommittedPrivatePolicyBlocksTextBinaryAndNames(self):
        self.write('.private/public-source-policy.json', json.dumps({
            'schema': 'org.ucal.public-source-policy/1', 'blocked_terms': ['private-vendor'],
            'blocked_patterns': [r'PRIVATE_DEVICE_\d+']}))
        self.write('.gitattributes', '/.private export-ignore\n/frontends/example/private/** export-ignore\n')
        self.commit()
        for name, payload in [('README.md', b'PRIVATE-VENDOR'),
                              ('sample.bin', 'private-vendor'.encode('utf-16-le')),
                              ('private-vendor.txt', b'example'),
                              ('example.txt', b'PRIVATE_DEVICE_123')]:
            with self.subTest(name=name, payload=payload):
                path = self.root / name
                previous = path.read_bytes() if path.exists() else None
                path.write_bytes(payload)
                self.commit()
                with self.assertRaisesRegex(ValueError, 'Forbidden public source content'):
                    exporter.export(self.root, self.root / 'out/public')
                self.assertFalse((self.root / 'out/public').exists())
                if previous is None: path.unlink()
                else: path.write_bytes(previous)
                self.git('add', '-A')
                self.commit()
        result = exporter.export(self.root, self.root / 'out/public')
        self.assertFalse(result['gitHistoryIncluded'])
        self.assertFalse((self.root / 'out/public/.private').exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
