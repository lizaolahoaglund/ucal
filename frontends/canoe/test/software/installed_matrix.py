"""Run the installed CANoe 11 and 12 physical acceptance subsets."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys


def installed_executable(major, override):
    if override is not None:
        return override.resolve(strict=True)

    import winreg

    registered = set()
    uninstall = r'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall'
    prefix = f'Vector CANoe {major}.'
    for view in (winreg.KEY_WOW64_64KEY, winreg.KEY_WOW64_32KEY):
        try:
            root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, uninstall, 0, winreg.KEY_READ | view)
        except FileNotFoundError:
            continue
        with root:
            for index in range(winreg.QueryInfoKey(root)[0]):
                try:
                    with winreg.OpenKey(root, winreg.EnumKey(root, index)) as entry:
                        name, _ = winreg.QueryValueEx(entry, 'DisplayName')
                        if not name.casefold().startswith(prefix.casefold()) or '64 bit' not in name.casefold():
                            continue
                        try:
                            location, _ = winreg.QueryValueEx(entry, 'InstallLocation')
                        except FileNotFoundError:
                            location = ''
                        if location.strip():
                            registered.add(Path(location.strip().strip('"')) / 'Exec64/CANoe64.exe')
                        else:
                            registered.add(None)
                except FileNotFoundError:
                    continue

    program_files = os.environ.get('ProgramW6432') or os.environ.get('ProgramFiles')
    candidates = {path for path in registered if path is not None}
    if program_files:
        candidates.add(Path(program_files) / f'Vector CANoe {major}.0/Exec64/CANoe64.exe')
    existing = {path.resolve() for path in candidates if path.is_file()}
    if len(existing) > 1:
        raise ValueError(f'Multiple CANoe {major} installations found. Select one with --canoe{major}-exe.')
    if existing:
        return existing.pop()
    if registered:
        raise ValueError(f'CANoe {major} is registered but CANoe64.exe could not be located. Check the installation or set --canoe{major}-exe.')
    return None


def check_version(executable, major):
    import win32api

    if not executable.is_file() or executable.name.casefold() != 'canoe64.exe':
        raise ValueError(f'Expected a CANoe64.exe file: {executable}')
    try:
        info = win32api.GetFileVersionInfo(str(executable), '\\')
    except win32api.error as error:
        raise ValueError(f'Cannot read the CANoe executable version: {executable}') from error
    actual = info['FileVersionMS'] >> 16
    if actual != major:
        raise ValueError(f'Expected CANoe {major}, found version {actual}: {executable}')


def run_case(arguments, directory):
    runner = Path(__file__).with_name('installed_launch.py')
    with directory.with_suffix('.log').open('wb') as log:
        process = subprocess.run([sys.executable, '-B', str(runner), *arguments,
                                  '--evidence-directory', str(directory)],
                                 stdout=log, stderr=subprocess.STDOUT)
    result_path = directory / 'result.json'
    result = json.loads(result_path.read_text(encoding='utf-8')) if result_path.is_file() else {}
    passed = process.returncode == 0 and result.get('status') == 'passed'
    summary = {'status': 'passed' if passed else 'failed', 'exitCode': process.returncode,
               'result': f'{directory.name}/result.json' if result else None,
               'log': directory.with_suffix('.log').name}
    if not passed:
        summary['reason'] = result.get('error') or 'Installed acceptance did not pass. See the case log.'
    return summary


def run_subset(major, args, directory):
    version = f'canoe{major}'
    try:
        executable = installed_executable(major, getattr(args, f'{version}_exe'))
        if executable is None:
            return {'version': major, 'status': 'skipped', 'reason': f'CANoe {major} x64 is not installed.'}
        check_version(executable, major)
        # The legacy copy suffix adds 45 characters to LOCALAPPDATA. Keep
        # the standard cases on the legacy side of the 144-character budget.
        if len(str((directory / 'classic-standard/local-data').resolve())) > 99:
            raise ValueError('Use a shorter evidence directory for the standard path cases.')
        configurations = {mode: getattr(args, f'{version}_{mode}_config') for mode in ('classic', 'fd')}
        for mode, path in configurations.items():
            if path is None or not path.is_file():
                raise ValueError(f'Specify an existing {mode} configuration with --{version}-{mode}-config.')
    except (ImportError, OSError, ValueError) as error:
        return {'version': major, 'status': 'failed', 'reason': str(error)}

    directory.mkdir()
    peer_option = '--vector-peer-channel' if args.vector_peer_channel is not None else '--pcan-peer-channel'
    peer = args.vector_peer_channel if args.vector_peer_channel is not None else args.pcan_peer_channel
    common = ['--package-root', str(args.package_root), '--runtime-root', str(args.runtime_root),
              '--canoe-exe', str(executable), '--integration-adapter', 'vxl-process-route',
              '--fresh-launch-copy', '--create-application', '--pcan-endpoint-id', args.pcan_endpoint_id,
              '--endpoint-label', args.endpoint_label, peer_option, str(peer)]
    cases = []
    for mode, config in configurations.items():
        for depth, minimum in (('standard', '0'), ('deep', '111')):
            name = f'{mode}-{depth}'
            try:
                result = run_case(common + ['--configuration', str(config), '--mode', mode,
                                            '--local-app-data-min-length', minimum], directory / name)
            except (OSError, ValueError) as error:
                result = {'status': 'failed', 'reason': str(error)}
            cases.append({'case': name, **result})
    status = 'failed' if any(case['status'] == 'failed' for case in cases) else 'passed'
    return {'version': major, 'status': status, 'directory': directory.name, 'cases': cases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package-root', required=True, type=Path)
    parser.add_argument('--runtime-root', required=True, type=Path)
    parser.add_argument('--evidence-directory', required=True, type=Path)
    parser.add_argument('--pcan-endpoint-id', required=True)
    parser.add_argument('--endpoint-label', required=True)
    peer = parser.add_mutually_exclusive_group(required=True)
    peer.add_argument('--vector-peer-channel', type=int)
    peer.add_argument('--pcan-peer-channel')
    for major in (11, 12):
        parser.add_argument(f'--canoe{major}-exe', type=Path, help='Explicit installation path for this version')
        for mode in ('classic', 'fd'):
            parser.add_argument(f'--canoe{major}-{mode}-config', type=Path)
    args = parser.parse_args()
    root = args.evidence_directory
    root.mkdir(parents=True)
    subsets = [run_subset(major, args, root / f'canoe{major}') for major in (11, 12)]
    statuses = {subset['status'] for subset in subsets}
    status = 'failed' if 'failed' in statuses else 'passed' if 'passed' in statuses else 'skipped'
    result = json.dumps({'status': status, 'subsets': subsets}, indent=2, ensure_ascii=False)
    (root / 'result.json').write_text(result + '\n', encoding='utf-8')
    print(result)
    return int(status == 'failed')


if __name__ == '__main__':
    sys.exit(main())
