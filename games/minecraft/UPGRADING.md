# Takaro Minecraft Connector: upgrading and older releases

The install steps are in [README.md](README.md). This page covers older release files and the
server-version check.

## Older file names

Releases before 0.5.0 shipped one file per platform, without the server version in the name:

| Your server | Old file name | Current file name |
|---|---|---|
| Fabric | `takaro-fabric-<version>.jar` | `takaro-minecraft-mod-fabric-26.2-<version>.jar` |
| Paper | `takaro-paper-<version>.jar` | `takaro-minecraft-mod-paper-1.21.11-<version>.jar` |
| NeoForge | `takaro-neoforge-<version>.jar` | `takaro-minecraft-mod-neoforge-1.21.11-<version>.jar` |

For two stable releases after the rename, each old name is still attached to the release as a
byte-identical copy of that platform's default file (Fabric 26.2, Paper 1.21.11,
NeoForge 1.21.11), so existing download scripts keep working for now. Switch them to the current
names. Fabric servers on 26.1.2 or 26.3 have their own file and never had an old name.

To upgrade, follow [README.md](README.md#6-upgrading): stop the server, replace the jar, start it
again. The config file stays as it is.

## The server-version check

Just before connecting, the connector logs one `Takaro target-check:` line saying which server build
the jar was made for and which one it found. If they do not match, it refuses to connect rather than
half-working on a server it was never tested on:

```
Takaro refuses to connect: target fabric-26.2 does not match this server
(built for game version 26.2, this server runs 26.1.2).
Set TAKARO_TARGET_POLICY=warn to override.
```

The fix is to download the file for your server's version. The environment variable
`TAKARO_TARGET_POLICY` changes the check: `warn` connects anyway and `off` skips the check entirely.
Neither makes an untested combination work.
