QT += core gui widgets
CONFIG += c++17 dll
CONFIG -= app_bundle
TEMPLATE = lib
TARGET = UcalFrontendPcanView
SOURCES += $$PWD/src/FrontendAdapter.cpp $$PWD/src/SoftwareAdapter.cpp $$PWD/src/ProductAdapter.cpp $$PWD/src/PcanViewText.cpp
isEmpty(UCAL_SOURCE_ROOT):UCAL_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$UCAL_SOURCE_ROOT/manager/src/frontend/Frontend.pri)
