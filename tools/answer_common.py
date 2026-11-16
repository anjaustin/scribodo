"""Helpers the answer-cycle tools share (AUDIT-004 F12): the repository root, file hashing, the pinned chat model's
GGUF, and the private Unix-socket HTTP client for a llama-server the tool starts itself.

Before 2026-11-15 these were reached through other experiments' modules (memory_causal_parity for ROOT, sha and the
GGUF pin; answer_bedrock_probe for the socket client), so an answer tool loaded a memory probe to learn the
repository root. answer_bedrock_probe now imports them from here; memory_causal_parity keeps its own copies (a
memory tool, outside this family; its recorded runs hash it).

A tool imports it after putting its own directory on sys.path (the tests load tools by file path, so the script
directory is not on the path by itself):

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from answer_common import ROOT, sha  # noqa: E402
"""
from contextlib import contextmanager
import hashlib
import http.client
import json
from pathlib import Path
import socket
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GGUF_REV = "cd0c74c575b5c416f3011b5b4c298077f26ca8c2"
GGUF = Path.home() / ".cache/huggingface/hub/models--LiquidAI--LFM3-1.7B-GGUF/snapshots" / GGUF_REV / "LFM3-1.7B-BF16.gguf"
GGUF_SHA = "39158a79f56edb85e0c4252ce6447b8d7883ef82ffedd02f0cda7c84da5e9f68"


def sha(path):
    """The SHA-256 of a file, read in 1 MiB pieces."""
    h = hashlib.sha256()
    with Path(path).open("rb") as file:
        for chunk in iter(lambda: file.read(1048576), b""):
            h.update(chunk)
    return h.hexdigest()


class UnixHTTP(http.client.HTTPConnection):
    def __init__(self, path, timeout=180):
        super().__init__("localhost", timeout=timeout)
        self.path = str(path)

    def connect(self):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(self.timeout)
        self.sock.connect(self.path)


def call(sock, endpoint, data=None, timeout=180):
    """GET (no data) or POST JSON to a llama-server on a Unix socket; the decoded reply, or RuntimeError."""
    connection = UnixHTTP(sock, timeout=timeout)
    try:
        connection.request("GET" if data is None else "POST", endpoint,
                           body=None if data is None else json.dumps(data).encode(),
                           headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        body = response.read()
        if response.status != 200:
            raise RuntimeError(f"{endpoint}: HTTP {response.status}: {body[:1000]!r}")
        return json.loads(body)
    finally:
        connection.close()


@contextmanager
def private_socket():
    """A socket path in a private temporary directory, short enough for AF_UNIX; removed on exit."""
    preferred = Path(tempfile.gettempdir()) / "opencode"
    with tempfile.TemporaryDirectory(prefix="ab-", dir=preferred if preferred.is_dir() else None) as directory:
        sock = Path(directory) / "s.sock"
        if len(str(sock).encode()) >= 104:
            raise ValueError("Temporary Unix socket path is too long; set a shorter TMPDIR")
        yield sock
