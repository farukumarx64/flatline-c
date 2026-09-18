"""Recover real running jobs after worker failure, using the real CLI and workers."""

import argparse
from pathlib import Path
import re
import signal
import time
import unittest

import test_ping
from test_scheduling import JobProcessTestCase
from test_worker import read_output


class HardCrashRecoveryTests(JobProcessTestCase):
    # Keep the six-second default so prompt disconnect detection cannot be
    # mistaken for an accelerated heartbeat timeout.
    def job_events(self, job_id):
        events = []
        for line in read_output(self.log).splitlines(keepends=True):
            # The coordinator may still be writing a line when we read its log.
            if not line.endswith('\n'):
                continue
            match = re.fullmatch(
                rf'\[INFO\] coordinator (job_\w+) job_id={job_id} ([^\n]*)\n', line)
            if match:
                fields = dict(re.findall(r'(\w+)=("[^"]*"|\S+)', match[2]))
                events.append({'event': match[1], **fields})
        return events

    def wait_transition(self, job_id, event, timeout=2, **fields):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for entry in self.job_events(job_id):
                if entry['event'] == event and all(entry.get(key) == str(value)
                                                 for key, value in fields.items()):
                    return entry
            self.assertIsNone(self.server.poll(), read_output(self.log))
            time.sleep(0.01)
        self.fail(f'Missing {event} for job {job_id} with {fields}:\n{read_output(self.log)}')

    def wait_next_heartbeat(self, worker_id, process):
        previous = len(self.worker_events('heartbeat_received', worker_id))
        deadline = time.monotonic() + 2
        while len(self.worker_events('heartbeat_received', worker_id)) <= previous:
            self.assertIsNone(process.poll(), 'Worker exited while awaiting its next heartbeat')
            self.assertLess(time.monotonic(), deadline, read_output(self.log))
            time.sleep(0.01)

    def test_sigkill_busy_worker_recovers_on_connected_idle_worker(self):
        with self.worker_process(interval_ms=100) as first, \
                self.worker_process(interval_ms=100) as second:
            workers = {}
            for process, output, errors in (first, second):
                worker_id = self.wait_for_registration(process, output, errors)
                self.wait_for_worker_event('heartbeat_received', worker_id)
                workers[worker_id] = (process, output, errors)
            self.assertEqual(len(workers), 2)

            # Both workers are registered BEFORE submission. Discover the owner
            # from STARTED instead of assuming process launch/registration order.
            job_id = self.accepted_id(self.submit_cli('sleep', '--args', '3000',
                                                      '--max-retries', '1'))
            started = self.wait_transition(job_id, 'job_started', state='RUNNING',
                                           attempt=1, retry_count=0)
            owner_id = int(started['worker_id'])
            owner, owner_output, owner_errors = workers[owner_id]
            survivor_id, = workers.keys() - {owner_id}
            survivor, survivor_output, survivor_errors = workers[survivor_id]
            survivor_pid = survivor.pid
            self.assertNotIn('worker job_assigned', read_output(survivor_output))
            # Require activity after STARTED, rather than killing at registration.
            self.wait_next_heartbeat(owner_id, owner)
            self.assertFalse(any(event['event'] == 'job_completed'
                                 for event in self.job_events(job_id)))

            crashed_at = time.monotonic()
            owner.send_signal(signal.SIGKILL)
            self.assertEqual(owner.wait(timeout=2), -signal.SIGKILL, read_output(owner_errors))
            self.assertEqual(read_output(owner_errors), '')
            self.assertNotIn('job_completed_sent', read_output(owner_output))
            self.assertNotIn('job_failed_sent', read_output(owner_output))

            self.wait_transition(job_id, 'job_worker_lost', state='QUEUED', worker_id=0,
                                 attempt=1, retry_count=1, pending=1)
            dead = self.wait_for_worker_event('worker_dead', owner_id)
            self.assertEqual(dead['state'], 'DEAD')
            self.assertIn(dead['reason'], ('eof', 'recv_error', 'truncated_message'))
            self.assertEqual(self.worker_events('heartbeat_timeout', owner_id), [])
            self.wait_transition(job_id, 'job_started', state='RUNNING', worker_id=survivor_id,
                                 attempt=2, retry_count=1)
            recovery_seconds = time.monotonic() - crashed_at
            self.assertLess(recovery_seconds, 2.5)  # Comfortably below the 6 s heartbeat limit.
            self.assertIsNone(survivor.poll())
            self.assertEqual(survivor.pid, survivor_pid)
            self.assertEqual(len(self.worker_events('worker_registered', survivor_id)), 1)
            self.assert_pong(self.run_cli())

            self.wait_transition(job_id, 'job_completed', timeout=6, state='DONE',
                                 worker_id=survivor_id, attempt=2, retry_count=1,
                                 pending=0, result_bytes=13, result='"slept_ms=3000"')

            # The surviving process must also be free for a new, unrelated job.
            followup = self.accepted_id(self.submit_cli('fibonacci', '--args', '10'))
            self.assertNotEqual(followup, job_id)
            self.wait_transition(followup, 'job_completed', state='DONE', worker_id=survivor_id,
                                 attempt=1, retry_count=0, pending=0, result_bytes=2, result='"55"')
            self.wait_next_heartbeat(survivor_id, survivor)

            # One job ID, exactly one requeue, two assignments/starts, and one
            # accepted completion: no duplicate or missing transition can pass.
            transitions = [(entry['event'], entry['state'], entry['worker_id'],
                            entry['attempt'], entry['retry_count'])
                           for entry in self.job_events(job_id)]
            self.assertEqual(transitions, [
                ('job_submitted', 'QUEUED', '0', '0', '0'),
                ('job_assigned', 'ASSIGNED', str(owner_id), '1', '0'),
                ('job_started', 'RUNNING', str(owner_id), '1', '0'),
                ('job_worker_lost', 'QUEUED', '0', '1', '1'),
                ('job_assigned', 'ASSIGNED', str(survivor_id), '2', '1'),
                ('job_started', 'RUNNING', str(survivor_id), '2', '1'),
                ('job_completed', 'DONE', str(survivor_id), '2', '1'),
            ])
            self.assertEqual(len(self.worker_events('worker_dead', owner_id)), 1)
            self.assertEqual(self.worker_events('worker_dead', survivor_id), [])
            self.assert_pong(self.run_cli())
            print(f'job {job_id}: SIGKILL worker {owner_id}; connected worker {survivor_id} '
                  f'started attempt 2 within {recovery_seconds:.3f}s; '
                  f'result=slept_ms=3000; follow-up job {followup}=55', flush=True)
            self.stop_worker(survivor, survivor_errors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin-dir', type=Path, default=Path('build/debug'))
    parser.add_argument('--port', type=int, default=None)
    arguments = parser.parse_args()
    test_ping.BIN_DIR = arguments.bin_dir
    test_ping.TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
