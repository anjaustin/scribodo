## What and why

<!-- One step per pull request: what it changes, and why. -->

## Evidence

<!-- The gates you ran and their exit status (CONTRIBUTING.md, "Building and testing"), and for a model-facing change the old-against-new measurement. -->

- [ ] `make test`, `make analyze` and `make sanitize` exit 0
- [ ] The gates for the areas touched (store, UI, threads, services) exit 0, or n/a
- [ ] Living docs updated in this change (README, USER-GUIDE, SERVICES, CONFIGURATION, TESTING), or n/a
- [ ] No experiment code deleted; concluded experiments stay behind their flag
