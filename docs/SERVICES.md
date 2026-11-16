# The project store and its services

Chats, notes, canon, findings, History and the observer's progress live in a
local MariaDB ([SOW](SOW.md) §5, [STORE-001](STORE-001-design.md)), beside a
Qdrant index that can be rebuilt from it. Documents always stay plain files
in the project folder.

`tools/services.sh up` starts both services: MariaDB 11.4 and Qdrant 1.20,
listening only on localhost, with generated credentials. The app also starts
them when it opens.

## Native or Docker

The services run one of two ways.

- **Natively** on the Mac: MariaDB from Homebrew (`brew install mariadb@11.4`)
  and Qdrant as the pinned release binary (`make qdrant`). This is the
  recommended default on a machine whose memory goes to the model. On the
  16 GB development Mac the Docker virtual machine cost about 1.9 GB, while the
  same services natively use about 0.17 GB at rest. (`docker stats` showed
  0.8 GB for the containers inside the VM, but it counts their page cache.)
  With the reranker resident, that difference meant an 8K or a 24K chat
  context at launch (`journal/store-cycle2_native_services.md`).
- **In Docker** (`docker/compose.yml`, images pinned by digest): an
  installation option for a machine with memory to spare.

`SCRIBODO_SERVICES=native|docker` forces a mode for one call. Otherwise the
script uses, in order:

1. the mode the last `up` remembered;
2. native, if a native store exists;
3. Docker, if credentials exist without a native store, so a store kept in
   the containers is never replaced by an empty native one;
4. native, when its binaries are installed; otherwise Docker. A machine
   missing either MariaDB 11.4 or the Qdrant binary therefore reports that
   Docker isn't running.

The native services do not start at login. Scribodo starts them when it
opens, and again, at most once a minute, if the database stops while it runs.
A stopped Qdrant is restarted only with the database, or by
`tools/services.sh up`.

## Versions

MariaDB is pinned to 11.4.13 (`tools/services.defaults`). Native mode starts
the pinned release silently, starts a newer 11.4 patch with a warning shown
once in the app, and refuses another series or an older patch. The Docker
images are pinned by digest and checked by `make dependency-report`.

## Moving a store from Docker to native

`tools/services.sh migrate-from-docker` moves an existing store from the
containers to the native services.

- It refuses while Scribodo runs, and when a native store already exists.
  `SCRIBODO_MIGRATE_REPLACE=1` replaces the native store, keeping a dump of it
  first.
- It keeps the dump of the containers' store.
- If anything fails, it puts both stores back as they were.
- `make servicestest` rehearses the move, including a load that fails and
  rolls back, when Docker is running, and skips it with a message otherwise.
  `SCRIBODO_SERVICESTEST_DOCKER=1` turns a missing Docker into a failure.

## Moving a project into the store

- The first time a project is opened with the store up, its `.scribodo/`
  files are moved in. The old files are kept in `.scribodo/legacy-…`.
- Documents stay plain files in the folder, and a rename keeps each
  document's id, so History and sources follow it.
- A failed read of notes or history stops the move without switching
  authority. Making a copied folder its own project uses a private snapshot,
  so retries do not duplicate chats or mix in later changes from the
  original.
- If the store is down, documents stay editable; notes, canon, chats and
  History pause until it's back. A project not yet moved keeps using its
  files.
- `SCRIBODO_STORE=off` runs without the store.

## Durability and backups

Commits survive the app or the database dying. A sudden power loss can lose
the last moments of commits, because MariaDB does not force the drive's
cache; see [USER-GUIDE](USER-GUIDE.md#how-saving-protects-your-text) for the
measurement. **Back up now** in the app exports the whole store to files.
`make backuptest` rehearses losing the folder, the database and the index,
then restoring and resuming.

The variables that move ports, folders and binaries are in
[CONFIGURATION](CONFIGURATION.md#services).
