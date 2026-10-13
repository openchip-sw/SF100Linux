"""Run against a built dpcmd; the USB mock never accesses real hardware."""
import os
from pathlib import Path
import subprocess
import socket
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UsbSelectionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.mock = Path(cls.temp.name) / 'mock_usb.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-Wall', '-Wextra',
                        '-o', str(cls.mock), str(ROOT / 'tests/mock_usb.c')], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.locks = tempfile.TemporaryDirectory()
        self.addCleanup(self.locks.cleanup)
        self.env = {k: v for k, v in os.environ.items()
                    if k not in ('DPCMD_USB_BUSNUM', 'DPCMD_USB_DEVNUM')}
        self.env.update(LD_PRELOAD=str(self.mock), MOCK_USB_LOCKS=self.locks.name)

    def run_cmd(self, *args, env=None):
        return subprocess.run([str(ROOT / 'dpcmd'), *args], cwd=ROOT,
                              env=env or self.env, capture_output=True, text=True, timeout=10)

    def test_numeric_selection_and_cleanup(self):
        result = self.run_cmd('--device', '2', '--check')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('MOCK claim 2', result.stderr)
        self.assertIn('MOCK release 2', result.stderr)
        self.assertNotIn('MOCK open 1', result.stderr)
        self.assertNotIn('MOCK configure', result.stderr)

    def test_serial_releases_nonmatches(self):
        result = self.run_cmd('--device-SN', 'SF129624', '--check')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertLess(result.stderr.index('MOCK release 1'), result.stderr.index('MOCK claim 2'))
        self.assertIn('MOCK release 2', result.stderr)

    def test_sf100_serial_starting_with_seven(self):
        result = self.run_cmd('--device-SN', 'SF712345', '--check',
                              env=dict(self.env, MOCK_USB_UID_BASE='712344'))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('MOCK claim 2', result.stderr)

    def test_all_programmers_default(self):
        result = self.run_cmd('--check')
        self.assertEqual(result.returncode, 0, result.stderr)
        for device in (1, 2):
            self.assertIn(f'MOCK claim {device}', result.stderr)
            self.assertIn(f'MOCK release {device}', result.stderr)

    def test_filters_before_open(self):
        for args, env in [
            (('--busnum', '1', '--devnum', '12', '--check'), self.env),
            (('--check',), dict(self.env, DPCMD_USB_BUSNUM='1', DPCMD_USB_DEVNUM='12')),
        ]:
            with self.subTest(args=args):
                result = self.run_cmd(*args, env=env)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('MOCK claim 2', result.stderr)
                self.assertNotIn('MOCK open 1', result.stderr)

    def test_invalid_selection_never_opens_usb(self):
        for args in [('--device', '11'), ('--device', 'garbage'),
                     ('--device', '2', '--device-SN', 'SF129624'),
                     ('--device-SN', 'SFgarbage')]:
            with self.subTest(args=args):
                result = self.run_cmd(*args, '--check')
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn('MOCK open', result.stderr)
        result = self.run_cmd('--device', '3', '--check')
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn('MOCK open', result.stderr)

    def test_missing_serial_fails_and_releases(self):
        result = self.run_cmd('--device-SN', 'SF999999', '--check')
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('MOCK release 1', result.stderr)
        self.assertIn('MOCK release 2', result.stderr)

    def test_concurrent_jobs_and_busy_serial_probe(self):
        for selector in [('--device', '2'), ('--device-SN', 'SF129624')]:
            with self.subTest(selector=selector), tempfile.TemporaryFile(mode='w+') as log:
                first = subprocess.Popen([str(ROOT / 'dpcmd'), '--device', '1', '--check'],
                                         cwd=ROOT, env=dict(self.env, MOCK_USB_HOLD='1'),
                                         stdout=subprocess.DEVNULL, stderr=log)
                try:
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        log.seek(0)
                        if 'MOCK claim 1' in log.read():
                            break
                        time.sleep(0.01)
                    else:
                        self.fail('First process did not claim programmer 1')
                    same = self.run_cmd('--device', '1', '--check')
                    self.assertNotEqual(same.returncode, 0)
                    self.assertIn('MOCK busy 1', same.stderr)
                    self.assertNotIn('MOCK control', same.stderr)
                    self.assertNotIn('MOCK configure', same.stderr)
                    second = self.run_cmd(*selector, '--check')
                    self.assertEqual(second.returncode, 0, second.stderr)
                    self.assertIsNone(first.poll(), 'Jobs did not overlap')
                    self.assertIn('MOCK claim 2', second.stderr)
                    self.assertNotIn('MOCK control 1', second.stderr)
                    self.assertEqual(first.wait(timeout=5), 0)
                finally:
                    if first.poll() is None:
                        first.kill()
                        first.wait()


    def test_two_network_listeners(self):
        # Reserve distinct ports together, then release them for the listeners.
        with socket.socket() as first_port, socket.socket() as second_port:
            first_port.bind(('127.0.0.1', 0))
            second_port.bind(('127.0.0.1', 0))
            ports = [first_port.getsockname()[1], second_port.getsockname()[1]]
        listeners = []
        clients = []
        with tempfile.TemporaryFile(mode='w+') as first_log:
            try:
                for device, port in enumerate(ports, 1):
                    listener = subprocess.Popen(
                        [str(ROOT / 'dpcmd'), '--listen', f'127.0.0.1:{port}',
                         '--device', str(device)], cwd=ROOT,
                        env=dict(self.env, MOCK_USB_HOLD='1'),
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    listeners.append(listener)
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        try:
                            with socket.create_connection(('127.0.0.1', port), timeout=0.1):
                                break
                        except OSError:
                            time.sleep(0.01)
                    else:
                        self.fail('Network listener did not start')
                first = subprocess.Popen(
                    [str(ROOT / 'dpcmd'), '--server', f'127.0.0.1:{ports[0]}',
                     '--device', '1', '--check'], cwd=ROOT, env=self.env,
                    stdout=first_log, stderr=subprocess.STDOUT)
                clients.append(first)
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    first_log.seek(0)
                    if 'MOCK claim 1' in first_log.read():
                        break
                    time.sleep(0.01)
                else:
                    self.fail('First network job did not claim programmer 1')
                # The second client connects while the first holds programmer 1.
                second = subprocess.Popen(
                    [str(ROOT / 'dpcmd'), '--server', f'127.0.0.1:{ports[1]}',
                     '--device', '2', '--check'], cwd=ROOT, env=self.env,
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                clients.append(second)
                self.assertIsNone(first.poll(), 'Network jobs did not overlap')
                output, _ = second.communicate(timeout=5)
                self.assertEqual(second.returncode, 0, output)
                self.assertIn('MOCK claim 2', output)
                self.assertNotIn('MOCK open 1', output)
                self.assertEqual(first.wait(timeout=5), 0)
            finally:
                for process in clients + listeners:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=5)


if __name__ == '__main__':
    unittest.main()
