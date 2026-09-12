"""Exercise the real binaries with independent Python TCP peers (stdlib only)."""

import argparse
import concurrent.futures
from pathlib import Path
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest


PING = bytes.fromhex("46 4c 49 4e 00 01 00 01 00 00 00 00")
PONG = bytes.fromhex("46 4c 49 4e 00 01 00 02 00 00 00 00")
# Exercise field boundaries and splits inside each field, in both directions.
FRAGMENT_SIZES = [(1,) * 12, (3, 5, 4)] + [(cut, 12 - cut) for cut in range(1, 12)]
BIN_DIR = Path("build/debug")
TEST_PORT = None


def receive_exact(connection, size):
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise AssertionError(f"EOF after {len(result)} of {size} bytes")
        result.extend(chunk)
    return bytes(result)


class PingExchangeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cli = str((BIN_DIR / "faultline").resolve())
        cls.coordinator = str((BIN_DIR / "faultline-coordinator").resolve())
        cls.log = tempfile.TemporaryFile(mode="w+")
        cls.addClassCleanup(cls.log.close)
        if TEST_PORT is None:
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                cls.port = reservation.getsockname()[1]
        else:
            cls.port = TEST_PORT
        # Port 9000 also exercises the coordinator's no-argument default.
        command = [cls.coordinator]
        if cls.port != 9000:
            command.extend(["--port", str(cls.port)])
        cls.server = subprocess.Popen(
            command,
            stdout=cls.log, stderr=cls.log,
        )
        cls.addClassCleanup(cls.stop_server)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if cls.server.poll() is not None:
                cls.log.seek(0)
                raise AssertionError(f"Coordinator exited:\n{cls.log.read()}")
            try:
                with socket.create_connection(("127.0.0.1", cls.port), timeout=0.2):
                    return
            except OSError:
                time.sleep(0.02)
        raise AssertionError("Coordinator did not become ready in 5 seconds")

    @classmethod
    def stop_server(cls):
        if cls.server.poll() is None:
            cls.server.send_signal(signal.SIGTERM)
        try:
            code = cls.server.wait(timeout=3)
        except subprocess.TimeoutExpired:
            cls.server.kill()
            cls.server.wait()
            raise AssertionError("Coordinator did not stop on SIGTERM")
        cls.log.seek(0)
        output = cls.log.read()
        if code != 0 or "ERROR: AddressSanitizer" in output or "runtime error:" in output:
            raise AssertionError(f"Coordinator failed ({code}):\n{output}")

    def connect(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=2)

    def run_cli(self, port=None):
        return subprocess.run(
            [self.cli, "ping", "--coordinator", f"127.0.0.1:{port or self.port}"],
            capture_output=True, text=True, timeout=8,
        )

    def assert_pong(self, result):
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "PONG\n")
        self.assertEqual(result.stderr, "")

    def assert_closed(self, connection):
        try:
            self.assertEqual(connection.recv(1), b"")
        except ConnectionResetError:
            pass

    def assert_waiting_for_more(self, connection):
        previous_timeout = connection.gettimeout()
        connection.settimeout(0.05)
        try:
            # Data, EOF, or reset all fail: an incomplete frame must stay pending.
            with self.assertRaises(socket.timeout, msg="Peer acted on an incomplete header"):
                connection.recv(1)
        finally:
            connection.settimeout(previous_timeout)

    def send_fragments(self, connection, frame, sizes):
        self.assertEqual(sum(sizes), len(frame))
        offset = 0
        for size in sizes:
            connection.sendall(frame[offset:offset + size])
            offset += size
            if offset < len(frame):
                # Withhold the suffix; separate send calls alone can coalesce.
                self.assert_waiting_for_more(connection)

    def fake_peer(self, reply):
        """Run the CLI against a peer with deliberately controlled replies."""
        errors = []
        finished = threading.Event()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(8)
            port = listener.getsockname()[1]

            def serve():
                try:
                    with listener.accept()[0] as connection:
                        connection.settimeout(7)
                        if receive_exact(connection, 12) != PING:
                            raise AssertionError("CLI did not send the exact PING bytes")
                        reply(connection, finished)
                except Exception as error:
                    errors.append(error)

            thread = threading.Thread(target=serve, daemon=True)
            thread.start()
            try:
                result = self.run_cli(port)
            finally:
                finished.set()
                thread.join(timeout=8)
            self.assertFalse(thread.is_alive(), "Fake peer did not finish")
            if errors:
                raise errors[0]
            return result

    def test_cli_exchange_and_sequential_clients(self):
        for _ in range(3):
            self.assert_pong(self.run_cli())

    def test_cli_default_endpoint(self):
        if self.port != 9000:
            self.skipTest("Run with --port 9000 to exercise the default CLI endpoint")
        result = subprocess.run(
            [self.cli, "ping"], capture_output=True, text=True, timeout=8,
        )
        self.assert_pong(result)

    def test_fragmented_ping(self):
        with self.connect() as connection:
            for sizes in FRAGMENT_SIZES:
                with self.subTest(sizes=sizes):
                    self.send_fragments(connection, PING, sizes)
                    self.assertEqual(receive_exact(connection, 12), PONG)

    def test_coalesced_and_repeated_frames(self):
        with self.connect() as connection:
            connection.sendall(PING * 3)
            self.assertEqual(receive_exact(connection, 36), PONG * 3)
            connection.sendall(PING)
            self.assertEqual(receive_exact(connection, 12), PONG)

    def test_complete_frame_followed_by_partial_frame(self):
        with self.connect() as connection:
            connection.sendall(PING + PING[:5])
            self.assertEqual(receive_exact(connection, 12), PONG)
            self.assert_waiting_for_more(connection)
            self.send_fragments(connection, PING[5:], (3, 4))
            self.assertEqual(receive_exact(connection, 12), PONG)
            connection.sendall(PING)
            self.assertEqual(receive_exact(connection, 12), PONG)

    def test_idle_and_partial_clients_do_not_block_others(self):
        with self.connect(), self.connect() as partial:
            partial.sendall(PING[:3])
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                results = list(pool.map(lambda _: self.run_cli(), range(4)))
            for result in results:
                self.assert_pong(result)

    def test_half_close_still_receives_pong(self):
        with self.connect() as connection:
            connection.sendall(PING)
            connection.shutdown(socket.SHUT_WR)
            self.assertEqual(receive_exact(connection, 12), PONG)
            self.assert_closed(connection)

    def test_truncated_request_does_not_stop_server(self):
        for size in range(len(PING)):
            with self.subTest(size=size), self.connect() as connection:
                connection.sendall(PING[:size])
                connection.shutdown(socket.SHUT_WR)
                self.assert_closed(connection)
        self.assert_pong(self.run_cli())

    def test_invalid_requests_are_rejected(self):
        requests = [
            b"NOPE" + PING[4:],
            PING[:4] + b"\x00\x02" + PING[6:],
            PING[:6] + b"\x00\x06" + PING[8:],
            PING[:6] + b"\x00\x03" + PING[8:],  # Known, but not a PING request.
            PONG,
            PING[:8] + struct.pack("!I", 1),
            PING[:8] + struct.pack("!I", 1048577),
        ]
        for request in requests:
            with self.subTest(request=request.hex()), self.connect() as connection:
                connection.sendall(request)
                self.assert_closed(connection)
        self.assert_pong(self.run_cli())

    def test_reset_during_reply_does_not_stop_server(self):
        with self.connect() as connection:
            connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            connection.sendall(PING)
        self.assert_pong(self.run_cli())

    def test_cli_accepts_fragmented_pong(self):
        for sizes in FRAGMENT_SIZES:
            with self.subTest(sizes=sizes):
                self.assert_pong(self.fake_peer(
                    lambda connection, _done: self.send_fragments(connection, PONG, sizes)
                ))

    def test_cli_rejects_bad_and_incomplete_replies(self):
        responses = [PONG[:size] for size in range(len(PONG))]
        responses.extend((PING, b"NOPE" + PONG[4:], PONG[:8] + struct.pack("!I", 1)))
        for response in responses:
            with self.subTest(response=response.hex()):
                result = self.fake_peer(lambda connection, _done: connection.sendall(response))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertTrue(result.stderr.startswith("faultline:"), result.stderr)

    def test_cli_receive_timeout(self):
        result = self.fake_peer(lambda _connection, done: done.wait(timeout=7))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(result.stdout, "")
        self.assertIn("faultline: receive PONG:", result.stderr)

    def test_connection_refused(self):
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
            result = self.run_cli(unused.getsockname()[1])
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("faultline: connect:", result.stderr)

    def test_port_conflict_and_bad_arguments(self):
        for command in (
            [self.coordinator, "--port", str(self.port)],
            [self.coordinator, "--port", "65536"],
            [self.cli, "ping", "--coordinator", "127.0.0.1:0"],
            [self.cli, "ping", "--coordinator", "invalid:9000"],
        ):
            with self.subTest(command=command):
                result = subprocess.run(command, capture_output=True, text=True, timeout=3)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertTrue(result.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--bin-dir", type=Path, default=BIN_DIR)
    parser.add_argument("--port", type=int, default=None)
    arguments = parser.parse_args()
    BIN_DIR = arguments.bin_dir
    TEST_PORT = arguments.port
    unittest.main(argv=[__file__], verbosity=2)
