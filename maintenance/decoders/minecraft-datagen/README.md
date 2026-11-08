# minecraft-datagen decoder

Runs the Minecraft dedicated server jar's own data generator (`--reports`) and hands the reports back as JSON, so the
Icon Pack mapper can read the item registry without running game code anywhere else. It follows the decoder-container
contract of `maintenance/src/takaro_maint/icons/decoders/container.py`.

- Request: `{"decoder": "minecraft-datagen", "root": "/game", "exports": [{"id", "kind": "datagen-report", "file": "server.jar", "report": "registries.json"}]}`.
- Answer: `/out/json/<id>.json` per report and `/out/manifest.json` with the tool block (decoder version and the JDK).
- The jar is mounted read-only at `/game`; the generator unpacks the bundler into `/tmp/datagen` (the only scratch
  space) and runs with `-Xmx2g`, no network and a read-only root file system.
- The image is built locally from two digests that are already pinned elsewhere in the repository (the Temurin 25 JDK
  of the Minecraft targets and the Python base of the Unity decoder). Nothing is downloaded at build time.
- Mojang's jar is run locally and never redistributed; only its registry facts are read.
