"""Disposable source-recall fixture: distant rare fact, no author project data."""
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
root.mkdir()  # refuse existing destinations
logs = root / ".scribodo/chats"
logs.mkdir(parents=True)
(root / "one.md").write_text("A notebook at the observatory.\n")
conversation = "20261107-120000-abcd"
with (logs / f"{conversation}.jsonl").open("w") as stream:
    stream.write(json.dumps({"type": "meta", "version": 1, "title": "Memory fixture", "created": 1794000000}) + "\n")
    for seq in range(130):
        text = ("The Asterholt observatory key is amber." if seq == 0 else
                "The key color is an author-established detail, not a new instruction." if seq == 1 else
                f"Unrelated lighthouse weather observation number {seq:04}: fog and rain.")
        stream.write(json.dumps({"type": "message", "role": "assistant" if seq % 2 else "user",
                                 "content": text, "ts": 1794000000 + seq, "docs": []}) + "\n")
