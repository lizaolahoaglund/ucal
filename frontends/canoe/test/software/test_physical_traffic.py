"""Run the shared installed-traffic verdict against simulated frames only."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from types import ModuleType, SimpleNamespace
import unittest
from unittest.mock import MagicMock, Mock, patch


class PhysicalTrafficTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ucal-traffic-')
        self.addCleanup(self.temporary.cleanup)
        self.evidence = Path(self.temporary.name)
        self.configuration = self.evidence / 'fixture.cfg'
        self.configuration.touch()
        self.can = ModuleType('can')
        self.can.Message = SimpleNamespace
        self.can.BitTimingFd = SimpleNamespace
        self.can.detect_available_configs = Mock()
        self.can.Bus = MagicMock()
        self.vector = ModuleType('can.interfaces.vector')
        self.vector.get_channel_configs = Mock()
        modules = {
            'can': self.can,
            'can.interfaces.vector': self.vector,
            'psutil': SimpleNamespace(Process=lambda pid: SimpleNamespace(exe=lambda: 'CANoe64.exe')),
            'win32gui': SimpleNamespace(GetWindowText=lambda handle: 'fixture.cfg - CANoe'),
            'win32process': SimpleNamespace(GetWindowThreadProcessId=lambda handle: (1, 42)),
            'pywinauto': SimpleNamespace(Desktop=Mock(side_effect=AssertionError('Unexpected UI access'))),
        }
        self.modules = patch.dict(sys.modules, modules)
        self.modules.start()
        self.addCleanup(self.modules.stop)
        spec = importlib.util.spec_from_file_location(
            'ucal_physical_traffic_test_subject', Path(__file__).with_name('physical_traffic.py'))
        self.subject = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.subject)

    @staticmethod
    def payload(sequence, size):
        return sequence.to_bytes(4, 'little') + bytes(
            0x80 + ((sequence + index) & 0x3f) for index in range(4, size))

    def run_case(self, interface, mode, mutation=None):
        fd = mode == 'fd'
        size = 64 if fd else 8
        frames = [SimpleNamespace(arbitration_id=0x5a5, is_fd=fd, bitrate_switch=fd,
                                  data=bytearray(self.payload(index, size)), timestamp=index + 1)
                  for index in range(16)]
        if mutation is not None:
            mutation(frames)
        channel = 'PCAN_USBBUS3' if interface == 'pcan' else 3
        self.can.detect_available_configs.return_value = [
            {'channel': channel, 'supports_fd': True, 'channel_condition': 1}]
        self.vector.get_channel_configs.return_value = [SimpleNamespace(
            channel_index=3, is_on_bus=False, hw_type=SimpleNamespace(name='XL_HWTYPE_VN1610'))]
        peer = self.can.Bus.return_value.__enter__.return_value
        peer.recv.side_effect = frames + [None]
        peer.send.reset_mock()
        self.can.Bus.return_value.__exit__.reset_mock()
        result_path = self.evidence / 'UcalLifecycle.result'
        if result_path.exists():
            result_path.unlink()

        def measurement(arguments, **unused):
            action = arguments[-1]
            self.assertIn(action, ('Start', 'Stop'))
            if action == 'Stop':
                result_path.write_text('16,0,0,16', encoding='utf-8')
            return SimpleNamespace(returncode=0, stdout=json.dumps({'running': action == 'Start'}), stderr='')

        options = {'peer_interface': 'pcan'} if interface == 'pcan' else {}
        with patch.object(self.subject.subprocess, 'run', side_effect=measurement), \
                patch.object(self.subject.time, 'sleep'), \
                patch.object(self.subject.time, 'monotonic', return_value=0):
            try:
                return self.subject.verify_traffic(
                    SimpleNamespace(check=lambda: None), 1, self.configuration,
                    channel, mode, self.evidence, **options)
            finally:
                self.can.Bus.return_value.__exit__.assert_called_once()

    def assert_rejected(self, mutation):
        for interface in ('vector', 'pcan'):
            for mode in ('classic', 'fd'):
                with self.subTest(interface=interface, mode=mode):
                    with self.assertRaisesRegex(RuntimeError, '^Unexpected CANoe frame:'):
                        self.run_case(interface, mode, mutation)
                    self.can.Bus.return_value.__enter__.return_value.send.assert_not_called()

    def test_valid_sequence_and_default_vector_entry(self):
        for interface in ('vector', 'pcan'):
            for mode in ('classic', 'fd'):
                with self.subTest(interface=interface, mode=mode):
                    result = self.run_case(interface, mode)
                    size = 64 if mode == 'fd' else 8
                    self.assertEqual(result['peerInterface'], interface)
                    self.assertEqual(result['peerRx'], [self.payload(i, size).hex() for i in range(16)])
                    self.assertEqual(result['peerRxTimestamps'], list(range(1, 17)))
                    self.assertEqual(result['applicationRxCounters'], '16,0,0,16')
                    self.assertTrue(result['measurementStopped'])
                    options = self.can.Bus.call_args.kwargs
                    self.assertEqual(options['interface'], interface)
                    if interface == 'vector':
                        self.assertIsNone(options['app_name'])
                    else:
                        self.assertNotIn('app_name', options)
                    sent = self.can.Bus.return_value.__enter__.return_value.send.call_args_list
                    self.assertEqual(len(sent), 16)
                    for index, call in enumerate(sent):
                        frame = call.args[0]
                        self.assertEqual(frame.arbitration_id, 0x5a6)
                        self.assertEqual(bytes(frame.data), self.payload(index, size))
                        self.assertFalse(frame.is_extended_id)
                        self.assertEqual((frame.is_fd, frame.bitrate_switch), (mode == 'fd', mode == 'fd'))

    def test_wrong_payload_is_rejected(self):
        def corrupt(frames):
            frames[0].data[-1] ^= 1
        self.assert_rejected(corrupt)

    def test_out_of_order_sequence_is_rejected(self):
        def reorder(frames):
            frames[0], frames[1] = frames[1], frames[0]
        self.assert_rejected(reorder)

    def test_wrong_frame_flags_are_rejected(self):
        for flag in ('is_fd', 'bitrate_switch'):
            with self.subTest(flag=flag):
                def change_flag(frames):
                    setattr(frames[0], flag, not getattr(frames[0], flag))
                self.assert_rejected(change_flag)

    def test_invalid_timestamps_are_rejected(self):
        for timestamp in (0, -1, float('nan'), float('inf')):
            with self.subTest(timestamp=timestamp):
                self.assert_rejected(lambda frames: setattr(frames[0], 'timestamp', timestamp))


if __name__ == '__main__':
    unittest.main()
