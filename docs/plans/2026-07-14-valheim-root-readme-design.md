# Valheim Root README Design

## Goal

Make the repository's root README accurately advertise the merged Valheim connector and direct readers to its detailed installation and capability documentation.

## Scope

Update only `README.md`:

- add Valheim to the connector table;
- add the Valheim release command beside the other connector release commands;
- add a concise connector note describing the dedicated-server plugin and separately packaged graphical-client companion; and
- link the connector name and note to `valheim/README.md`.

No Valheim implementation, workflow, package, or capability documentation changes are required. The root entry should summarize the architecture without duplicating the full Valheim README.

## Verification

Confirm the relative `valheim/` and `valheim/README.md` links resolve, the documented `release-valheim` recipe exists in the root `justfile`, and the final Markdown diff is limited to the approved root README additions.
