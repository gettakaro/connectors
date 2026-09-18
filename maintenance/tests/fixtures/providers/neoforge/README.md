# NeoForge provider fixtures

Recorded 2026-09-17 with the identifying User-Agent the tool sends
(`takaro-connectors-maint/<version> (+https://github.com/gettakaro/connectors)`):

```
UA="takaro-connectors-maint/0.1.0 (+https://github.com/gettakaro/connectors)"
curl -sS -A "$UA" https://maven.neoforged.net/releases/net/neoforged/neoforge/maven-metadata.xml \
    > maven-metadata.xml
for v in 21.11.45 26.2.0.88 26.3.0.3-beta; do
  curl -sS -A "$UA" "https://maven.neoforged.net/releases/net/neoforged/neoforge/$v/neoforge-$v-installer.jar.sha256" \
      > "sha256/$v.sha256"
done
```

- `maven-metadata.xml` — 1710 live `<version>` entries trimmed to 19, in publish order
  (oldest first), covering every grammar the provider has to survive:
  - `21.11.38-beta` … `21.11.45` — the three-segment grammar, where `21.11.45` means
    Minecraft `1.21.11`;
  - `26.1.2.108`, `26.1.2.109`, `26.2.0.85` … `26.2.0.88` — the four-segment grammar, where
    the third segment is the game patch (`26.2.0.88` means `26.2`, `26.1.2.109` means `26.1.2`);
  - `26.3.0.0-beta` … `26.3.0.3-beta` — a game version NeoForge has only betas for;
  - `26.1.0.0-alpha.1+snapshot-1` — an alpha, and
  - `0.25w14craftmine.3-beta` — an April-Fools line that parses as nothing at all.
  `<latest>` and `<release>` are the live values and both point at a beta, which is why the
  provider never reads either pointer.
- `sha256/<version>.sha256` — the live installer digest sidecars, plain hex, newline
  terminated.

Recorded heads: release `26.2.0.88`, beta `26.3.0.3-beta`.
