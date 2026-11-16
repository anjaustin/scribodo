#!/usr/bin/env python3
"""Every build/ path a dated document cites is archived, declared not evidence, or recorded as lost (AUDIT-004 F1).

Scans docs/*.md, journal/*.md and README.md for cited build/ paths and checks each against:

  (a) the evidence archives in the archive folder (docs/EVIDENCE.md): a tarball's member listing, or, for an
      archive whose tarball is missing, its manifest (top-level names without "build/" or full build/ paths);
  (b) tests/fixtures/evidence_not_archived.txt: paths that are not evidence (make's own directories, binaries,
      tool outputs named in prose, locked environments), each with a reason;
  (c) tests/fixtures/evidence_lost.txt: paths that existed nowhere when AUDIT-004 ran, each with the citing doc.

A concrete path under a directory a gate deletes (gate_owned_dirs) must be archived or recorded as lost; the
not-evidence list cannot excuse it, because the next run of the gate erases it. Placeholder citations
(build/ui/<script>/, build/audit004/<target>.log) and globs are families: covered when an archive holds a member
that matches, or when the not-evidence list names them.

Without the archive folder (hosted CI) the check is skipped, and says so. Exit 1 lists every violation.

    tools/evidence_check.py [--root DIR] [--evidence DIR] [--build DIR] [--list]
"""
import argparse
import fnmatch
import os
import re
import sys
import tarfile
from pathlib import Path

DEFAULT_EVIDENCE = Path.home() / "Library/Application Support/Scribodo/evidence"
NOT_ARCHIVED = "tests/fixtures/evidence_not_archived.txt"
LOST = "tests/fixtures/evidence_lost.txt"

# A citation starts at "build/" not preceded by a path or identifier character ("rebuild/", "$(BUILD)/" and
# "foo/build/" are not citations) and runs to whitespace, a backtick or a quote.
CITE = re.compile(r"(?<![A-Za-z0-9_./$-])build/[^\s`'\"|]*")
TRAILING = ".,;:)]!?"


def gate_owned_dirs(root):
    """Directories a make target deletes before it writes, or in make clean: build/src, build/tests and build/asan,
    one build/ui/<script> per UI script (uitest, uitest-real), the store, backup, offline and remote suites',
    cstest's volume and make mutationtest's build/mutationtest (Makefile UI_OWNED and clean; tools/*test.sh). make distclean removes all of build/ and is the
    one explicit exception: run it only after an archive pass. tests/test_evidence_check.py pins this list."""
    owned = ["build/src", "build/tests", "build/asan", "build/csvol", "build/cs.sparseimage", "build/mutationtest",
             "build/ui/store", "build/ui/backup", "build/ui/offline", "build/ui/remote"]
    for script in sorted((Path(root) / "tests/ui").glob("*.scribo")):
        if script.stem != "case_sensitive":  # cstest runs it on its own volume (build/csvol)
            owned.append("build/ui/" + script.stem)
    return owned


def expand_braces(text):
    m = re.search(r"\{([^{}]*)\}", text)
    if not m:
        return [text]
    out = []
    for alt in m.group(1).split(","):
        out.extend(expand_braces(text[:m.start()] + alt + text[m.end():]))
    return out


def normalize(raw):
    """A cited token to its paths: trailing punctuation and slashes dropped, braces expanded."""
    s = raw
    while s and s[-1] in TRAILING + "/":
        # Keep a closing bracket or paren that belongs to the path ("(a)" inside a name is not cited here).
        if s[-1] == ")" and s.count("(") >= s.count(")"):
            break
        s = s[:-1]
    if "{" in s and "}" not in s:
        return []
    m = re.search(r"([A-Za-z_-])(\d+)\.\.(\d+)$", s)  # build/ux1..3: prose for ux1, ux2, ux3
    if m and int(m.group(2)) < int(m.group(3)) <= int(m.group(2)) + 50:
        head = s[:m.start(2)]
        s = "{%s}" % ",".join(str(i) for i in range(int(m.group(2)), int(m.group(3)) + 1))
        s = head + s
    paths = []
    for p in expand_braces(s):
        p = p.rstrip("/")
        while p and p[-1] in TRAILING:
            p = p[:-1]
        if p and p != "build" and p.startswith("build/"):
            paths.append(p)
    return paths


def is_pattern(path):
    return any(c in path for c in "*?[<")


def pattern_regex(path):
    out = []
    i = 0
    while i < len(path):
        c = path[i]
        if c == "<":
            j = path.find(">", i)
            if j < 0:
                out.append(re.escape(c))
            else:
                out.append(r"[^/]+")
                i = j
        elif c == "*":
            out.append(r"[^/]*")
        elif c == "?":
            out.append(r"[^/]")
        else:
            out.append(re.escape(c))
        i += 1
    return re.compile("".join(out) + r"(/.*)?$")


# A relative name in backticks ("`gates-exit.txt`", "`restart-real-2/*`") inside a document that also cites a
# directory: the file it names under that directory, when one exists there (journals cite a run directory once and
# its files by name after that).
RELATIVE = re.compile(r"`([A-Za-z0-9_.*?{},/-]+)`")


def citations(root, build=None):
    """(path, "file:line") for every cited build/ path, in file order, then each relative name that resolves to an
    existing file under a directory the same document cites."""
    root = Path(root)
    build = Path(build) if build else default_build(root)
    allow = load_list(root / NOT_ARCHIVED, 2)
    files = sorted(root.glob("docs/*.md")) + sorted(root.glob("journal/*.md"))
    if (root / "README.md").exists():
        files.append(root / "README.md")
    found = []
    for f in files:
        lines = f.read_text(errors="replace").splitlines()
        cited_dirs = []
        for n, line in enumerate(lines, 1):
            for m in CITE.finditer(line):
                for p in normalize(m.group(0)):
                    found.append((p, "%s:%d" % (f.relative_to(root), n)))
                    if not is_pattern(p) and (build / p[len("build/"):]).is_dir() and p not in cited_dirs \
                            and not listed(p, allow):  # not a checkout or build tree named in prose
                        cited_dirs.append(p)
        if not cited_dirs:
            continue
        for n, line in enumerate(lines, 1):
            for m in RELATIVE.finditer(line):
                token = m.group(1)
                if token.startswith(("build/", "/", "-", ".")) or not any(c in token for c in "./*"):
                    continue
                for t in expand_braces(token.rstrip(TRAILING)):
                    for d in cited_dirs:
                        candidate = d + "/" + t.rstrip("/")
                        if candidate != d and denoted_files(candidate, build):
                            found.append((candidate, "%s:%d" % (f.relative_to(root), n)))
    return found


def default_build(root):
    """The build/ that holds the evidence: the main checkout's, also from a git worktree (whose own build/ is nearly
    empty), found through git's common directory; ROOT/build outside git."""
    import subprocess
    try:
        common = subprocess.run(["git", "-C", str(root), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                                capture_output=True, text=True, timeout=10)
        if common.returncode == 0 and common.stdout.strip():
            return Path(common.stdout.strip()).parent / "build"
    except (OSError, subprocess.SubprocessError):
        pass
    return Path(root) / "build"


def denoted_files(path, build):
    """The existing files a citation denotes under build: the file itself, every file under a directory, every file
    a glob or placeholder matches. Binaries, objects, debug symbols and sockets are left out."""
    rel = path[len("build/"):]
    tops = [build / rel] if not is_pattern(path) else sorted(build.glob(re.sub(r"<[^>]*>", "*", rel)))
    out = []
    for top in tops:
        files = [top] if top.is_file() else (sorted(top.rglob("*")) if top.is_dir() else [])
        out.extend(f for f in files if not f.is_dir() and not is_artifact(f))
    return out


def norm_member(name):
    name = name.strip()
    if name.startswith("./"):
        name = name[2:]
    name = name.rstrip("/")
    if not name:
        return None
    return name if name.startswith("build/") else "build/" + name


def load_archives(evidence):
    """{archive name: (set of member paths, whether they are exact)}: a tarball's listing is exact; a manifest
    without its tarball lists entries whose descendants count as archived."""
    evidence = Path(evidence)
    archives = {}
    for tgz in sorted(evidence.glob("*.tar.gz")):
        with tarfile.open(tgz) as t:
            members = {m for m in (norm_member(n) for n in t.getnames()) if m}
        archives[tgz.name] = (members, True)
    for man in sorted(evidence.glob("manifest-*.txt")):
        stem = man.name[len("manifest-"):-len(".txt")]
        name = stem if stem.startswith("scribodo-evidence-") else "scribodo-evidence-" + stem
        if name + ".tar.gz" in archives:
            continue
        members = {m for m in (norm_member(l) for l in man.read_text().splitlines()) if m}
        archives[name + ".tar.gz"] = (members, False)
    return archives


class Index:
    def __init__(self, archives):
        self.exact = set()
        self.prefixes = set()  # manifest-only entries: descendants are archived
        for members, exact in archives.values():
            self.exact |= members
            if not exact:
                self.prefixes |= members
        self.dirs = set()
        for m in self.exact:
            parts = m.split("/")
            for k in range(2, len(parts)):
                self.dirs.add("/".join(parts[:k]))

    def covers(self, path):
        if is_pattern(path):
            rx = pattern_regex(path)
            return any(rx.match(m) for m in self.exact)
        if path in self.exact or path in self.dirs:
            return True
        parts = path.split("/")
        return any("/".join(parts[:k]) in self.prefixes for k in range(2, len(parts)))


def load_list(path, fields):
    """Tab-separated entries; '#' comments. Each entry: (pattern, rest...)."""
    entries = []
    if not Path(path).exists():
        return entries
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        cols = line.split("\t")
        if len(cols) < fields or not all(c.strip() for c in cols[:fields]):
            raise SystemExit("%s:%d: needs %d tab-separated fields" % (path, n, fields))
        entries.append(tuple(c.strip() for c in cols))
    return entries


def listed(path, entries):
    """The entry naming the path itself, else the first naming a directory above it or a glob matching it."""
    for e in entries:
        if e[0].rstrip("/") == path:
            return e
    for e in entries:
        pat = e[0].rstrip("/")
        if path == pat or path.startswith(pat + "/") or fnmatch.fnmatchcase(path, pat):
            return e
    return None


def under(path, dirs):
    prefix = path.split("*")[0].split("<")[0].split("?")[0]
    return next((d for d in dirs if prefix == d or prefix.startswith(d + "/")), None)


def file_archived(name, idx, allow):
    return name in idx.exact or bool(listed(name, allow)) or any(name.startswith(p + "/") for p in idx.prefixes)


def check(root, evidence, build=None):
    """(violations, report rows, stale entries, the build/ looked at)."""
    root = Path(root)
    build = Path(build) if build else default_build(root)
    idx = Index(load_archives(evidence))
    allow = load_list(root / NOT_ARCHIVED, 2)
    lost = load_list(root / LOST, 2)
    owned = gate_owned_dirs(root)
    violations, rows = [], []
    used = set()
    seen = set()
    for path, where in citations(root, build):
        if (path, where) in seen:
            continue
        seen.add((path, where))
        gone = listed(path, lost)
        wiped = under(path, owned)
        not_ev = listed(path, allow)
        if gone:
            used.add(("lost", gone[0]))
        if not_ev:
            used.add(("allow", not_ev[0]))
        exact_excuse = bool(not_ev) and not_ev[0].rstrip("/") == path
        # What exists must be archived file by file: one archived file does not cover its directory (AUDIT-004 F1).
        files = [] if not_ev else denoted_files(path, build)
        missing = ["build/" + str(f.relative_to(build)) for f in files]
        missing = [n for n in missing if not file_archived(n, idx, allow)]
        archived = (bool(files) or idx.covers(path)) and not missing
        if archived:
            status = "archived"
        elif missing:
            status = "unarchived"
            violations.append("%s: %s%s: %d existing file(s) under %s in no archive (%s%s); archive them "
                              "(tools/evidence_check.py --unarchived)"
                              % (where, path, " is recorded as lost but exists" if gone else "", len(missing), build,
                                 ", ".join(missing[:3]), ", ..." if len(missing) > 3 else ""))
        elif gone:
            status = "lost"
        elif wiped and not is_pattern(path) and not exact_excuse:
            # Only an entry naming exactly this path (the gate's own directory named in prose) excuses it.
            status = "wiped"
            violations.append("%s: %s is under %s, which a gate deletes; archive it or record it as lost"
                              % (where, path, wiped))
        elif not_ev:
            status = "not-evidence"
        else:
            status = "unarchived"
            violations.append("%s: %s is in no archive, not declared not-evidence (%s), not recorded as lost (%s)"
                              % (where, path, NOT_ARCHIVED, LOST))
        rows.append((status, path, where))
    stale = ["%s: %s" % (NOT_ARCHIVED, e[0]) for e in allow if ("allow", e[0]) not in used]
    stale += ["%s: %s" % (LOST, e[0]) for e in lost if ("lost", e[0]) not in used]
    return violations, rows, stale, build


MACHO = (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\x7fELF")
SKIP_SUFFIXES = (".o", ".d", ".a", ".dylib", ".so", ".sock", ".gguf")


def is_artifact(path):
    """A compiled binary, debug symbols, object, dependency file or socket: never archived (docs/EVIDENCE.md)."""
    if path.name.endswith(SKIP_SUFFIXES) or any(p.endswith(".dSYM") for p in path.parts) \
            or not path.is_file() or path.is_symlink():
        return True
    try:
        with open(path, "rb") as f:
            return f.read(4) in MACHO
    except OSError:
        return True


def unarchived_files(root, evidence, build=None):
    """Every existing file a citation denotes that is in no archive and not declared not-evidence: the input of the
    next archive pass (relative to the folder holding build/). Binaries, objects and sockets are left out."""
    root = Path(root)
    build = Path(build) if build else default_build(root)
    idx = Index(load_archives(evidence))
    allow = load_list(root / NOT_ARCHIVED, 2)
    out = set()
    for path, _ in citations(root, build):
        if listed(path, allow):
            continue
        for f in denoted_files(path, build):
            name = "build/" + str(f.relative_to(build))
            if not file_archived(name, idx, allow):
                out.add(name)
    return sorted(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    ap.add_argument("--evidence", default=os.environ.get("SCRIBODO_EVIDENCE_DIR", str(DEFAULT_EVIDENCE)))
    ap.add_argument("--build", help="where cited paths exist (default the main checkout's build/, also from a worktree)")
    ap.add_argument("--list", action="store_true", help="print every citation with its status")
    ap.add_argument("--unarchived", action="store_true",
                    help="print every existing file under a cited path that no archive holds (the next archive pass)")
    a = ap.parse_args(argv)
    if not Path(a.evidence).is_dir():
        print("evidence_check: skipped: no archive folder at %s (hosted CI has none; the check runs where the "
              "archives are)" % a.evidence)
        return 0
    if a.unarchived:
        for name in unarchived_files(a.root, a.evidence, a.build):
            print(name)
        return 0
    violations, rows, stale, build = check(a.root, a.evidence, a.build)
    if a.list:
        for status, path, where in rows:
            print("%-13s %s\t%s" % (status, path, where))
    for s in stale:
        print("evidence_check: warning: no document cites %s" % s)
    counts = {}
    for status, _, _ in rows:
        counts[status] = counts.get(status, 0) + 1
    summary = ", ".join("%d %s" % (counts[k], k) for k in sorted(counts))
    if violations:
        for v in violations:
            print(v)
        print("evidence_check: %d violation(s) over %d citations (%s); files looked for under %s"
              % (len(violations), len(rows), summary, build))
        return 1
    print("evidence_check: %d citations: %s; files looked for under %s" % (len(rows), summary, build))
    return 0


if __name__ == "__main__":
    sys.exit(main())
