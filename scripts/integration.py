#!/usr/bin/env python3
"""Real HTTP checks. Each server gets fresh in-memory state."""
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build/bidforge"


def call(url, path, body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url + path, data=data, headers={"Content-Type": "application/json"})
    try:
        response = urllib.request.urlopen(req, timeout=5)
    except urllib.error.HTTPError as error:
        response = error
    with response:
        data = response.read()
        return response.status, json.loads(data) if data else None


class Server:
    def __init__(self, config, mode="indexed", history=10000, workers=8, queue=128, binary=BINARY):
        self.directory = tempfile.TemporaryDirectory()
        path = Path(self.directory.name)
        (path / "campaigns.json").write_text(json.dumps(config))
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        self.url = f"http://127.0.0.1:{port}"
        self.log = (path / "server.log").open("w+")
        self.process = subprocess.Popen([str(binary), "--campaigns", str(path / "campaigns.json"),
            "--port", str(port), "--mode", mode, "--history", str(history), "--workers", str(workers), "--queue", str(queue)], stdout=self.log, stderr=self.log)
        for _ in range(100):
            if self.process.poll() is not None:
                self.log.seek(0)
                raise RuntimeError(self.log.read())
            try:
                if call(self.url, "/health")[0] == 200:
                    return
            except OSError:
                time.sleep(0.02)
        self.close()
        raise RuntimeError("server startup timed out")

    def close(self):
        self.process.terminate()
        self.process.wait(timeout=5)
        self.log.close()
        self.directory.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


def request(identifier="r", category="IAB17", width=300):
    return {"id": identifier, "tmax": 1000, "imp": [{"id": "imp-1", "banner": {"w": width, "h": 250}}], "site": {"cat": [category]}}


def main():
    config = json.loads((ROOT / "examples/campaigns.json").read_text())[:1]
    config[0]["budget_micros"] = 9000
    for mode in ["baseline", "indexed"]:
        with Server(config, mode) as server:
            def bid(_):
                return call(server.url, "/bid", request())
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                replies = list(pool.map(bid, range(32)))
            assert all(reply == replies[0] for reply in replies)
            assert replies[0][0] == 200
            bid_id = replies[0][1]["seatbid"][0]["bid"][0]["id"]
            assert call(server.url, "/stats")[1]["pending"] == 1
            assert call(server.url, "/bid", request(width=301))[0] == 409
            outcome = {"bid_id": bid_id, "outcome": "win"}
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                assert all(status == 200 for status, _ in pool.map(lambda _: call(server.url, "/outcome", outcome), range(32)))
            assert call(server.url, "/outcome", {"bid_id": bid_id, "outcome": "loss"})[0] == 409
            assert call(server.url, "/outcome", {"bid_id": "missing", "outcome": "win"})[0] == 404
            assert call(server.url, "/bid", request("no", "unknown"))[0] == 204
            assert call(server.url, "/bid", {"id": "bad"})[0] == 400
            with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
                responses = list(pool.map(lambda i: call(server.url, "/bid", request(f"pressure-{i}")), range(64)))
            assert sum(status == 200 for status, _ in responses) == 2
            stats = call(server.url, "/stats")[1]
            assert stats["pending"] == 2 and stats["won"] == 1
            assert stats["campaigns"][0]["committed_micros"] == 3000
            assert stats["campaigns"][0]["reserved_micros"] == 6000
            assert stats["invariant_violations"] == 0
            # No expiration timer releases missing feedback, even after other requests.
            assert call(server.url, "/bid", request("exhausted"))[0] == 204
        with Server(config, mode, history=1) as server:
            assert call(server.url, "/bid", request())[0] == 200
            assert call(server.url, "/bid", request("new"))[0] == 503
            assert call(server.url, "/bid", request())[0] == 200
    print("HTTP integration passed: both matching modes, concurrent duplicates, outcomes, pressure, missing feedback, capacity")


if __name__ == "__main__":
    main()
