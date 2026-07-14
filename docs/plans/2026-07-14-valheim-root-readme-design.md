# Valheim Root README Design

## Goal

Make the repository's root README accurately advertise the merged Valheim connector and direct readers to its detailed installation and capability documentation.

## Scope

Update `README.md` and the root `justfile`:

- add Valheim to the connector table;
- replace the obsolete manual `release-*` examples with the current Release Please workflow and valid local `build-release-*` recipes;
- add the missing `build-release-valheim` recipe used by that documentation;
- add a concise connector note describing the dedicated-server plugin and separately packaged graphical-client companion; and
- link the connector name and note to `valheim/README.md`.

No Valheim implementation, workflow, package, or capability documentation changes are required. The root entry should summarize the architecture without duplicating the full Valheim README.

## Verification

Confirm the relative `valheim/` and `valheim/README.md` links resolve, every documented `build-release-*` recipe exists in the root `justfile`, and the final diff is limited to the approved README and Just additions.
