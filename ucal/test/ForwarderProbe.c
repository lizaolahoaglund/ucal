#include <windows.h>

typedef int (__cdecl *UcalFixtureFunction)(void);

int wmain(int argc, wchar_t** argv) {
    HMODULE module;
    UcalFixtureFunction function;
    int result;
    if (argc != 2) return 2;
    module = LoadLibraryW(argv[1]);
    if (!module) return 3;
    function = (UcalFixtureFunction)GetProcAddress(module, "?opaqueVendorExport@@YAHXZ");
    result = function && function() == 1 ? 0 : 4;
    FreeLibrary(module);
    return result;
}
