import hashlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('source_export', Path(__file__).with_name('Export-UcalSource.py'))
source_export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source_export)


class SourceExportTest(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix='ucal-source-export-')
        self.addCleanup(self.fixture.cleanup)
        self.root = Path(self.fixture.name)
        self.git('init', '-q')
        for name, text in {
            '.gitattributes': '/backends/private/** export-ignore\n',
            'README.md': 'Public source fixture\n',
            'backends/public/plugin-package.json': '{"visibility":"public"}',
            'backends/private/plugin-package.json': '{"visibility":"private"}',
            'backends/private/implementation.cpp': 'private source fixture',
        }.items():
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, 'utf-8')
        self.commit()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], stderr=subprocess.STDOUT)

    def commit(self):
        self.git('add', '.')
        self.git('-c', 'user.name=Source Fixture', '-c', 'user.email=fixture@example.invalid',
                 'commit', '-qm', 'Source export fixture')

    def test_archive_uses_validated_files_and_excludes_private_empty_directory(self):
        with tarfile.open(fileobj=io.BytesIO(self.git('archive', '--format=tar', 'HEAD'))) as raw:
            self.assertIn('backends/private', raw.getnames())
        hashes = []
        for index in range(2):
            output = self.root / 'out' / str(index)
            archive = self.root / 'out' / (str(index) + '.tar')
            source_export.export(self.root, output, archive_output=archive)
            with tarfile.open(archive) as delivered:
                names = delivered.getnames()
                self.assertIn('README.md', names)
                self.assertFalse(any(name.startswith('backends/private') for name in names))
                self.assertTrue(all(member.isfile() for member in delivered.getmembers()))
                self.assertEqual(sorted(names), sorted(path.relative_to(output).as_posix()
                                 for path in output.rglob('*') if path.is_file()))
            hashes.append(hashlib.sha256(archive.read_bytes()).digest())
        self.assertEqual(hashes[0], hashes[1])

    def test_existing_archive_is_preserved(self):
        archive = self.root / 'out' / 'existing.tar'
        archive.parent.mkdir()
        archive.write_bytes(b'preserve existing artifact')
        with self.assertRaisesRegex(ValueError, 'already exist'):
            source_export.export(self.root, self.root / 'out' / 'tree', archive_output=archive)
        self.assertEqual(archive.read_bytes(), b'preserve existing artifact')
        self.assertFalse((archive.parent / 'tree').exists())

    def test_private_source_validation_also_blocks_archive(self):
        (self.root / '.gitattributes').write_text('', 'utf-8')
        self.commit()
        archive = self.root / 'out' / 'rejected.tar'
        with self.assertRaisesRegex(ValueError, 'Private source group'):
            source_export.export(self.root, self.root / 'out' / 'tree', archive_output=archive)
        self.assertFalse(archive.exists())

    def test_committed_publication_policy_blocks_delivery(self):
        previous = self.git('rev-parse', 'HEAD').decode().strip()
        policy = self.root / '.private/public-source-policy.json'
        policy.parent.mkdir()
        policy.write_text(json.dumps({'schema': 'org.ucal.public-source-policy/1',
                                      'blocked_terms': ['hidden-company-fixture']}), 'utf-8')
        attributes = self.root / '.gitattributes'
        attributes.write_text(attributes.read_text('utf-8') + '/.private/** export-ignore\n', 'utf-8')
        (self.root / 'README.md').write_text('hidden-company-fixture', 'utf-8')
        self.commit()
        # An uncommitted edit cannot weaken the selected revision's policy.
        policy.write_text('{}', 'utf-8')
        archive = self.root / 'out/rejected.tar'
        with self.assertRaisesRegex(ValueError, 'Forbidden public source content'):
            source_export.export(self.root, self.root / 'out/rejected', archive_output=archive)
        self.assertFalse(archive.exists())
        self.assertFalse((self.root / 'out/rejected').exists())
        source_export.export(self.root, self.root / 'out/previous', revision=previous)
        self.assertFalse((self.root / 'out/previous/.private').exists())


if __name__ == '__main__':
    unittest.main()
