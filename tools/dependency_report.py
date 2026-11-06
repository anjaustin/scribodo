#!/usr/bin/env python3
"""Report pinned submodules, container images, and known commit advisories."""

from __future__ import annotations

import argparse
import configparser
import json
import re
import subprocess
import sys
import urllib.parse
import urllib.request
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
USER_AGENT = "scribodo-dependency-report/1"
MANIFEST_ACCEPT = ", ".join(
    (
        "application/vnd.oci.image.index.v1+json",
        "application/vnd.oci.image.manifest.v1+json",
        "application/vnd.docker.distribution.manifest.list.v2+json",
        "application/vnd.docker.distribution.manifest.v2+json",
    )
)


def run(*args: str, cwd: Path = ROOT) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.STDOUT).strip()


def request_json(url: str, data: dict | None = None, headers: dict[str, str] | None = None) -> tuple[dict, object]:
    body = json.dumps(data).encode() if data is not None else None
    request_headers = {"User-Agent": USER_AGENT, **(headers or {})}
    if body is not None:
        request_headers["Content-Type"] = "application/json"
    request = urllib.request.Request(url, data=body, headers=request_headers)
    with urllib.request.urlopen(request, timeout=30) as response:
        return json.load(response), response.headers


def cell(value: object) -> str:
    return str(value).replace("|", "\\|").replace("\n", " ")


def submodules(offline: bool) -> tuple[list[list[str]], list[dict], list[str]]:
    config = configparser.ConfigParser()
    config.read(ROOT / ".gitmodules")
    rows: list[list[str]] = []
    advisories: list[dict] = []
    errors: list[str] = []
    for section in config.sections():
        path = config[section]["path"]
        url = config[section]["url"]
        checkout = ROOT / path
        try:
            current = run("git", "rev-parse", "HEAD", cwd=checkout)
            label = run("git", "describe", "--tags", "--always", cwd=checkout)
        except (OSError, subprocess.CalledProcessError) as exc:
            errors.append(f"{path}: cannot read checked-out pin ({exc})")
            rows.append([path, "error", "error", "error"])
            continue

        upstream = "not checked"
        status = "offline"
        if not offline:
            try:
                upstream = run("git", "ls-remote", url, "HEAD").split()[0]
                status = "current" if upstream == current else "update available"
            except (OSError, subprocess.CalledProcessError, IndexError) as exc:
                errors.append(f"{path}: cannot resolve upstream HEAD ({exc})")
                upstream = "error"
                status = "error"
            try:
                result, _ = request_json("https://api.osv.dev/v1/query", {"commit": current})
                for advisory in result.get("vulns", []):
                    advisories.append(
                        {
                            "dependency": path,
                            "id": advisory.get("id", "unknown"),
                            "summary": advisory.get("summary", "No summary supplied"),
                        }
                    )
            except Exception as exc:  # network/protocol errors must make the scheduled report visible
                errors.append(f"{path}: OSV query failed ({exc})")
        rows.append([path, f"`{label}` / `{current[:12]}`", f"`{upstream[:12]}`" if upstream not in ("error", "not checked") else upstream, status])
    return rows, advisories, errors


def docker_digest(repository: str, tag: str) -> str:
    query = urllib.parse.urlencode(
        {"service": "registry.docker.io", "scope": f"repository:{repository}:pull"}
    )
    auth, _ = request_json(f"https://auth.docker.io/token?{query}")
    _, headers = request_json(
        f"https://registry-1.docker.io/v2/{repository}/manifests/{tag}",
        headers={"Authorization": f"Bearer {auth['token']}", "Accept": MANIFEST_ACCEPT},
    )
    digest = headers.get("Docker-Content-Digest")
    if not digest:
        raise RuntimeError("registry response omitted Docker-Content-Digest")
    return digest


def images(offline: bool) -> tuple[list[list[str]], list[str]]:
    compose = (ROOT / "docker/compose.yml").read_text()
    refs = re.findall(r"^\s*image:\s*([^\s#]+)", compose, flags=re.MULTILINE)
    rows: list[list[str]] = []
    errors: list[str] = []
    for ref in refs:
        match = re.fullmatch(r"([^:@]+(?:/[^:@]+)*):([^@]+)@(sha256:[0-9a-f]{64})", ref)
        if not match:
            errors.append(f"container image is not pinned by tag and sha256 digest: {ref}")
            rows.append([ref, "invalid", "invalid"])
            continue
        image, tag, pinned = match.groups()
        repository = image if "/" in image else f"library/{image}"
        resolved = "not checked"
        status = "offline"
        if not offline:
            try:
                resolved = docker_digest(repository, tag)
                status = "verified" if resolved == pinned else "tag moved"
                if resolved != pinned:
                    errors.append(f"{image}:{tag} resolves to {resolved}, expected {pinned}")
            except Exception as exc:
                errors.append(f"{image}:{tag}: registry lookup failed ({exc})")
                resolved = "error"
                status = "error"
        rows.append([f"`{image}:{tag}`", f"`{pinned[:19]}…`", f"{status} (`{resolved[:19]}…`)" if resolved.startswith("sha256:") else status])
    return rows, errors


def render(offline: bool) -> tuple[str, bool]:
    module_rows, advisories, errors = submodules(offline)
    image_rows, image_errors = images(offline)
    errors.extend(image_errors)
    lines = ["# Scribodo dependency report", "", "## Git submodules", "", "| Path | Pin | Upstream HEAD | Status |", "|---|---|---|---|"]
    lines.extend("| " + " | ".join(cell(value) for value in row) + " |" for row in module_rows)
    lines.extend(["", "## Container images", "", "| Image | Pinned digest | Status |", "|---|---|---|"])
    lines.extend("| " + " | ".join(cell(value) for value in row) + " |" for row in image_rows)
    lines.extend(["", "## OSV commit advisories", ""])
    if advisories:
        lines.extend(["| Dependency | Advisory | Summary |", "|---|---|---|"])
        for advisory in advisories:
            advisory_id = cell(advisory["id"])
            lines.append(
                f"| {cell(advisory['dependency'])} | [{advisory_id}](https://osv.dev/vulnerability/{advisory_id}) | {cell(advisory['summary'])} |"
            )
    else:
        lines.append("No advisories were returned for the pinned submodule commits." if not offline else "Not checked in offline mode.")
    lines.extend(["", "## Report health", ""])
    if errors:
        lines.extend(f"- **Error:** {cell(error)}" for error in errors)
    elif advisories:
        lines.append(f"**{len(advisories)} confirmed advisory result(s) require review.**")
    else:
        lines.append("All pins and remote checks completed successfully." if not offline else "Local pin structure is valid; remote checks were skipped.")
    lines.extend(
        [
            "",
            "> Update availability is informational. This report fails only for invalid or moved pins, lookup failures, or confirmed advisories.",
            "",
        ]
    )
    return "\n".join(lines), bool(errors or advisories)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--offline", action="store_true", help="validate local inventory without network requests")
    parser.add_argument("--output", type=Path, help="write Markdown to this path instead of stdout")
    args = parser.parse_args()
    report, failed = render(args.offline)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(report)
    else:
        print(report, end="")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
