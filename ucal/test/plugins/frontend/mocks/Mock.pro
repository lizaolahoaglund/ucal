QT =
CONFIG += c++17 dll
msvc:CONFIG += no_batch
CONFIG -= app_bundle
TEMPLATE = lib
TARGET = UcalFrontendFixture
INCLUDEPATH += $$clean_path($$PWD/../../../../include)
SOURCES += Mock.cpp
DEF_FILE = Mock.def
contains(CONFIG, legacy_exports) {
    TARGET = UcalFrontendLegacyFixture
    DEF_FILE = $$PWD/LegacyMock.def
    !contains(QT_ARCH, x86_64):DEF_FILE = $$PWD/LegacyMockWin32.def
}
DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
