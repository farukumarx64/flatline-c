"""Exercise worker processes against the real coordinator and controlled peers."""

import argparse
from contextlib import contextmanager
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

import test_ping


def read_output(stream):
    return os.pread(stream.fileno(), os.fstat(stream.fileno()).st_size, 0).decode()


class WorkerProcessTestCase(test_ping.CoordinatorTestCase):
    @contextmanager
    def worker_process(self, port=None, arguments=None, interval_ms=None):
        executable = str((test_ping.BIN_DIR / 'faultline-worker').resolve())
        if arguments is None:
            arguments = ['--coordinator', f'127.0.0.1:{port or self.port}']
        if interval_ms is not None:
            arguments = [*arguments, '--heartbeat-interval-ms', str(interval_ms)]
        with tempfile.TemporaryFile(mode='w+') as output, tempfile.TemporaryFile(mode='w+') as errors:
            process = subprocess.Popen([executable, *arguments], stdout=output, stderr=errors)
            try:
                yield process, output, errors
            finally:
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
                try:
                    # Connect/send helpers can finish their current five-second budget.
                    process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                    self.fail('Worker did not stop after SIGTERM')
                self.assertNotIn('ERROR: AddressSanitizer', read_output(errors))
                self.assertNotIn('runtime error:', read_output(errors))

    def wait_for_registration(self, process, output, errors):
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            match = re.search(r'worker registered worker_id=(\d+)\b', read_output(output))
            if match:
                worker_id = int(match[1])
                self.assertGreater(worker_id, 0)
                return worker_id
            if process.poll() is not None:
                self.fail(f'Worker exited before registering: {read_output(errors)}')
            time.sleep(0.01)
        self.fail(f'Worker did not report registration: {read_output(errors)}')

    def stop_worker(self, process, errors, stop_signal=signal.SIGTERM):
        process.send_signal(stop_signal)
        self.assertEqual(process.wait(timeout=2), 0, read_output(errors))
        self.assertEqual(read_output(errors), '')

    @contextmanager
    def fake_coordinator(self, interval_ms=None):
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            listener.settimeout(3)
            with self.worker_process(listener.getsockname()[1], interval_ms=interval_ms) as (process, output, errors):
                with listener.accept()[0] as connection:
                    connection.settimeout(2)
                    self.assertEqual(test_ping.receive_exact(connection, 12), test_ping.REGISTER)
                    yield connection, process, output, errors

class WorkerExecutableTests(WorkerProcessTestCase):
    def test_two_workers_get_distinct_ids(self):
        with self.worker_process() as (first, first_output, first_errors), \
                self.worker_process() as (second, second_output, second_errors):
            first_id = self.wait_for_registration(first, first_output, first_errors)
            second_id = self.wait_for_registration(second, second_output, second_errors)
            self.assertNotEqual(first_id, second_id)
            self.assertIsNone(first.poll())
            self.assertIsNone(second.poll())
            for worker_id in (first_id, second_id):
                event = self.wait_for_worker_event('worker_registered', worker_id)
                self.assertEqual(event['state'], 'ALIVE')
                self.assertEqual(self.worker_events('worker_dead', worker_id), [])
            print(f'Two running workers registered with IDs {first_id} and {second_id}.', flush=True)
            self.stop_worker(first, first_errors, signal.SIGINT)
            self.wait_for_worker_event('worker_dead', first_id)
            self.assertIsNone(second.poll())
            self.assertEqual(self.worker_events('worker_dead', second_id), [])
            self.assert_pong(self.run_cli())
            self.stop_worker(second, second_errors)
            self.wait_for_worker_event('worker_dead', second_id)

    def test_default_worker_endpoint(self):
        if self.port != 9000:
            self.skipTest('Run with --port 9000 to exercise the default worker endpoint')
        with self.worker_process(arguments=[]) as (process, output, errors):
            worker_id = self.wait_for_registration(process, output, errors)
            self.wait_for_worker_event('worker_registered', worker_id)
            self.stop_worker(process, errors)

    def test_fragmented_registration_ack(self):
        patterns = [(1,) * 16, (3, 5, 4, 4)] + [(cut, 16 - cut) for cut in range(1, 16)]
        for sizes in patterns:
            with self.subTest(sizes=sizes), self.fake_coordinator() as (connection, process, output, errors):
                worker_id = 0x01020304
                frame = test_ping.REGISTER_ACK_HEADER + struct.pack('!I', worker_id)
                offset = 0
                for size in sizes:
                    connection.sendall(frame[offset:offset + size])
                    offset += size
                    if offset < len(frame):
                        self.assert_waiting_for_more(connection)
                        self.assertEqual(read_output(output), '')
                        self.assertIsNone(process.poll())
                self.assertEqual(self.wait_for_registration(process, output, errors), worker_id)
                self.stop_worker(process, errors)
        with self.fake_coordinator() as (connection, process, output, errors):
            connection.sendall(test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 0xffffffff))
            self.assertEqual(self.wait_for_registration(process, output, errors), 0xffffffff)
            self.stop_worker(process, errors)

    def test_invalid_and_truncated_registration_ack(self):
        ack = test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 12)
        for size in range(16):
            with self.subTest(size=size), self.fake_coordinator() as (connection, process, output, errors):
                connection.sendall(ack[:size])
                connection.shutdown(socket.SHUT_WR)
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertEqual(read_output(output), '')
                self.assertIn('registration ACK', read_output(errors))
        invalid = [
            b'NOPE' + ack[4:12],
            ack[:4] + b'\x00\x02' + ack[6:12],
            test_ping.PONG,
            test_ping.HEARTBEAT_HEADER,
            ack[:6] + b'\x00\x0c' + ack[8:12],
            *(ack[:8] + struct.pack('!I', size) for size in (0, 3, 5, 1048577)),
            test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 0),
        ]
        for frame in invalid:
            with self.subTest(frame=frame.hex()), self.fake_coordinator() as (connection, process, output, errors):
                connection.sendall(frame)
                # Keep the peer open: invalid headers must fail before reading a payload.
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertEqual(read_output(output), '')
                self.assertIn('registration ACK', read_output(errors))

    def test_registration_ack_has_one_deadline(self):
        with self.fake_coordinator() as (connection, process, output, errors):
            ack = test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 12)
            start = time.monotonic()
            connection.sendall(ack[:1])
            time.sleep(3)
            connection.sendall(ack[1:14])
            self.assertEqual(process.wait(timeout=3), 1)
            self.assertLess(time.monotonic() - start, 6)
            self.assertEqual(read_output(output), '')
            self.assertIn('worker: receive registration ACK:', read_output(errors))

    def test_stop_during_registration(self):
        for stop_signal in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=stop_signal), self.fake_coordinator() as (connection, process, output, errors):
                connection.sendall(test_ping.REGISTER_ACK_HEADER + b'\x00\x00')
                self.stop_worker(process, errors, stop_signal)
                self.assertEqual(read_output(output), '')

    def test_coordinator_disconnect_and_unexpected_data(self):
        for mode in ('eof', 'reset', 'extra_frame'):
            with self.subTest(mode=mode), self.fake_coordinator() as (connection, process, output, errors):
                ack = test_ping.REGISTER_ACK_HEADER + struct.pack('!I', 12)
                connection.sendall(ack + (test_ping.PONG if mode == 'extra_frame' else b''))
                self.assertEqual(self.wait_for_registration(process, output, errors), 12)
                if mode == 'eof':
                    connection.shutdown(socket.SHUT_WR)
                elif mode == 'reset':
                    connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                    connection.close()
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertIn('worker:', read_output(errors))
                if mode == 'extra_frame':
                    self.assertIn('unexpected data after registration', read_output(errors))

    def test_arguments_and_connection_refused(self):
        with self.worker_process(arguments=['--help']) as (process, output, errors):
            self.assertEqual(process.wait(timeout=2), 0)
            self.assertIn('127.0.0.1:9000', read_output(output))
            self.assertEqual(read_output(errors), '')
        for arguments in (
            ['--coordinator'], ['--unknown'], ['--coordinator', 'localhost:9000'],
            ['--coordinator', '127.0.0.1:0'], ['--coordinator', '127.0.0.1:65536'],
            ['--coordinator', '127.0.0.1'], ['--coordinator', '127.0.0.1:9000', 'extra'],
        ):
            with self.subTest(arguments=arguments), self.worker_process(arguments=arguments) as (process, output, errors):
                self.assertEqual(process.wait(timeout=2), 1)
                self.assertEqual(read_output(output), '')
                self.assertIn('Usage:', read_output(errors))
        with socket.socket() as unused:
            unused.bind(('127.0.0.1', 0))
            with self.worker_process(unused.getsockname()[1]) as (process, output, errors):
                # An unavailable endpoint may time out instead of refusing immediately.
                self.assertEqual(process.wait(timeout=8), 1)
                self.assertEqual(read_output(output), '')
                self.assertIn('worker: connect:', read_output(errors))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    arguments = parser.parse_args()
    test_ping.BIN_DIR = arguments.bin_dir
    test_ping.TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
