"""Create a disposable legacy project for the store UI window regression."""
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
root.mkdir()  # refuse to overwrite an existing project
logs = root / ".scribodo/chats"
logs.mkdir(parents=True)
(root / "one.md").write_text("The lantern in the fog.\n")
conversation = "20261106-180000-abcd"
large = "Zoë Ångström in fog. " * 3000
with (logs / f"{conversation}.jsonl").open("w") as stream:
    stream.write(json.dumps({"type": "meta", "version": 1, "title": "Window fixture", "created": 1794000000}) + "\n")
    for i in range(400):
        message = {"type": "message", "role": "assistant" if i % 2 else "user",
                   "content": large if i == 101 else f"message {i:04}", "ts": 1794000000 + i}
        if i == 398:
            message["docs"] = ["one.md"]
        stream.write(json.dumps(message) + "\n")
note = {"id": "n-20261106-180000-abcd", "ver": 1, "ts": 1794000500, "status": "active",
        "content": "Old source in a large message", "sources": [
            {"kind": "chat", "chat": conversation, "message": 101, "start": 9000, "len": 20, "quote": large[9000:9020]}]}
(root / ".scribodo/notes.jsonl").write_text(json.dumps(note) + "\n")
