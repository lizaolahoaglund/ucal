"""Verify a local CANoe measurement fixture against an independent physical peer."""
import json
import math
from pathlib import Path
import subprocess
import sys
import time

import psutil
import win32gui
import win32process


def verify_traffic(lifecycle, main_window, configuration, peer_channel, mode, evidence,
                   *, peer_interface='vector'):
    import can
    from pywinauto import Desktop

    if peer_interface == 'pcan':
        peer_config = [channel for channel in can.detect_available_configs('pcan')
                       if channel['channel'] == peer_channel and channel.get('supports_fd')
                       and channel.get('channel_condition') == 1]
    elif peer_interface == 'vector':
        from can.interfaces.vector import get_channel_configs
        peer_config = [channel for channel in get_channel_configs()
                       if channel.channel_index == peer_channel and not channel.is_on_bus
                       and channel.hw_type.name != 'XL_HWTYPE_VIRTUAL']
    else:
        raise ValueError(f'Unsupported physical peer interface: {peer_interface}')
    if len(peer_config) != 1:
        raise RuntimeError('The requested independent physical peer channel is unavailable')
    pid = win32process.GetWindowThreadProcessId(main_window)[1]
    executable = psutil.Process(pid).exe()
    helper = Path(__file__).with_name('measurement.py')
    result_path = configuration.with_name('UcalLifecycle.result')
    if result_path.exists():
        raise RuntimeError('The CANoe fixture already contains a measurement result')

    def measurement(action):
        lifecycle.check()
        response = subprocess.run([sys.executable, '-B', '-X', 'utf8', str(helper),
                                   '--pid', str(pid), '--executable', executable,
                                   '--action', action], capture_output=True, text=True,
                                  encoding='utf-8', timeout=18)
        with (evidence/'measurement.log').open('a', encoding='utf-8') as log:
            log.write(response.stdout + response.stderr)
        if response.returncode:
            raise RuntimeError(f'CANoe {action} failed: {response.stdout} {response.stderr}')
        return json.loads(response.stdout)

    fd = mode == 'fd'
    size = 64 if fd else 8
    frame_count = 16
    timing = can.BitTimingFd(f_clock=80_000_000, nom_brp=2, nom_tseg1=63, nom_tseg2=16,
                             nom_sjw=16, data_brp=2, data_tseg1=15, data_tseg2=4, data_sjw=4)

    def payload(sequence):
        return sequence.to_bytes(4, 'little') + bytes(0x80 + ((sequence+i) & 0x3f) for i in range(4, size))

    frames = []
    timestamps = []
    with can.Bus(interface=peer_interface, channel=peer_channel, bitrate=500000, fd=fd,
                 **({'app_name': None} if peer_interface == 'vector' else {}),
                 timing=timing if fd else None,
                 can_filters=[{'can_id': 0x5a5, 'can_mask': 0x7ff, 'extended': False}]) as peer:
        if not measurement('Start')['running']:
            raise RuntimeError('The CANoe measurement did not start')
        deadline = time.monotonic() + 10
        while len(frames) < frame_count and time.monotonic() < deadline:
            lifecycle.check()
            frame = peer.recv(timeout=.25)
            if frame is None:
                continue
            if (frame.arbitration_id != 0x5a5 or frame.is_fd != fd or frame.bitrate_switch != fd
                    or bytes(frame.data) != payload(len(frames))
                    or not math.isfinite(frame.timestamp) or frame.timestamp <= 0):
                raise RuntimeError(f'Unexpected CANoe frame: {frame}')
            frames.append(bytes(frame.data).hex())
            timestamps.append(frame.timestamp)
        if len(frames) != frame_count or peer.recv(timeout=.15) is not None:
            raise RuntimeError('The independent physical peer did not receive exactly 16 ordered frames')
        for index in range(frame_count):
            lifecycle.check()
            peer.send(can.Message(arbitration_id=0x5a6, is_extended_id=False,
                                  is_fd=fd, bitrate_switch=fd, data=payload(index)), timeout=1)
            time.sleep(.01)
        time.sleep(.4)
        if measurement('Stop')['running']:
            raise RuntimeError('CANoe was still measuring after Stop')
    counts = result_path.read_text().strip()
    if counts != '16,0,0,16':
        raise RuntimeError(f'Unexpected CANoe receive verdict: {counts}')
    if '*' in win32gui.GetWindowText(main_window):
        # Save the isolated project through the normal UI before closing it.
        window = Desktop(backend='uia').window(handle=main_window)
        window.set_focus()
        window.type_keys('^s', set_foreground=False)
        deadline = time.monotonic() + 5
        while '*' in win32gui.GetWindowText(main_window):
            lifecycle.check()
            if time.monotonic() >= deadline:
                raise RuntimeError('CANoe did not save the isolated configuration')
            time.sleep(.1)
    return {'mode': mode, 'peerInterface': peer_interface, 'peerChannel': peer_channel, 'peerRx': frames,
            'peerRxTimestamps': timestamps,
            'applicationRxCounters': counts, 'measurementStopped': True}
