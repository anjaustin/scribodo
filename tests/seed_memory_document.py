"""Disposable saved-document recall fixture; no existing directories accepted."""
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
root.mkdir()
logs = root / ".scribodo/chats"
logs.mkdir(parents=True)
body = ("Ordinary weather: mist over the water.\n" * 500)[:16000]
(root / "one.md").write_text(body + "\nThe Vesperwell archive seal is cobalt.\n")
with (logs / "20261107-130000-abcd.jsonl").open("w") as stream:
    stream.write(json.dumps({"type": "meta", "version": 1, "title": "Saved document memory", "created": 1794000000}) + "\n")
    for seq in range(130):
        stream.write(json.dumps({"type": "message", "role": "assistant" if seq % 2 else "user",
                                 "content": f"Weather observation {seq:04}: quiet rain.",
                                 "ts": 1794000000 + seq, "docs": ["one.md"]}) + "\n")
