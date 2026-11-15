"""Opt-in installed CANoe loading or physical traffic check.
Requires Windows Python with psutil and pywin32, a built portable
package, matching x64 deterministic native fixtures, and a supported CANoe.
"""
from pathlib import Path
import argparse
import json
import hashlib
import os
import shutil
import subprocess
import time
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[4]/"test/installed"))
from lifecycle import Lifecycle, close_main_window, windows_for
from manager_flow import create_and_launch, expected_manager_window
from physical_traffic import verify_traffic


def minimum_path_length(path, minimum):
    missing = minimum - len(str(path))
    return path.parent/('x' * max(1, missing - 1))/path.name if missing > 0 else path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package-root', required=True, type=Path)
    parser.add_argument('--runtime-root', required=True, type=Path)
    parser.add_argument('--canoe-exe', required=True, type=Path)
    parser.add_argument('--evidence-directory', required=True, type=Path)
    parser.add_argument('--integration-adapter', choices=('canoe-provider', 'vxl-process-route'), default='vxl-process-route')
    parser.add_argument('--fresh-launch-copy', action='store_true',
                        help='Prepare the launch copy in a new isolated local-data directory.')
    parser.add_argument('--create-application', action='store_true',
                        help='Create, save and launch a profile in the Manager UI')
    parser.add_argument('--configuration', type=Path, help='Local isolated CANoe measurement fixture')
    parser.add_argument('--pcan-endpoint-id', type=uuid.UUID)
    parser.add_argument('--endpoint-label', help='Exact Manager channel label for GUI selection')
    peer = parser.add_mutually_exclusive_group()
    peer.add_argument('--vector-peer-channel', type=int, help='Opt-in physical Vector peer on an isolated bus')
    peer.add_argument('--pcan-peer-channel', help='Opt-in independent physical PCAN peer on an isolated bus')
    parser.add_argument('--local-app-data-min-length', type=int, default=0,
                        help='Minimum absolute length of the isolated local application data path.')
    parser.add_argument('--mode', choices=('classic', 'fd'), default='fd')
    args = parser.parse_args()
    peer_channel = args.vector_peer_channel if args.vector_peer_channel is not None else args.pcan_peer_channel
    if (args.pcan_endpoint_id is not None) != (peer_channel is not None):
        parser.error('Specify both the physical PCAN endpoint and an independent physical peer')
    if args.pcan_endpoint_id is not None and not args.configuration:
        parser.error('The physical test requires a CANoe measurement fixture')
    if args.create_application and args.pcan_endpoint_id is not None and not args.endpoint_label:
        parser.error('The physical Manager workflow requires --endpoint-label')
    if args.local_app_data_min_length < 0 or (args.local_app_data_min_length and not args.fresh_launch_copy):
        parser.error('A minimum local application data path length requires --fresh-launch-copy')
    import psutil
    import win32api
    import win32con
    import win32gui
    import win32process
    package, runtime, software, evidence = [p.resolve() for p in (
        args.package_root, args.runtime_root, args.canoe_exe, args.evidence_directory)]
    for path in [package/'UcalManager.exe', runtime/'UcalBroker.exe', runtime/'UcalHarness.exe',
                 runtime/'UcalBackendFixture.dll', software]:
        if not path.is_file():
            parser.error(f'Missing prerequisite: {path}')
    if evidence.exists():
        parser.error('Use a new evidence directory.')
    evidence.mkdir(parents=True)
    station = evidence/'package'
    shutil.copytree(package, station)
    shutil.move(str(station/'backends'), str(evidence/'original_backends'))
    configuration = None
    if args.configuration:
        source = args.configuration.resolve(strict=True)
        configuration = evidence/'configuration'/source.name
        shutil.copytree(source.parent, configuration.parent)
    endpoint_id = str(args.pcan_endpoint_id) if args.pcan_endpoint_id is not None else None
    if endpoint_id:
        shutil.copytree(evidence/'original_backends/pcan-basic', station/'backends/pcan-basic')
    fixture = station/'backends/fixture'
    if not endpoint_id:
        (fixture/'x64').mkdir(parents=True)
        shutil.copy2(runtime/'UcalBackendFixture.dll', fixture/'x64/UcalBackendFixture.dll')
        (fixture/'backend.ini').write_text('[fixture]\nchannels=2\ncan_fd=1\n', encoding='utf-16')
        (fixture/'backend.json').write_text(json.dumps({
            'schema': 'org.ucal.backend-package/5', 'id': 'installed-fixture',
            'name': 'UCAL virtual fixture', 'vendor': 'UCAL', 'version': '1.0.0', 'type': 'ucal',
            'library': 'x64/UcalBackendFixture.dll', 'configuration': 'backend.ini',
            'backend_family': 'ucal-fixture'}), encoding='utf-8')
    data = station/'data'
    data.mkdir(exist_ok=True)
    version_info = win32api.GetFileVersionInfo(str(software), '\\')
    version = '.'.join(str(n) for value in [version_info['FileVersionMS'], version_info['FileVersionLS']]
                       for n in [value >> 16, value & 0xffff])
    software_id = 'canoe:' + hashlib.sha256(str(software).replace('\\', '/').casefold().encode()).hexdigest()[:20]
    application_id = 'canoe-installed-regression'
    workspace = {'schemaVersion': 3, 'activeProjectId': 'regression',
        'projects': [{'id': 'regression', 'name': 'Regression', 'includeAllApplications': True,
                      'applicationIds': []}],
        'applications': [{'id': application_id, 'name': 'CANoe regression',
            'softwareInstallationId': software_id, 'channelCount': 1 if endpoint_id else 2,
            'assignmentMode': 'selected' if endpoint_id else 'full-access',
            'integrationAdapterId': args.integration_adapter,
            'configurationPath': str(configuration) if configuration else '',
            'mappings': [{'logicalChannel': 1, 'endpointId': endpoint_id, 'bus': 'CAN'}] if endpoint_id else []}],
        'softwareBindings': [{'id': software_id, 'family': 'canoe', 'displayName': 'Vector CANoe',
            'executablePath': str(software), 'architecture': 'x64', 'version': version,
            'source': 'Manual', 'compatibilityKind': 'warning',
            'integrationAdapters': [args.integration_adapter, 'native']}]}
    if args.create_application:
        workspace['applications'] = []
    (data/'preferences.ini').write_text('[window]\ncloseBehavior=2\n', encoding='utf-8')
    (data/'profiles.json').write_text(json.dumps(workspace), encoding='utf-8')
    env = {k: v for k, v in os.environ.items() if not k.startswith(('UCAL_', 'UCAI_', 'QT_'))}
    env.update(UCAL_BROKER_INSTANCE='ucal_installed_' + uuid.uuid4().hex,
               UCAL_BACKEND='PLUGINS',
               UCAL_MANAGER_SETTINGS_DIRECTORY=str(data))
    fresh_copy = evidence/'local-data'
    if args.fresh_launch_copy:
        fresh_copy = minimum_path_length(fresh_copy, args.local_app_data_min_length)
        fresh_copy.mkdir(parents=True)
        env['LOCALAPPDATA'] = str(fresh_copy)
    result = {'status': 'failed', 'version': version, 'physicalTraffic': False,
              'backend': 'pcan-basic' if endpoint_id else 'UCAL virtual backend plugin fixture',
              'integrationAdapter': args.integration_adapter, 'freshLaunchCopy': args.fresh_launch_copy}
    result['inputPaths'] = {'package': str(station), 'configuration': str(configuration) if configuration else '',
                            'localAppData': env.get('LOCALAPPDATA', '')}
    lifecycle = Lifecycle(evidence/'lifecycle.jsonl', ('canoe64.exe', 'runtimekernel.exe', 'runtimekernel64.exe'),
                          lambda name, title, kind, phase: expected_manager_window(name, title, kind, phase)
                          or name == 'canoe64.exe' and title.upper() == 'DISCLAIMER' and kind == '#32770' and phase == 'startup')
    result['createApplicationInManager'] = args.create_application
    functional_complete = False
    manager = broker = None
    children = []
    with lifecycle, (evidence/'manager.log').open('wb') as manager_log, (evidence/'broker.log').open('wb') as broker_log:
        try:
            broker = subprocess.Popen([str(runtime/'UcalBroker.exe'), '--allow-test-shutdown'],
                env=env, stdout=broker_log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            lifecycle.root(broker.pid)
            time.sleep(.5)
            started = time.monotonic()
            launch_args = [] if args.create_application else ['--profile', application_id]
            manager = subprocess.Popen([str(station/'UcalManager.exe'), '--language', 'en', *launch_args],
                env=env, cwd=station, stdout=manager_log, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            lifecycle.root(manager.pid)
            if args.create_application:
                created = create_and_launch(manager.pid, data, software, lifecycle, endpoint_id=endpoint_id,
                                            endpoint_label=args.endpoint_label, configuration=configuration)
                application_id = created['id']
                assert created['softwareInstallationId'] == software_id, 'Unexpected saved software installation'
                assert created['integrationAdapterId'] == args.integration_adapter, 'Unexpected saved CANoe integration'
                result['createdApplicationId'] = application_id
                started = time.monotonic()
            owned = set()
            main_window = None
            accepted = False
            deadline = started + 90
            while time.monotonic() < deadline:
                lifecycle.check()
                assert manager.poll() is None, 'Manager exited before CANoe startup'
                children = psutil.Process(manager.pid).children(recursive=True)
                owned.update(child.pid for child in children
                    if child.name().lower() in ('canoe64.exe', 'canoe.exe', 'runtimekernel.exe', 'runtimekernel64.exe'))
                windows = []
                win32gui.EnumWindows(lambda handle, _: windows.append(handle)
                    if win32process.GetWindowThreadProcessId(handle)[1] in owned
                    and win32gui.IsWindowVisible(handle) else None, None)
                for handle in windows:
                    title = win32gui.GetWindowText(handle)
                    window_pid = win32process.GetWindowThreadProcessId(handle)[1]
                    if (not accepted and title.upper() == 'DISCLAIMER'
                            and win32gui.GetClassName(handle) == '#32770'
                            and psutil.Process(window_pid).name().lower() == 'canoe64.exe'):
                        buttons = []
                        win32gui.EnumChildWindows(handle, lambda child, _: buttons.append(child)
                            if win32gui.GetWindowText(child).replace('&', '').strip().lower() == 'i accept'
                            and win32gui.GetClassName(child) == 'Button'
                            and win32gui.GetDlgCtrlID(child) == 1
                            and win32gui.IsWindowEnabled(child) else None, None)
                        assert len(buttons) == 1, 'Expected one enabled acceptance control in the owned disclaimer'
                        try:
                            win32gui.SetForegroundWindow(handle)
                        except win32gui.error:
                            continue
                        if win32gui.GetForegroundWindow() != handle:
                            continue
                        win32gui.SendMessage(buttons[0], win32con.BM_CLICK, 0, 0)
                        accepted = True
                        lifecycle.record('disclaimer-accepted', pid=window_pid)
                    elif ('CANoe' in title and win32gui.IsWindowEnabled(handle)
                          and (configuration is None or configuration.name in title)):
                        main_window = handle
                if main_window and accepted:
                    break
                time.sleep(.1)
            assert accepted, 'The owned CANoe startup disclaimer was not accepted'
            assert main_window, 'No enabled CANoe main window'
            lifecycle.set_phase('running')
            time.sleep(2)
            lifecycle.check()
            modules = set()
            module_paths = set()
            for child in children:
                if child.pid not in owned:
                    continue
                try:
                    paths = {Path(item.path) for item in child.memory_maps()}
                    modules.update(path.name.lower() for path in paths)
                    module_paths.update(path for path in paths if 'ucal' in path.name.lower())
                except (psutil.NoSuchProcess, psutil.AccessDenied):
                    pass
            expected = {'UcalCanoeProvider64.dll', 'ucal-canoe-provider64.dll',
                        'UcalVcndrvmsDataIntercept64.dll', 'ucal-vcndrvms-data-intercept64.dll'}
            expected = {name.lower() for name in expected}
            assert modules.intersection(expected), 'No UCAL compatibility module loaded in the owned CANoe tree'
            result['loadedUcalPaths'] = sorted(str(path) for path in module_paths)
            hashes = {hashlib.sha256(path.read_bytes()).hexdigest() for path in module_paths if path.is_file()}
            data32 = 'UcalCanoeProvider.dll' if args.integration_adapter == 'canoe-provider' else 'UcalVcndrvmsDataIntercept.dll'
            expected_data = [package/'frontends/canoe/x86'/data32,
                             package/'frontends/canoe/x64/UcalCanoeProvider64.dll']
            result['packagedDataModulesLoaded'] = {
                str(path.relative_to(package)): hashlib.sha256(path.read_bytes()).hexdigest() in hashes
                for path in expected_data}
            assert all(result['packagedDataModulesLoaded'].values()), 'CANoe loaded a stale compatibility copy instead of the packaged data modules'
            expected_discovery = [package/'frontends/vxlapi/x86/UcalXlDiscovery.dll',
                                  package/'frontends/vxlapi/x64/UcalXlDiscovery64.dll']
            result['packagedDiscoveryModulesLoaded'] = {
                str(path.relative_to(package)): hashlib.sha256(path.read_bytes()).hexdigest() in hashes
                for path in expected_discovery}
            assert all(result['packagedDiscoveryModulesLoaded'].values()), 'CANoe loaded stale discovery modules instead of the packaged modules'
            if args.fresh_launch_copy:
                pid = win32process.GetWindowThreadProcessId(main_window)[1]
                copy_root = Path(psutil.Process(pid).exe()).resolve().parents[1]
                assert copy_root.is_relative_to(fresh_copy), 'CANoe launched outside the isolated local data directory'
                manifest = json.loads((copy_root/'.ucal_canoe_shadow_manifest.json').read_text(encoding='utf-8'))
                assert manifest.get('completeRuntime') and manifest.get('softwareInstallationId') == software_id, (
                    'First launch did not prepare a complete copy for the selected installation')
                assert all(path.is_relative_to(copy_root) for path in module_paths), 'UCAL loaded a pre-existing launch copy'
                result['preparedLaunchCopy'] = str(copy_root)
            result.update(startupSeconds=round(time.monotonic()-started, 3),
                disclaimerAccepted=True, configuredLogicalChannels=1 if endpoint_id else 2,
                mainTitle=win32gui.GetWindowText(main_window),
                loadedUcalModules=sorted(name for name in modules if 'ucal' in name))
            if endpoint_id:
                result['endpointId'] = endpoint_id
                result['traffic'] = verify_traffic(lifecycle, main_window, configuration,
                                                   peer_channel, args.mode, evidence,
                                                   peer_interface='pcan' if args.pcan_peer_channel else 'vector')
                result['physicalTraffic'] = True
            lifecycle.check()
            lifecycle.set_phase('closing')
            pid = win32process.GetWindowThreadProcessId(main_window)[1]
            close_main_window(main_window, pid)
            lifecycle.wait_for_application_exit()
            functional_complete = True
        except Exception as error:
            result['error'] = f'{type(error).__name__}: {error}'
        finally:
            if not functional_complete:
                lifecycle.terminate_after_failure()
            lifecycle.set_phase('cleanup')
            if manager is not None and manager.poll() is None:
                for handle in windows_for(manager.pid):
                    if win32gui.GetWindowText(handle) == 'UCAL Manager':
                        win32gui.PostMessage(handle, win32con.WM_CLOSE, 0, 0)
                try:
                    manager.wait(10)
                except subprocess.TimeoutExpired:
                    lifecycle.fail('manager-exit-timeout')
                    manager.terminate()
                    manager.wait(5)
            if broker is not None:
                shutdown = subprocess.run([str(runtime/'UcalHarness.exe'), 'shutdown'], env=env,
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
                try:
                    broker.wait(10)
                except subprocess.TimeoutExpired:
                    lifecycle.fail('broker-exit-timeout')
                    broker.terminate()
                    broker.wait(5)
                if shutdown.returncode:
                    lifecycle.fail('broker-shutdown-error', exitCode=shutdown.returncode)
    result['lifecycleFailures'] = lifecycle.failures
    if functional_complete and not lifecycle.failures:
        result.update(status='passed', applicationClosedNormally=True)
    (evidence/'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    print(json.dumps(result))
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
