"""Distinguish connection loss from heartbeat silence using real coordinators."""

import argparse
from pathlib import Path
import signal
import socket
import struct
import time
import unittest

import test_ping
from test_worker import WorkerProcessTestCase, read_output


class FailureTestCase(WorkerProcessTestCase):
    def assert_one_death(self, worker_id, reason):
        dead = self.wait_for_worker_event('worker_dead', worker_id)
        self.assertEqual(dead['state'], 'DEAD')
        self.assertEqual(dead['reason'], reason)
        self.assertEqual(len(self.worker_events('worker_dead', worker_id)), 1)
        return dead

    def assert_transport_death(self, worker_id, started, reason):
        self.assert_one_death(worker_id, reason)
        elapsed = time.monotonic() - started
        # The default heartbeat deadline is six seconds. Connection loss is prompt.
        self.assertLess(elapsed, 2.5)
        self.assertEqual(self.worker_events('heartbeat_timeout', worker_id), [])
        print(f'worker {worker_id}: {reason}, detected within {elapsed:.3f}s of connection loss',
              flush=True)

    def assert_heartbeat_death(self, worker_id, timeout_ms):
        dead = self.assert_one_death(worker_id, 'heartbeat_timeout')
        events = self.worker_events('heartbeat_timeout', worker_id)
        self.assertEqual(len(events), 1)
        timeout = events[0]
        silence = int(timeout['silence_ms'])
        self.assertEqual(int(timeout['timeout_ms']), timeout_ms)
        self.assertEqual(timeout['fd'], dead['fd'])
        self.assertEqual(silence, int(timeout['detected_at_ms']) - int(dead['last_heartbeat_ms']))
        self.assertGreaterEqual(silence, timeout_ms)
        self.assertLess(silence, timeout_ms + 1500)  # Allow OS scheduling slack.
        print(f'worker {worker_id}: heartbeat_timeout, silence={silence}ms, limit={timeout_ms}ms',
              flush=True)
        return dead

    def assert_survivor_is_healthy(self, process, worker_id):
        previous = len(self.worker_events('heartbeat_received', worker_id))
        deadline = time.monotonic() + 2
        while len(self.worker_events('heartbeat_received', worker_id)) <= previous:
            self.assertIsNone(process.poll(), 'Surviving worker exited')
            self.assertLess(time.monotonic(), deadline, 'Surviving worker stopped heartbeating')
            time.sleep(0.01)
        self.assertEqual(self.worker_events('worker_dead', worker_id), [])
        self.assert_pong(self.run_cli())


class ConnectionFailureTests(FailureTestCase):
    def check_worker_exit(self, stop_signal, expected_exit):
        with self.worker_process(interval_ms=200) as (worker, output, errors), \
                self.worker_process(interval_ms=100) as (survivor, survivor_output, survivor_errors):
            worker_id = self.wait_for_registration(worker, output, errors)
            survivor_id = self.wait_for_registration(survivor, survivor_output, survivor_errors)
            self.wait_for_worker_event('heartbeat_received', worker_id)
            self.wait_for_worker_event('heartbeat_received', survivor_id)
            started = time.monotonic()
            worker.send_signal(stop_signal)
            self.assertEqual(worker.wait(timeout=2), expected_exit, read_output(errors))
            self.assertEqual(read_output(errors), '')
            self.assert_transport_death(worker_id, started, 'eof')
            self.assert_survivor_is_healthy(survivor, survivor_id)
            # A freed registration slot must accept a fresh identity.
            with self.connect() as replacement:
                new_id = self.register_worker(replacement)
                self.assertNotIn(new_id, (worker_id, survivor_id))
                self.send_heartbeat_and_ping(replacement, new_id)
            self.assertEqual(len(self.worker_events('worker_dead', worker_id)), 1)
            self.stop_worker(survivor, survivor_errors)

    def test_orderly_worker_exit_is_detected_without_heartbeat_timeout(self):
        self.check_worker_exit(signal.SIGTERM, 0)

    def test_killed_worker_is_detected_without_heartbeat_timeout(self):
        self.check_worker_exit(signal.SIGKILL, -signal.SIGKILL)

    def test_registered_connection_reset_is_detected_without_heartbeat_timeout(self):
        with self.worker_process(interval_ms=100) as (survivor, output, errors):
            survivor_id = self.wait_for_registration(survivor, output, errors)
            with self.connect() as connection:
                worker_id = self.register_worker(connection)
                self.send_heartbeat_and_ping(connection, worker_id)
                # Abort this registered peer's TCP connection, sending a reset.
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                started = time.monotonic()
                connection.close()
            self.assert_transport_death(worker_id, started, 'recv_error')
            self.assert_survivor_is_healthy(survivor, survivor_id)
            self.stop_worker(survivor, errors)

    def test_heartbeats_stop_but_registered_connection_remains_open(self):
        with self.worker_process(interval_ms=100) as (survivor, output, errors), \
                self.connect() as silent:
            survivor_id = self.wait_for_registration(survivor, output, errors)
            worker_id = self.register_worker(silent)
            self.send_heartbeat_and_ping(silent, worker_id)
            time.sleep(0.05)
            self.send_heartbeat_and_ping(silent, worker_id)
            heartbeats = self.worker_events('heartbeat_received', worker_id)
            self.assertEqual(len(heartbeats), 2)
            last_heartbeat = heartbeats[-1]['last_heartbeat_ms']
            # Deliberately send nothing and do not close/shutdown this peer.
            # recv() below verifies the connection stays open before expiry.
            silent.settimeout(1)
            with self.assertRaises(socket.timeout):
                silent.recv(1)
            self.assertEqual(self.worker_events('worker_dead', worker_id), [])
            silent.settimeout(7)
            self.assert_closed(silent)  # The coordinator must initiate closure.
            dead = self.assert_heartbeat_death(worker_id, 6000)
            self.assertEqual(dead['last_heartbeat_ms'], last_heartbeat)
            self.assertEqual(len(self.worker_events('heartbeat_received', worker_id)), 2)
            self.assert_survivor_is_healthy(survivor, survivor_id)
            self.stop_worker(survivor, errors)


class PausedWorkerFailureTests(FailureTestCase):
    COORDINATOR_ARGUMENTS = ('--heartbeat-timeout-ms', '750')

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
                dead = self.assert_heartbeat_death(first_id, 750)
                self.assertIsNone(first.poll(), 'Paused worker unexpectedly exited')
                latest = self.worker_events('heartbeat_received', first_id)[-1]
                self.assertEqual(dead['last_heartbeat_ms'], latest['last_heartbeat_ms'])
                self.assert_survivor_is_healthy(second, second_id)
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
            self.assertEqual(len(self.worker_events('worker_dead', first_id)), 1)
            self.stop_worker(second, second_errors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    arguments = parser.parse_args()
    test_ping.BIN_DIR = arguments.bin_dir
    test_ping.TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
