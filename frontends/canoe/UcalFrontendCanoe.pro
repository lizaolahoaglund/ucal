QT += core gui widgets
CONFIG += c++17 dll
CONFIG -= app_bundle
TEMPLATE = lib
TARGET = UcalFrontendCanoe
SOURCES += $$PWD/src/FrontendAdapter.cpp $$PWD/src/SoftwareAdapter.cpp $$PWD/src/ProductAdapter.cpp $$PWD/src/CanoeText.cpp $$PWD/src/Deployment.cpp $$PWD/src/DeploymentAssets.cpp
SOURCES += $$PWD/src/LaunchCopy.cpp $$PWD/src/PeForwarder.cpp
isEmpty(UCAL_SOURCE_ROOT):UCAL_SOURCE_ROOT = $$clean_path($$PWD/../..)
include($$UCAL_SOURCE_ROOT/manager/src/frontend/Frontend.pri)
