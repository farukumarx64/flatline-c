"""Verify heartbeat cadence, silence detection, and isolation with real processes."""

import argparse
from pathlib import Path
import signal
import socket
import struct
import subprocess
import time
import unittest

import test_ping
from test_worker import WorkerProcessTestCase, read_output


class DefaultHeartbeatTests(WorkerProcessTestCase):
    def test_default_cadence_and_six_second_timeout(self):
        with self.connect() as silent, self.worker_process() as (process, output, errors):
            silent_id = self.register_worker(silent)
            started = time.monotonic()
            worker_id = self.wait_for_registration(process, output, errors)
            silent.settimeout(8)
            self.assert_closed(silent)
            self.assertGreaterEqual(time.monotonic() - started, 5.7)
            dead = self.wait_for_worker_event('worker_dead', silent_id)
            self.assertEqual(dead['reason'], 'heartbeat_timeout')
            deadline = time.monotonic() + 2
            while len(self.worker_events('heartbeat_received', worker_id)) < 3:
                self.assertLess(time.monotonic(), deadline, 'Missing default heartbeats')
                time.sleep(0.02)
            times = [int(self.wait_for_worker_event('worker_registered', worker_id)['last_heartbeat_ms'])]
            times += [int(event['last_heartbeat_ms'])
                      for event in self.worker_events('heartbeat_received', worker_id)[:3]]
            for previous, current in zip(times, times[1:]):
                self.assertGreaterEqual(current - previous, 1900)
                self.assertLess(current - previous, 3500)
            self.assertIsNone(process.poll())
            self.assertEqual(self.worker_events('worker_dead', worker_id), [])
            self.assertIn('heartbeat_interval_ms=2000', read_output(output))
            self.assert_pong(self.run_cli())
            self.stop_worker(process, errors)


class HeartbeatTests(WorkerProcessTestCase):
    COORDINATOR_ARGUMENTS = ('--heartbeat-timeout-ms', '750')

    def test_exact_periodic_frames_only_after_complete_ack(self):
        with self.fake_coordinator(interval_ms=120) as (connection, process, output, errors):
            worker_id = 0x01020304
            ack = test_ping.REGISTER_ACK_HEADER + struct.pack('!I', worker_id)
            connection.sendall(ack[:14])
            # Longer than the configured interval: no heartbeat before the full ID.
            connection.settimeout(0.3)
            with self.assertRaises(socket.timeout):
                connection.recv(1)
            self.assertEqual(read_output(output), '')
            start = time.monotonic()
            connection.sendall(ack[14:])
            connection.settimeout(2)
            for _ in range(3):
                self.assertEqual(test_ping.receive_exact(connection, 16), test_ping.heartbeat(worker_id))
                now = time.monotonic()
                self.assertGreaterEqual(now - start, 0.09)
                self.assertLess(now - start, 1.5)
                start = now
            self.stop_worker(process, errors, signal.SIGINT)

    def test_paused_worker_expires_while_other_worker_keeps_running(self):
        with self.worker_process(interval_ms=80) as (first, first_output, first_errors), \
                self.worker_process(interval_ms=150) as (second, second_output, second_errors):
            first_id = self.wait_for_registration(first, first_output, first_errors)
            second_id = self.wait_for_registration(second, second_output, second_errors)
            time.sleep(1.7)  # Both stay alive through more than two timeout windows.
            for process, worker_id in ((first, first_id), (second, second_id)):
                self.assertIsNone(process.poll())
                self.assertGreater(len(self.worker_events('heartbeat_received', worker_id)), 3)
                self.assertEqual(self.worker_events('worker_dead', worker_id), [])
            first.send_signal(signal.SIGSTOP)
            try:
                dead = self.wait_for_worker_event('worker_dead', first_id)
                self.assertEqual(dead['reason'], 'heartbeat_timeout')
                self.assertEqual(dead['state'], 'DEAD')
                self.assertEqual(len(self.worker_events('worker_dead', first_id)), 1)
                self.assertEqual(self.worker_events('worker_dead', second_id), [])
                self.assertIsNone(second.poll())
                self.assert_pong(self.run_cli())
            finally:
                # A stopped child cannot handle SIGTERM; always resume before cleanup.
                first.send_signal(signal.SIGCONT)
            self.assertEqual(first.wait(timeout=2), 1)
            self.assertIn('worker:', read_output(first_errors))
            with self.worker_process(interval_ms=80) as (replacement, output, errors):
                new_id = self.wait_for_registration(replacement, output, errors)
                self.assertNotIn(new_id, (first_id, second_id))
                self.wait_for_worker_event('heartbeat_received', new_id)
                self.stop_worker(replacement, errors)
            self.stop_worker(second, second_errors)

    def test_complete_heartbeat_renews_deadline(self):
        with self.connect() as connection:
            worker_id = self.register_worker(connection)
            time.sleep(0.45)
            self.send_heartbeat_and_ping(connection, worker_id)
            renewed = time.monotonic()
            heartbeat = self.wait_for_worker_event('heartbeat_received', worker_id)
            time.sleep(0.45)  # Past the original deadline, before the renewed one.
            connection.sendall(test_ping.PING)
            self.assertEqual(test_ping.receive_exact(connection, 12), test_ping.PONG)
            self.assert_closed(connection)
            self.assertGreaterEqual(time.monotonic() - renewed, 0.7)
            dead = self.wait_for_worker_event('worker_dead', worker_id)
            self.assertEqual(dead['reason'], 'heartbeat_timeout')
            self.assertEqual(dead['last_heartbeat_ms'], heartbeat['last_heartbeat_ms'])

    def test_pings_and_partial_heartbeat_do_not_renew_deadline(self):
        for mode in ('ping', 'partial_header', 'partial_payload'):
            with self.subTest(mode=mode), self.connect() as connection:
                worker_id = self.register_worker(connection)
                registered = self.wait_for_worker_event('worker_registered', worker_id)
                frame = test_ping.heartbeat(worker_id)
                if mode == 'partial_payload':
                    connection.sendall(frame[:12])
                for offset in range(3 if mode == 'partial_payload' else 4):
                    time.sleep(0.12)
                    if mode == 'ping':
                        connection.sendall(test_ping.PING)
                        self.assertEqual(test_ping.receive_exact(connection, 12), test_ping.PONG)
                    else:
                        position = offset + (12 if mode == 'partial_payload' else 0)
                        connection.sendall(frame[position:position + 1])
                # Recent traffic at ~360/480ms must not move the ~750ms deadline.
                connection.settimeout(0.6)
                self.assert_closed(connection)
                dead = self.wait_for_worker_event('worker_dead', worker_id)
                self.assertEqual(dead['reason'], 'heartbeat_timeout')
                self.assertEqual(dead['last_heartbeat_ms'], registered['last_heartbeat_ms'])
                self.assertEqual(self.worker_events('heartbeat_received', worker_id), [])

    def test_interval_longer_than_timeout_expires_before_first_heartbeat(self):
        with self.worker_process(interval_ms=1500) as (process, output, errors):
            worker_id = self.wait_for_registration(process, output, errors)
            self.assertEqual(process.wait(timeout=2), 1)
            dead = self.wait_for_worker_event('worker_dead', worker_id)
            self.assertEqual(dead['reason'], 'heartbeat_timeout')
            self.assertEqual(self.worker_events('heartbeat_received', worker_id), [])
            self.assertNotIn('heartbeat_sent', read_output(output))

    def test_late_buffered_heartbeat_cannot_revive_expired_worker(self):
        with self.connect() as connection:
            worker_id = self.register_worker(connection)
            self.server.send_signal(signal.SIGSTOP)
            try:
                time.sleep(0.9)
                connection.sendall(test_ping.heartbeat(worker_id))
            finally:
                self.server.send_signal(signal.SIGCONT)
            self.assert_closed(connection)
            dead = self.wait_for_worker_event('worker_dead', worker_id)
            self.assertEqual(dead['reason'], 'heartbeat_timeout')
            self.assertEqual(self.worker_events('heartbeat_received', worker_id), [])
            self.assert_pong(self.run_cli())

    def test_duration_arguments(self):
        for executable, option in (
            (self.coordinator, '--heartbeat-timeout-ms'),
            (str((test_ping.BIN_DIR / 'faultline-worker').resolve()), '--heartbeat-interval-ms'),
        ):
            for suffix in ([], ['0'], ['-1'], ['+2'], ['1.5'], [' 2'], ['2147483648'],
                           ['9' * 60], ['200', option, '300']):
                with self.subTest(executable=executable, suffix=suffix):
                    result = subprocess.run([executable, option, *suffix],
                                            capture_output=True, text=True, timeout=2)
                    self.assertEqual(result.returncode, 1)
                    self.assertIn('Usage:', result.stderr)
            result = subprocess.run([executable, '--help'], capture_output=True, text=True, timeout=2)
            self.assertEqual(result.returncode, 0)
            self.assertIn(option, result.stdout)
        # Accept the worker's options in the reverse order too.
        with self.worker_process(arguments=['--heartbeat-interval-ms', '80', '--coordinator',
                                            f'127.0.0.1:{self.port}']) as (process, output, errors):
            worker_id = self.wait_for_registration(process, output, errors)
            self.wait_for_worker_event('heartbeat_received', worker_id)
            self.stop_worker(process, errors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    arguments = parser.parse_args()
    test_ping.BIN_DIR = arguments.bin_dir
    test_ping.TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
