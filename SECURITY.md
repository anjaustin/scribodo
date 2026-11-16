# Security policy

## Reporting a vulnerability

Please report security issues privately, through GitHub: on the repository's
**Security** tab, choose **Report a vulnerability**. Don't open a public issue
or pull request for one.

Include what you found, how to reproduce it, and the commit you tested. You
will get an acknowledgement, and the fix and its disclosure are coordinated
with you in the advisory.

## Supported versions

Scribodo is pre-alpha and has no releases yet. Only the current `main` branch
is supported.

## What the design promises

Scribodo is a local application. Reports are most useful where they break one
of these:

- **Local only.** MariaDB and Qdrant bind only to localhost and use generated
  credentials kept in a private folder. The local model server listens on a
  private Unix socket with a fresh API key each launch.
- **No silent data exit.** Nothing leaves the Mac unless the author configures
  and selects a remote model. Then HTTPS is required, redirects are not
  followed, the API key is read only from a private file and sent only to the
  configured host, and the app shows where data is going.
- **Pinned inputs.** Model files are checked against pinned hashes, vendored
  libraries are pinned submodules, container images are pinned by digest,
  and GitHub Actions are pinned by commit.
- **The author's files.** Saves never overwrite a file another program
  changed. Backups refuse symlinks and special files, and restore refuses a
  backup that contains a link or a file not in its checksum list. The
  checksums detect damage, not deliberate tampering, so restore only backups
  you made.
- **Child processes.** The model servers and the services script start with
  only the descriptors they are given (`POSIX_SPAWN_CLOEXEC_DEFAULT`).

The reviews behind these properties are in the documentation map
([docs/README.md](docs/README.md#reviews)), and the risks accepted for now are
listed in [TECHNICAL-DEBT](docs/TECHNICAL-DEBT.md#accepted-residual-risks--persistence-review-and-direct-push-workflow).
