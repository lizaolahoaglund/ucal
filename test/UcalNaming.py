"""Source naming policy for original Ucal C/C++ code; no compiler/runtime needed."""
from pathlib import Path
import re

# Required spellings from C++, the standard library, Qt and the Windows SDK.
STANDARD_IDENTIFIERS = set('''condition_variable uint32_t uint16_t uint8_t size_t static_assert wchar_t static_cast reinterpret_cast uint64_t steady_clock time_point shared_ptr lock_guard time_since_epoch find_if pop_front push_back unique_lock this_thread sleep_for notify_all make_unique c_str to_string any_of make_shared notify_one unique_ptr find_last_of int32_t find_first_not_of numeric_limits get_id packaged_task get_future emplace_back count_if remove_if unordered_map int16_t int8_t copy_n unordered_set atomic_bool atomic_uint32_t atomic_int32_t fetch_add duration_cast fill_n uintptr_t recursive_mutex compare_exchange_strong has_value push_front all_of swprintf_s none_of e_magic e_lfanew dbcc_size dbcc_devicetype istreambuf_iterator to_wstring copy_if back_inserter atomic_uintptr_t compare_exchange_weak wait_for qobject_cast find_first_of defer_lock length_error make_pair make_tuple make_move_iterator try_to_lock owns_lock const_cast atomic_load atomic_store pop_back int64_t fetch_sub runtime_error future_status ptrdiff_t strcpy_s initializer_list dynamic_cast thread_local char16_t char32_t char8_t wait_until value_or try_emplace insert_or_assign equal_range lower_bound upper_bound shrink_to_fit max_size va_start va_end va_copy get_allocator native_handle hardware_concurrency is_lock_free test_and_set fetch_or fetch_and fetch_xor find_last_not_of'''.split())
TOKENS = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|(?:u8|[uUL])?R"(?P<d>[^\s()\\]{0,16})\([\s\S]*?\)(?P=d)"|(?:u8|[uUL])?"(?:\\.|[^"\\])*"|(?:u8|[uUL])?\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*')

def code_tokens(source):
    return [m for m in TOKENS.finditer(source) if re.fullmatch(r'[A-Za-z_]\w*', m[0])]

def inspect(root):
    root = Path(root)
    failures, exceptions = [], {}
    files = sorted(path for folder in ('ucal', 'manager', 'frontends', 'backends', 'test') for path in (root/folder).rglob('*')
                   if path.suffix in {'.c', '.cpp', '.h', '.hpp'})
    for path in files:
        relative = path.relative_to(root).as_posix()
        source = path.read_text(encoding='utf-8-sig')
        tokens = code_tokens(source)
        clean = list(source)
        for token in TOKENS.finditer(source):
            if not re.fullmatch(r'[A-Za-z_]\w*', token[0]):
                clean[token.start():token.end()] = ' ' * len(token[0])
        clean = ''.join(clean)
        for token in tokens:
            name = token[0]
            if re.fullmatch(r'[a-z][a-z0-9]*(?:_[a-z0-9]+)+_?', name):
                if name in STANDARD_IDENTIFIERS or name.endswith('_t'):
                    exceptions[name] = exceptions.get(name, 0) + 1
                else:
                    failures.append(f'{relative}: nonstandard snake_case identifier {name}')
        for name in re.findall(r'\b(?:class|struct|enum\s+class)\s+(\w+)\s*(?::[^;{]*)?\{', clean):
            if not re.fullmatch(r'[A-Z][A-Za-z0-9]*', name):
                failures.append(f'{relative}: type name must be PascalCase: {name}')
        for name in re.findall(r'namespace\s+([\w:]+)\s*\{', clean):
            if name == 'std':
                continue
            if not all(re.fullmatch(r'[A-Z][A-Za-z0-9]*', part) for part in name.split('::')):
                failures.append(f'{relative}: namespace name must be PascalCase: {name}')
        if path.name != 'main.cpp' and not re.fullmatch(r'[A-Z][A-Za-z0-9]*\.(cpp|c|hpp|h)', path.name):
            # Standalone C fixtures use C/POSIX filenames, independent of C++ modules.
            if path.suffix != '.c':
                failures.append(f'{relative}: C++ module filename must be PascalCase')
    legacy_fixtures = []
    for folder in ('ucal', 'manager', 'frontends', 'backends', 'test'):
        for path in (root/folder).rglob('*.def'):
            for line in path.read_text(encoding='utf-8-sig').splitlines():
                if line.lstrip().upper().startswith('LIBRARY '):
                    continue
                relative = path.relative_to(root).as_posix()
                for name in re.findall(r'(?<![A-Za-z0-9])_?(ucal\w+)\b', line, re.I):
                    if name.startswith('Ucal') and re.fullmatch(r'[A-Z][A-Za-z0-9]*', name):
                        continue
                    if ('/test/' in relative and path.name.startswith('Legacy')
                            and name in {'ucal_backend_get_api', 'ucal_frontend_get_api'}):
                        legacy_fixtures.append(relative)
                    else:
                        failures.append(f'{relative}: original C export/implementation alias must use PascalCase: {name}')

    return {'schema': 'org.ucal.naming-audit/1', 'status': 'failed' if failures else 'passed',
            'codeFileCount': len(files), 'failures': sorted(set(failures)),
            'standardIdentifiers': dict(sorted(exceptions.items())),
            'legacyEntryPointFixtures': sorted(set(legacy_fixtures))}
