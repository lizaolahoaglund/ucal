QT += core gui widgets concurrent network testlib

CONFIG += c++17 testcase console
CONFIG -= app_bundle
msvc:QMAKE_CXXFLAGS += /utf-8
TEMPLATE = app
TARGET = UcalManagerModelTests

INCLUDEPATH += ../../manager/src ../../ucal/include

SOURCES += \
    ../src/Branding.cpp \
    ../src/HardwareChangeMonitor.cpp \
    ManagerModelTest.cpp \
    ../src/MainWindow.cpp \
    ../src/ProcessWatchdog.cpp \
    ../src/RuntimeDiscovery.cpp \
    ../src/SingleInstance.cpp \
    ../src/ShortcutWriter.cpp \
    ../../ucal/src/core/Model.cpp \
    ../../ucal/src/core/Protocol.cpp \
    ../../ucal/src/ipc/Client.cpp

HEADERS += \
    ../src/HardwareChangeMonitor.h \
    ../src/MainWindow.h \
    ../src/ProcessWatchdog.h \
    ../src/RuntimeDiscovery.h \
    ../src/SingleInstance.h \
    ../src/ShortcutWriter.h \
    ../../ucal/include/ucal/Client.hpp \
    ../../ucal/include/ucal/Model.hpp \
    ../../ucal/include/ucal/Protocol.hpp

DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc

win32:LIBS += -lversion -lole32 -luuid -ladvapi32 -lshell32 -luser32

SOURCES += ../../frontends/pcanview/src/SoftwareAdapter.cpp ../../frontends/pcanview/src/PcanViewText.cpp
SOURCES += ../../frontends/canoe/src/DeploymentAssets.cpp ../../frontends/canoe/src/CanoeText.cpp
SOURCES += ../../frontends/canoe/src/LaunchCopy.cpp ../../frontends/canoe/src/PeForwarder.cpp

INCLUDEPATH += ../../manager/src/control ../../ucal/include ../..

RESOURCES += ../res/branding.qrc ../res/resources.qrc

include(../src/control/Control.pri)
