"""Hardware-free notice collector contracts; run with python -m unittest discover."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('notices', Path(__file__).with_name('Collect-UcalRuntimeNotices.py'))
notices = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notices)


class RuntimeNoticesTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source, self.kit, self.mingw, self.ms, self.output = [self.root / name for name in ('src', 'kit', 'mingw', 'ms', 'out')]
        self.base = self.source / 'qtbase'
        self.write(self.base / '.qmake.conf', 'MODULE_VERSION = 5.14.2')
        self.write(self.kit / 'mkspecs/qconfig.pri', '''QT_VERSION = 5.14.2
QT_ARCH = x86_64
QT_EDITION = OpenSource
QT_GCC_MAJOR_VERSION = 7
QT_GCC_MINOR_VERSION = 3
CONFIG += shared''')
        self.write(self.kit / 'mkspecs/qmodule.pri', 'QT.global_private.disabled_features = system-zlib')
        self.write(self.kit / 'include/QtCore/qtcore-config.h', '#define QT_FEATURE_big_codecs 1\n#define QT_FEATURE_topleveldomain 1')
        self.write(self.kit / 'include/QtGui/qtgui-config.h', '\n'.join('#define QT_FEATURE_' + name + ' 1' for name in ('angle', 'vulkan', 'textmarkdownreader', 'accessibility', 'pdf')))
        for module, enabled, disabled in (
                ('core', 'doubleconversion sha3-fast', 'system-pcre2 system-doubleconversion'),
                ('gui', 'freetype harfbuzz jpeg png vkgen', 'system-freetype system-harfbuzz system-jpeg system-png')):
            self.write(self.kit / f'mkspecs/modules/qt_lib_{module}_private.pri',
                       f'QT.{module}.enabled_features = {enabled}\nQT.{module}.disabled_features = {disabled}')
        self.records = [{'Id': i, 'Name': i, 'License': 'Fixture permissive terms',
                         'Copyright': 'Copyright fixture author', 'LicenseFile': 'LICENSE'}
                        for i in sorted(notices.QT_COMPONENTS)]
        self.metadata = self.base / 'src/3rdparty/fixture/qt_attribution.json'
        self.write(self.metadata, json.dumps(self.records))
        self.write(self.metadata.parent / 'LICENSE', 'Shared fixture permission and disclaimer')
        self.write(self.metadata.parent / 'src/README.ijg', '\nLEGAL ISSUES\n============\nDistinct IJG fixture terms\nREFERENCES\n==========\nUnrelated JPEG documentation')
        self.write(self.base / 'src/3rdparty/freetype/docs/FTL.TXT', 'FreeType fixture terms')
        for relative in ('gcc/COPYING.RUNTIME', 'mingw-w64/COPYING.MinGW-w64-runtime.txt', 'winpthreads/COPYING'):
            self.write(self.mingw / relative, 'Fixture terms: ' + relative)
        self.write(self.ms / 'Microsoft-Runtime.txt', 'MICROSOFT SOFTWARE LICENSE TERMS\nD3Dcompiler_47.dll\nFixture runtime terms')

    def write(self, path, text):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')

    def collect(self):
        return notices.collect(self.source, self.kit, self.mingw, self.ms, self.output)

    def test_readable_complete_notices_and_external_receipt(self):
        self.collect()
        self.assertEqual({p.name for p in self.output.iterdir()}, {'ThirdPartyNotices.txt', 'RuntimeNotices.json'})
        text = (self.output / 'ThirdPartyNotices.txt').read_text(encoding='utf-8')
        self.assertIn('Distinct IJG fixture terms', text)
        self.assertNotIn('Unrelated JPEG documentation', text)
        self.assertIn('FreeType fixture terms', text)
        self.assertEqual(text.count('Shared fixture permission and disclaimer'), 1)
        self.assertIn('Copyright fixture author', text)
        receipt = json.loads((self.output / 'RuntimeNotices.json').read_text())
        self.assertEqual(receipt['files'][0]['sha256'], hashlib.sha256((self.output / 'ThirdPartyNotices.txt').read_bytes()).hexdigest())
        self.assertEqual(set(receipt['qtComponents']), notices.QT_COMPONENTS)

    def test_unrelated_platform_and_developer_material_is_not_collected(self):
        self.write(self.base / 'src/android/qt_attribution.json', json.dumps({'Id': 'android-gradle-wrapper', 'LicenseFile': 'missing-unshipped-license'}))
        self.write(self.mingw / 'mingw-w64/ddk-readme.txt', 'DO NOT SHIP DDK')
        self.write(self.ms / 'IDE.html', 'DO NOT SHIP IDE')
        self.collect()
        text = (self.output / 'ThirdPartyNotices.txt').read_text(encoding='utf-8')
        self.assertNotIn('DO NOT SHIP', text)
        self.assertNotIn('android-gradle-wrapper', text)

    def test_missing_selected_attribution_is_rejected(self):
        self.write(self.metadata, json.dumps(self.records[1:]))
        with self.assertRaisesRegex(ValueError, 'Missing selected Qt attribution'):
            self.collect()

    def test_missing_referenced_license_is_rejected(self):
        (self.metadata.parent / 'src/README.ijg').unlink()
        with self.assertRaises(FileNotFoundError):
            self.collect()

    def test_changed_qt_feature_selection_requires_profile_review(self):
        path = self.kit / 'mkspecs/modules/qt_lib_gui_private.pri'
        self.write(path, path.read_text().replace('freetype harfbuzz', 'harfbuzz'))
        with self.assertRaisesRegex(ValueError, 'feature selection'):
            self.collect()

    def test_existing_output_is_preserved(self):
        self.write(self.output / 'keep.txt', 'preserve')
        with self.assertRaisesRegex(ValueError, 'new or empty'):
            self.collect()
        self.assertEqual((self.output / 'keep.txt').read_text(), 'preserve')


if __name__ == '__main__':
    unittest.main()
