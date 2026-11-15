#include "PeForwarder.h"
#include <QSet>
#include <algorithm>
#include <cstring>
#include <windows.h>

namespace Ucal::Manager {
namespace {
bool fail(QString* error, const QString& message) {
    if (error) *error = message;
    return false;
}
template<class T> bool read(const QByteArray& bytes, quint64 offset, T* value) {
    if (offset > quint64(bytes.size()) || sizeof(T) > quint64(bytes.size()) - offset) return false;
    std::memcpy(value, bytes.constData() + int(offset), sizeof(T));
    return true;
}
template<class T> void put(QByteArray& bytes, int offset, const T& value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}
quint32 align(quint32 value, quint32 boundary) { return (value + boundary - 1) & ~(boundary - 1); }
bool exportString(const QByteArray& value) {
    if (value.isEmpty() || value.size() > 4096) return false;
    for (const auto c : value) if (static_cast<unsigned char>(c) < 33 || static_cast<unsigned char>(c) > 126) return false;
    return true;
}
bool moduleName(const QByteArray& value) {
    if (value.isEmpty()) return false;
    for (const auto c : value)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_') return false;
    return true;
}
}

bool readPeExports(const QByteArray& image, PeExportTable* exports, QString* error) {
    if (!exports) return fail(error, QStringLiteral("Missing export output"));
    *exports = {};
    if (error) error->clear();
    IMAGE_DOS_HEADER dos{};
    DWORD signature{};
    IMAGE_FILE_HEADER file{};
    if (!read(image, 0, &dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 64 ||
        !read(image, quint32(dos.e_lfanew), &signature) || signature != IMAGE_NT_SIGNATURE ||
        !read(image, quint64(dos.e_lfanew) + 4, &file) || file.NumberOfSections == 0 || file.NumberOfSections > 96)
        return fail(error, QStringLiteral("Invalid PE header"));
    const quint64 optionalOffset = quint64(dos.e_lfanew) + 4 + sizeof(file);
    WORD magic{};
    IMAGE_DATA_DIRECTORY directory{};
    DWORD headersSize{};
    if (!read(image, optionalOffset, &magic)) return fail(error, QStringLiteral("Missing PE optional header"));
    if (magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC && file.Machine == IMAGE_FILE_MACHINE_I386) {
        IMAGE_OPTIONAL_HEADER32 optional{};
        if (file.SizeOfOptionalHeader < sizeof(optional) || !read(image, optionalOffset, &optional) || optional.NumberOfRvaAndSizes < 1)
            return fail(error, QStringLiteral("Invalid x86 optional header"));
        directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]; headersSize = optional.SizeOfHeaders;
    } else if (magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && file.Machine == IMAGE_FILE_MACHINE_AMD64) {
        IMAGE_OPTIONAL_HEADER64 optional{};
        if (file.SizeOfOptionalHeader < sizeof(optional) || !read(image, optionalOffset, &optional) || optional.NumberOfRvaAndSizes < 1)
            return fail(error, QStringLiteral("Invalid x64 optional header"));
        directory = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]; headersSize = optional.SizeOfHeaders;
    } else return fail(error, QStringLiteral("Unsupported PE architecture"));
    QVector<IMAGE_SECTION_HEADER> sections;
    for (quint32 index = 0; index < file.NumberOfSections; ++index) {
        IMAGE_SECTION_HEADER section{};
        if (!read(image, optionalOffset + file.SizeOfOptionalHeader + quint64(index) * sizeof(section), &section))
            return fail(error, QStringLiteral("Truncated PE sections"));
        sections.push_back(section);
    }
    const auto offset = [&](quint32 rva, quint64 size) -> qint64 {
        if (rva < headersSize && size <= quint64(headersSize) - rva && quint64(rva) + size <= quint64(image.size())) return rva;
        for (const auto& section : sections) {
            if (rva < section.VirtualAddress) continue;
            const quint64 delta = quint64(rva) - section.VirtualAddress;
            const quint64 raw = quint64(section.PointerToRawData) + delta;
            if (delta <= section.SizeOfRawData && size <= quint64(section.SizeOfRawData) - delta &&
                raw <= quint64(image.size()) && size <= quint64(image.size()) - raw) return qint64(raw);
        }
        return -1;
    };
    const auto string = [&](quint32 rva, QByteArray* result) {
        result->clear();
        for (quint32 index = 0; index <= 4096; ++index) {
            if (quint64(rva) + index > 0xffffffffu) return false;
            const auto at = offset(rva + index, 1);
            if (at < 0) return false;
            const auto c = image.at(int(at));
            if (!c) return exportString(*result);
            result->append(c);
        }
        return false;
    };
    IMAGE_EXPORT_DIRECTORY table{};
    const auto at = offset(directory.VirtualAddress, sizeof(table));
    if (!directory.VirtualAddress || directory.Size < sizeof(table) || at < 0 || !read(image, quint64(at), &table) ||
        table.NumberOfFunctions == 0 || table.NumberOfFunctions > 65535 || table.NumberOfNames > 65535 ||
        table.Base < 1 || quint64(table.Base) + table.NumberOfFunctions - 1 > 65535)
        return fail(error, QStringLiteral("Invalid PE export directory"));
    const auto functions = offset(table.AddressOfFunctions, quint64(table.NumberOfFunctions) * 4);
    const auto names = offset(table.AddressOfNames, quint64(table.NumberOfNames) * 4);
    const auto ordinals = offset(table.AddressOfNameOrdinals, quint64(table.NumberOfNames) * 2);
    if (functions < 0 || (table.NumberOfNames && (names < 0 || ordinals < 0)))
        return fail(error, QStringLiteral("Invalid PE export arrays"));
    PeExportTable result; result.machine = file.Machine; result.ordinalBase = table.Base;
    for (quint32 index = 0; index < table.NumberOfFunctions; ++index) {
        DWORD rva{}; read(image, quint64(functions) + index * 4, &rva);
        if (rva && offset(rva, 1) < 0) return fail(error, QStringLiteral("Invalid PE export address"));
        result.functions.push_back(rva != 0);
    }
    QSet<QByteArray> seen;
    for (quint32 index = 0; index < table.NumberOfNames; ++index) {
        DWORD rva{}; WORD slot{}; QByteArray name;
        read(image, quint64(names) + index * 4, &rva); read(image, quint64(ordinals) + index * 2, &slot);
        if (slot >= table.NumberOfFunctions || !result.functions.at(slot) || !string(rva, &name) || seen.contains(name))
            return fail(error, QStringLiteral("Invalid PE export name or ordinal"));
        seen.insert(name); result.names.push_back({name, slot});
    }
    *exports = result;
    return true;
}

QByteArray makePeForwarder(const PeExportTable& exports, const QByteArray& library,
    const QMap<quint32, QByteArray>& targets, QString* error) {
    if (error) error->clear();
    if ((exports.machine != IMAGE_FILE_MACHINE_I386 && exports.machine != IMAGE_FILE_MACHINE_AMD64) ||
        exports.functions.isEmpty() || exports.functions.size() > 65535 || exports.ordinalBase < 1 ||
        quint64(exports.ordinalBase) + exports.functions.size() - 1 > 65535 || !exportString(library)) {
        fail(error, QStringLiteral("Invalid forwarder metadata")); return {};
    }
    auto names = exports.names;
    std::sort(names.begin(), names.end(), [](const PeExportName& a, const PeExportName& b) { return a.name < b.name; });
    QSet<QByteArray> seen;
    for (const auto& name : names) {
        if (!exportString(name.name) || name.slot >= exports.functions.size() || !exports.functions.at(name.slot) || seen.contains(name.name)) {
            fail(error, QStringLiteral("Invalid forwarder export name")); return {};
        }
        seen.insert(name.name);
    }
    constexpr quint32 dataRva = 0x1000;
    const int functionsAt = sizeof(IMAGE_EXPORT_DIRECTORY);
    const int namesAt = functionsAt + exports.functions.size() * 4;
    const int ordinalsAt = namesAt + names.size() * 4;
    QByteArray data(ordinalsAt + names.size() * 2, '\0');
    const auto append = [&data, dataRva](const QByteArray& value) { const quint32 rva = dataRva + quint32(data.size()); data += value; data += '\0'; return rva; };
    IMAGE_EXPORT_DIRECTORY table{};
    table.Name = append(library); table.Base = exports.ordinalBase;
    table.NumberOfFunctions = DWORD(exports.functions.size()); table.NumberOfNames = DWORD(names.size());
    table.AddressOfFunctions = dataRva + functionsAt;
    table.AddressOfNames = dataRva + namesAt; table.AddressOfNameOrdinals = dataRva + ordinalsAt;
    for (int index = 0; index < exports.functions.size(); ++index) if (exports.functions[index]) {
        const auto target = targets.value(quint32(index));
        if (!exportString(target) || !target.contains('.')) { fail(error, QStringLiteral("Missing forward target")); return {}; }
        const DWORD rva = append(target); put(data, functionsAt + index * 4, rva);
    }
    for (int index = 0; index < names.size(); ++index) {
        const DWORD rva = append(names[index].name); put(data, namesAt + index * 4, rva);
        const WORD slot = names[index].slot; put(data, ordinalsAt + index * 2, slot);
    }
    put(data, 0, table);
    const quint32 rawSize = align(quint32(data.size()), 0x200);
    QByteArray image(0x200 + int(rawSize), '\0');
    IMAGE_DOS_HEADER dos{}; dos.e_magic = IMAGE_DOS_SIGNATURE; dos.e_lfanew = 0x80; put(image, 0, dos);
    const DWORD signature = IMAGE_NT_SIGNATURE; put(image, 0x80, signature);
    IMAGE_FILE_HEADER file{}; file.Machine = exports.machine; file.NumberOfSections = 1;
    file.Characteristics = IMAGE_FILE_EXECUTABLE_IMAGE | IMAGE_FILE_DLL | IMAGE_FILE_LARGE_ADDRESS_AWARE;
    const bool x64 = exports.machine == IMAGE_FILE_MACHINE_AMD64;
    file.SizeOfOptionalHeader = WORD(x64 ? sizeof(IMAGE_OPTIONAL_HEADER64) : sizeof(IMAGE_OPTIONAL_HEADER32));
    if (!x64) file.Characteristics |= IMAGE_FILE_32BIT_MACHINE;
    put(image, 0x84, file);
    const auto fill = [&](auto& optional) {
        optional.SizeOfInitializedData = rawSize; optional.ImageBase = 0x18000000;
        optional.SectionAlignment = 0x1000; optional.FileAlignment = 0x200;
        optional.MajorOperatingSystemVersion = 6; optional.MajorSubsystemVersion = 6;
        optional.SizeOfImage = align(dataRva + quint32(data.size()), 0x1000); optional.SizeOfHeaders = 0x200;
        optional.Subsystem = IMAGE_SUBSYSTEM_WINDOWS_GUI; optional.DllCharacteristics = IMAGE_DLLCHARACTERISTICS_NX_COMPAT;
        optional.SizeOfStackReserve = 0x100000; optional.SizeOfStackCommit = 0x1000;
        optional.SizeOfHeapReserve = 0x100000; optional.SizeOfHeapCommit = 0x1000;
        optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {dataRva, DWORD(data.size())};
        put(image, 0x98, optional);
    };
    if (x64) { IMAGE_OPTIONAL_HEADER64 optional{}; optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC; fill(optional); }
    else { IMAGE_OPTIONAL_HEADER32 optional{}; optional.Magic = IMAGE_NT_OPTIONAL_HDR32_MAGIC; fill(optional); }
    IMAGE_SECTION_HEADER section{}; std::memcpy(section.Name, ".edata", 6);
    section.Misc.VirtualSize = DWORD(data.size()); section.VirtualAddress = dataRva;
    section.SizeOfRawData = rawSize; section.PointerToRawData = 0x200;
    section.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
    put(image, 0x98 + file.SizeOfOptionalHeader, section);
    std::memcpy(image.data() + 0x200, data.constData(), size_t(data.size()));
    return image;
}

QByteArray makeCanoeForwarder(const QByteArray& origin, const QByteArray& data,
    quint16 machine, const QByteArray& library, const QByteArray& originModule,
    const QByteArray& dataModule, QString* error) {
    PeExportTable vendor, ucal;
    if (!moduleName(originModule) || !moduleName(dataModule) || !readPeExports(origin, &vendor, error) ||
        !readPeExports(data, &ucal, error)) return {};
    if (vendor.machine != machine || ucal.machine != machine) { fail(error, QStringLiteral("Runtime architecture mismatch")); return {}; }
    QSet<QByteArray> supplied, expected;
    for (const auto& name : ucal.names) supplied.insert(name.name);
    for (const auto& name : vendor.names) expected.insert(name.name);
    for (const auto& required : {QByteArray("xlOpenDriver"), QByteArray("xlGetDriverConfigEx")})
        if (!expected.contains(required) || !supplied.contains(required)) { fail(error, QStringLiteral("Runtime lacks required XL entry points")); return {}; }
    QMap<quint32, QByteArray> targets;
    for (int index = 0; index < vendor.functions.size(); ++index) if (vendor.functions[index])
        targets.insert(quint32(index), originModule + ".#" + QByteArray::number(vendor.ordinalBase + quint32(index)));
    QSet<quint16> overridden;
    for (const auto& name : vendor.names) if (supplied.contains(name.name) && !name.name.startsWith('?')) {
        const auto target = dataModule + '.' + name.name;
        if (overridden.contains(name.slot) && targets.value(name.slot) != target) { fail(error, QStringLiteral("Conflicting export aliases")); return {}; }
        targets.insert(name.slot, target); overridden.insert(name.slot);
    }
    return makePeForwarder(vendor, library, targets, error);
}
} // namespace Ucal::Manager
