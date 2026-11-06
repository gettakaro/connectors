# Developing the Counter-Strike 2 connector

From the repository root:

```bash
docker run --rm -v "$PWD:/repo" -w /repo/games/counter-strike2/mod mcr.microsoft.com/dotnet/sdk:10.0 dotnet test Takaro.Cs2.sln
bash games/counter-strike2/scripts/build-release.sh 0.1.0 /tmp/cs2-release --target linux-26445667
python3 -m zipfile -l /tmp/cs2-release/takaro-counter-strike2-linux-26445667-0.1.0.zip
```

The catalog and release script pin the build toolchain and framework inputs. Keep `.runtime` server and proof data out of Git.
