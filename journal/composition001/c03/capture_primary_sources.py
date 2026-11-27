#!/usr/bin/env python3
"""Read-only C03 evidence capture: public advisories, patches and OCI metadata.

No image layers are pulled or executed. Anonymous registry tokens stay in memory.
Raw response bodies and selected non-secret headers are retained for independent review.
"""
import datetime
import argparse
import hashlib
import json
from pathlib import Path
import re
import sys
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from dependency_report import MANIFEST_ACCEPT  # noqa: E402

OUT = Path(__file__).resolve().parent / "sources"
RECORDS = []
MAX_BODY_BYTES = 16 * 1024 * 1024


def capture(url, name, headers=None, digest=None):
    request = urllib.request.Request(url, headers={"User-Agent": "scribodo-c03-triage/1", **(headers or {})})
    with urllib.request.urlopen(request, timeout=60) as response:
        body = response.read(MAX_BODY_BYTES + 1)
        if len(body) > MAX_BODY_BYTES:
            raise RuntimeError("Primary-source response exceeds the 16 MiB capture limit")
        selected = {key: response.headers[key] for key in
                    ("Content-Type", "Docker-Content-Digest", "ETag", "Last-Modified", "Date")
                    if key in response.headers}
    actual = "sha256:" + hashlib.sha256(body).hexdigest()
    if digest and actual != digest:
        raise RuntimeError(f"Immutable digest mismatch: {url}: {actual} != {digest}")
    if "Docker-Content-Digest" in selected and selected["Docker-Content-Digest"] != actual:
        raise RuntimeError(f"Registry response digest mismatch: {url}")
    with (OUT / name).open("xb") as file:
        file.write(body)
    RECORDS.append({"url": url, "file": name, "sha256": actual, "bytes": len(body),
                    "retrieved_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                    "headers": selected})
    (OUT / "retrievals.json").write_text(json.dumps(RECORDS, indent=2) + "\n")
    return body


def main():
    global OUT
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True, help="New evidence directory; existing directories refused")
    OUT = parser.parse_args().out
    OUT.mkdir(exist_ok=False)
    for cve in ("CVE-2026-133710", "CVE-2026-134540", "CVE-2026-137057", "CVE-2026-137567"):
        record = json.loads(capture(f"https://api.osv.dev/v1/vulns/{cve}", f"{cve}.json"))
        fixes = sorted({ref["url"].rsplit("/", 1)[-1] for ref in record.get("references", [])
                        if ref.get("type") == "FIX" and re.fullmatch(
                            r"https://github\.com/ggml-org/llama\.cpp/commit/[0-9a-f]{40}", ref.get("url", ""))})
        print(cve, record.get("summary"), "fixes=" + ",".join(fixes), flush=True)
        for commit in fixes:
            capture(f"https://github.com/ggml-org/llama.cpp/commit/{commit}.patch", f"{commit}.patch")
            capture(f"https://api.github.com/repos/ggml-org/llama.cpp/commits/{commit}", f"{commit}-commit.json")

    repository = "library/mariadb"
    query = urllib.parse.urlencode({"service": "registry.docker.io", "scope": f"repository:{repository}:pull"})
    with urllib.request.urlopen("https://auth.docker.io/token?" + query, timeout=60) as response:
        token = json.load(response)["token"]
    headers = {"Authorization": "Bearer " + token, "Accept": MANIFEST_ACCEPT}
    base = f"https://registry-1.docker.io/v2/{repository}"
    pin = re.search(r"image:\s*mariadb:11\.4\.13@(sha256:[0-9a-f]{64})",
                    (ROOT / "docker/compose.yml").read_text()).group(1)
    for label, ref in (("pinned", pin), ("tag", "11.4.13")):
        manifest = json.loads(capture(f"{base}/manifests/{ref}", f"mariadb-{label}.json",
                                      headers, pin if label == "pinned" else None))
        print("mariadb", label, manifest.get("mediaType"), flush=True)
        for child in manifest.get("manifests", []):
            platform = child.get("platform", {})
            arch = platform.get("architecture")
            if platform.get("os") != "linux" or arch not in ("arm64", "amd64"):
                continue
            child_digest = child["digest"]
            child_name = f"mariadb-{label}-{arch}-{child_digest[7:19]}"
            data = json.loads(capture(f"{base}/manifests/{child_digest}", child_name + ".json", headers, child_digest))
            config_digest = data["config"]["digest"]
            config = json.loads(capture(f"{base}/blobs/{config_digest}", child_name + "-config.json", headers, config_digest))
            print(label, arch, "manifest=" + child_digest, "config=" + config_digest,
                  "created=" + str(config.get("created")), "env=" + repr(config.get("config", {}).get("Env", [])),
                  flush=True)


if __name__ == "__main__":
    main()
