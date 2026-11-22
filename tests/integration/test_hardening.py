"""Hardening checks for resource and connection limits, using real coordinator/Bean processes on loopback."""
import hashlib
import os
import socket
import threading
import time

from harness import CoffeeTestCase, wait_until


def digest(b):
    return hashlib.sha256(b).hexdigest()


class Uploads(CoffeeTestCase):
    SERVE_EXTRA = ("--max-artifact-bytes", "100000")

    def setUp(self):
        super().setUp()
        self.start_coordinator()

    def leftovers(self):
        tmp = os.path.join(self.state, "artifacts", "tmp")
        found = []
        for d, _, files in os.walk(tmp):
            found += [os.path.join(d, f) for f in files]
        return found

    def test_input_body_over_the_artifact_limit_is_rejected_end_to_end_and_leaves_nothing(self):
        big = os.urandom(300000)
        try:
            st, body = self.api.put("/v1/inputs/" + digest(big), big)[:2]
            self.assertEqual(st, 413, body)
        except (ConnectionError, OSError):
            pass  # the server may close the socket while the client is still sending the rejected body
        self.assertEqual(self.blob_files("inputs"), [])
        self.assertEqual(self.leftovers(), [])
        small = os.urandom(5000)
        self.assertEqual(self.api.put("/v1/inputs/" + digest(small), small)[0], 201)  # service still healthy
        self.assertEqual(self.blob_files("inputs"), [digest(small)])

    def test_declared_length_lie_is_rejected_by_digest(self):
        data = os.urandom(5000)
        st, body = self.api.put("/v1/inputs/" + digest(data[:-1]), data)[:2]
        self.assertEqual(st, 400, body)
        self.assertEqual(self.blob_files("inputs"), [])

    def test_concurrent_identical_uploads_publish_exactly_one_correct_blob(self):
        data = os.urandom(90000)
        d = digest(data)
        codes, errors = [], []

        def put():
            try:
                codes.append(self.api.put("/v1/inputs/" + d, data)[0])
            except Exception as e:  # noqa: BLE001
                errors.append(repr(e))
        threads = [threading.Thread(target=put) for _ in range(12)]
        for t in threads:
            t.start()
        for t in threads:
            t.join(60)
        self.assertEqual(errors, [])
        self.assertTrue(set(codes) <= {200, 201}, codes)
        self.assertGreaterEqual(codes.count(201) + codes.count(200), 12)
        self.assertEqual(self.blob_files("inputs"), [d])
        with open(os.path.join(self.state, "artifacts", "inputs", d), "rb") as f:
            self.assertEqual(f.read(), data)
        self.assertEqual(self.leftovers(), [])


class ConnectionPressure(CoffeeTestCase):
    def setUp(self):
        super().setUp()
        self.start_coordinator()

    def test_idle_connection_flood_does_not_crash_and_service_recovers(self):
        socks = []
        try:
            for _ in range(40):  # more than the 8 base request threads, below the 128-entry queue
                s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                socks.append(s)
            t0 = time.time()
            first = None
            deadline = t0 + 45
            while time.time() < deadline:
                try:
                    if self.api.get("/v1/beans")[0] == 200:
                        first = time.time() - t0
                        break
                except OSError:
                    pass
                time.sleep(0.25)
            print("\nidle-flood: first authenticated success after %s s with 40 idle sockets held" % ("%.2f" % first if first is not None else "NONE"))
        finally:
            for s in socks:
                s.close()
        self.assertTrue(self.coordinator.alive())
        wait_until(lambda: self.api.get("/v1/beans")[0] == 200, 20, what="service responsive after sockets closed")
        self.assertIsNotNone(first, "service did not answer within 45 s while 40 idle sockets were held")


if __name__ == "__main__":
    import unittest
    unittest.main(verbosity=2)
