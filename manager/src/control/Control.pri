INCLUDEPATH += $$PWD $$clean_path($$PWD/../../../ucal/include) $$clean_path($$PWD/../../..)
# Qt 5's NMake batch inference skips quoted source directories containing spaces.
# Explicit per-file rules keep extracted SDK/Manager builds portable.
msvc:CONFIG += no_batch
SOURCES += $$PWD/BackendPlugins.cpp \
    $$PWD/PluginMetadata.cpp \
    $$PWD/FrontendPlugins.cpp \
    $$PWD/LaunchPlan.cpp \
    $$PWD/PackageTool.cpp \
    $$PWD/PortablePaths.cpp \
    $$PWD/ProductLauncher.cpp \
    $$PWD/ProductOperations.cpp \
    $$PWD/SoftwareDiscovery.cpp \
    $$PWD/SoftwareFile.cpp \
    $$PWD/ManagerModel.cpp \
    $$PWD/UiPresentation.cpp \
    $$PWD/UserPreferences.cpp
HEADERS += $$PWD/BackendPlugins.h \
    $$PWD/PluginMetadata.h \
    $$PWD/FrontendPlugins.h \
    $$PWD/LaunchPlan.h \
    $$PWD/PackageTool.h \
    $$PWD/PortablePaths.h \
    $$PWD/ProductLauncher.h \
    $$PWD/ProductOperations.h \
    $$PWD/SoftwareDiscovery.h \
    $$PWD/ManagerModel.h \
    $$PWD/UiPresentation.h \
    $$PWD/UserPreferences.h
