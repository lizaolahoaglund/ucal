QT += core gui widgets concurrent network

CONFIG += c++17
CONFIG -= app_bundle
msvc:QMAKE_CXXFLAGS += /utf-8
TEMPLATE = app
TARGET = UcalManager
VERSION = 0.9.1

INCLUDEPATH += ../ucal/include

SOURCES += \
    src/Branding.cpp \
    src/HardwareChangeMonitor.cpp \
    src/main.cpp \
    src/MainWindow.cpp \
    src/ProcessWatchdog.cpp \
    src/RuntimeDiscovery.cpp \
    src/SingleInstance.cpp \
    src/ShortcutWriter.cpp \
    ../ucal/src/core/Model.cpp \
    ../ucal/src/core/Protocol.cpp \
    ../ucal/src/ipc/Client.cpp

HEADERS += \
    src/HardwareChangeMonitor.h \
    src/MainWindow.h \
    src/ProcessWatchdog.h \
    src/RuntimeDiscovery.h \
    src/SingleInstance.h \
    src/ShortcutWriter.h \
    ../ucal/include/ucal/Client.hpp \
    ../ucal/include/ucal/Model.hpp \
    ../ucal/include/ucal/Protocol.hpp

DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc
RCC_DIR = $$OUT_PWD/rcc
UI_DIR = $$OUT_PWD/ui

win32:RC_ICONS = $$PWD/res/ucal.ico
QMAKE_TARGET_PRODUCT = UCAL Manager
QMAKE_TARGET_DESCRIPTION = UCAL Manager
win32:LIBS += -lversion -lole32 -luuid -ladvapi32 -lshell32 -luser32

INCLUDEPATH += src src/control ../ucal/include ..

RESOURCES += res/branding.qrc res/resources.qrc

include(src/control/Control.pri)
