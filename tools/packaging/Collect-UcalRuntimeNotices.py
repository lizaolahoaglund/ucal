"""Generate readable notices for UCAL's maintained Windows runtime profile.

RuntimeNotices.json is a build receipt, never an end-user package asset.
The profile covers compiled code, including entry points the app does not call;
application imports alone do not identify Qt's embedded third-party code.
"""
import argparse
import hashlib
import json
from pathlib import Path


PROFILE = 'qt-5.14.2-mingw73-x64-windows'
NOTICE_NAME = 'ThirdPartyNotices.txt'
# Reviewed against QtBase's .pri sources and the shipping kit's feature files.
# Excludes Android/Gradle, Cocoa, Unix forkfd/XCB, ARM-only pixman, QtDBus,
# QtTest, SQLite and WebAssembly fonts: none are part of this distribution.
QT_COMPONENTS = set('''angle angle-arrayboundsclamper angle-murmurhash
angle-systeminfo angle-trace_event doubleconversion easing freebsd
freetype freetype-zlib freetype-bdf freetype-pcf harfbuzz harfbuzz-ng
iaccessible2 icc-sRGB-color-profile libjpeg libpng md4 md4c md5 pcre2
pcre2-sljit rfc6234 sha1 sha3_endian sha3_keccak tinycbor
VulkanMemoryAllocator wintab zlib qbig5codecs qeucjpcodec qeuckrcodec
qjiscodec qsjiscodec qtsciicodec qbkcodec psl unicode-character-database
unicode-cldr opengl-headers opengl-es2-headers grayraster
smooth-scaling-algorithm webgradients xserverhelper aglfn vulkan-xml-spec'''.split())


def validate_kit(kit):
    checks = {
        'mkspecs/qconfig.pri': ('QT_VERSION = 5.14.2', 'QT_ARCH = x86_64',
                               'QT_EDITION = OpenSource', 'QT_GCC_MAJOR_VERSION = 7',
                               'QT_GCC_MINOR_VERSION = 3', 'CONFIG += shared'),
        'include/QtCore/qtcore-config.h': ('#define QT_FEATURE_big_codecs 1',
                                         '#define QT_FEATURE_topleveldomain 1'),
        'include/QtGui/qtgui-config.h': ('#define QT_FEATURE_angle 1',
                                       '#define QT_FEATURE_vulkan 1',
                                       '#define QT_FEATURE_textmarkdownreader 1',
                                       '#define QT_FEATURE_accessibility 1',
                                       '#define QT_FEATURE_pdf 1'),
    }
    for relative, tokens in checks.items():
        content = (kit / relative).read_text(encoding='utf-8')
        if any(token not in content for token in tokens):
            raise ValueError('Qt kit does not match the maintained notice profile: ' + relative)
    for module in ('core', 'gui'):
        content = (kit / f'mkspecs/modules/qt_lib_{module}_private.pri').read_text(encoding='utf-8')
        disabled = next(line for line in content.splitlines() if '.disabled_features =' in line).split('=', 1)[1].split()
        enabled = next(line for line in content.splitlines() if '.enabled_features =' in line).split('=', 1)[1].split()
        required = {'core': {'doubleconversion', 'sha3-fast'},
                    'gui': {'freetype', 'harfbuzz', 'jpeg', 'png', 'vkgen'}}[module]
        bundled = {'core': {'system-pcre2', 'system-doubleconversion'},
                   'gui': {'system-freetype', 'system-harfbuzz', 'system-jpeg', 'system-png'}}[module]
        if not required <= set(enabled) or not bundled <= set(disabled):
            raise ValueError('Qt feature selection needs a notice-profile review: ' + module)
    global_config = (kit / 'mkspecs/qmodule.pri').read_text(encoding='utf-8')
    disabled = next(line for line in global_config.splitlines() if '.disabled_features =' in line).split('=', 1)[1].split()
    if 'system-zlib' not in disabled:
        raise ValueError('Qt zlib selection needs a notice-profile review.')


def collect(qt_source, qt_kit, mingw_licenses, microsoft_notices, output):
    if output.exists() and any(output.iterdir()):
        raise ValueError('Notice output must be new or empty.')
    qtbase = (qt_source / 'qtbase').resolve(strict=True)
    if 'MODULE_VERSION = 5.14.2' not in (qtbase / '.qmake.conf').read_text():
        raise ValueError('Notices must match the selected Qt 5.14.2 source tree.')
    validate_kit(qt_kit)
    inputs = {}

    def read(source):
        raw = source.read_bytes()
        inputs[source.name + ':' + hashlib.sha256(raw).hexdigest()] = True
        return raw.decode('utf-8-sig').replace('\r\n', '\n').strip()

    sections = []

    def section(title, body):
        sections.append(title + '\n' + '=' * len(title) + '\n' + body.strip())

    section('UCAL third-party runtime notices', '''These notices apply to the dependency code distributed with UCAL.
They do not change the license of UCAL's own source code.
Full LGPL-3.0 terms are in ../LICENSE; GPL-3.0 terms are in GPL-3.0.txt.
Qt libraries are dynamically linked and may be replaced with compatible versions.
This software is based in part on the work of the Independent JPEG Group.
Portions of this software are copyright (C) The FreeType Project
(www.freetype.org). All rights reserved.''')
    section('Qt 5.14.2', '''Copyright (C) The Qt Company Ltd. and other Qt contributors.
Modules: Core, Gui, Widgets, Network, Concurrent.
Plugins: Windows platform, Windows Vista style, ICO and JPEG image formats.
License: GNU Lesser General Public License version 3 (see ../LICENSE and GPL-3.0.txt).
Source: https://download.qt.io/archive/qt/5.14/5.14.2/single/qt-everywhere-src-5.14.2.tar.xz
The following components are embedded in these libraries or the ANGLE DLLs.''')

    records = {}
    for metadata in sorted(qtbase.rglob('qt_attribution.json')):
        raw = json.loads(metadata.read_text(encoding='utf-8'), strict=False)
        for record in raw if isinstance(raw, list) else [raw]:
            identifier = record['Id']
            if identifier in QT_COMPONENTS:
                if identifier in records:
                    raise ValueError('Duplicate Qt attribution: ' + identifier)
                records[identifier] = (metadata, record)
    if QT_COMPONENTS != set(records):
        raise ValueError('Missing selected Qt attribution: ' + ', '.join(sorted(QT_COMPONENTS - set(records))))
    license_sections = {}
    for identifier, (metadata, record) in sorted(records.items()):
        body = '\n'.join(str(record[key]) for key in ('Copyright', 'License', 'Homepage') if record.get(key))
        names = record.get('LicenseFile', [])
        names = [names] if isinstance(names, str) else names
        # Choose the permissive FreeType license, rather than its optional GPLv2.
        if identifier in ('freetype', 'grayraster'):
            names = [str(qtbase / 'src/3rdparty/freetype/docs/FTL.TXT')]
        if identifier == 'libjpeg':
            names += ['src/README.ijg']  # The roll-up LICENSE references the IJG terms.
        for name in names:
            source = metadata.parent / name
            if not source.is_file():
                source = metadata.parent / record.get('Path', '') / name
            source = source.resolve(strict=True)
            if not source.is_relative_to(qtbase):
                raise ValueError('Qt notice reference escapes source tree.')
            license_text = read(source)
            if identifier == 'libjpeg' and name == 'src/README.ijg':
                # Keep the legal section, not JPEG usage/build documentation.
                license_text = license_text.split('LEGAL ISSUES\n============\n', 1)[1].split('\nREFERENCES\n==========', 1)[0].strip()
            if license_text in license_sections:
                body += '\n\nLicense text: see the "' + license_sections[license_text] + '" section above.'
            else:
                body += '\n\n' + license_text
                license_sections[license_text] = record['Name']
        section(record['Name'], body)

    section('GCC runtime libraries', '''libgcc_s_seh-1.dll and libstdc++-6.dll, GCC 7.3 toolchain.
Copyright (C) Free Software Foundation, Inc. and contributors.
License: GPL-3.0 with GCC Runtime Library Exception 3.1.
The compiler itself is not distributed. See GPL-3.0.txt and the exception below.
Source: https://gcc.gnu.org/releases.html
\n''' + read(mingw_licenses / 'gcc/COPYING.RUNTIME'))
    section('MinGW-w64 runtime', read(mingw_licenses / 'mingw-w64/COPYING.MinGW-w64-runtime.txt'))
    section('winpthreads runtime', 'libwinpthread-1.dll\n\n' + read(mingw_licenses / 'winpthreads/COPYING'))
    # A curated runtime notice, not the IDE's EULA, SDK developer agreement,
    # redistribution file list, or a saved web page. Kept outside source Git.
    microsoft = read(microsoft_notices / 'Microsoft-Runtime.txt')
    if 'MICROSOFT SOFTWARE LICENSE TERMS' not in microsoft or 'D3Dcompiler_47.dll' not in microsoft:
        raise ValueError('Supply the Microsoft runtime terms and shader-compiler notice.')
    section('Microsoft runtime components', microsoft)
    target_text = '\n\n'.join(sections) + '\n'
    output.mkdir(parents=True, exist_ok=True)
    target = output / NOTICE_NAME
    target.write_text(target_text, encoding='utf-8', newline='\n')
    manifest = {
        'schema': 'org.ucal.runtime-notices/2', 'profile': PROFILE,
        'qtVersion': '5.14.2', 'qtComponents': sorted(records),
        'files': [{'path': NOTICE_NAME, 'sha256': hashlib.sha256(target.read_bytes()).hexdigest()}],
        'inputDigests': sorted(inputs),
    }
    (output / 'RuntimeNotices.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return {'status': 'collected', 'userNoticeFiles': 1, 'qtComponents': len(records)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qt-source', type=Path, required=True)
    parser.add_argument('--qt-kit', type=Path, required=True)
    parser.add_argument('--mingw-licenses', type=Path, required=True)
    parser.add_argument('--microsoft-notices', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(collect(args.qt_source, args.qt_kit, args.mingw_licenses, args.microsoft_notices, args.output)))
