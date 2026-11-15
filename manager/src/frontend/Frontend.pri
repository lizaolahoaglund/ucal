isEmpty(UCAL_SOURCE_ROOT):UCAL_SOURCE_ROOT = $$clean_path($$PWD/../../..)
INCLUDEPATH += $$UCAL_SOURCE_ROOT/ucal/include $$UCAL_SOURCE_ROOT/manager/src/control $$UCAL_SOURCE_ROOT
msvc:QMAKE_CXXFLAGS += /utf-8
DEF_FILE = $$PWD/Exports.def
SOURCES += $$PWD/Entry.cpp
include($$UCAL_SOURCE_ROOT/manager/src/control/Control.pri)
LIBS += -lversion -lshell32 -luser32 -ladvapi32
DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
RC_FILE = $$UCAL_SOURCE_ROOT/ucal/src/plugins/PluginVersion.rc
