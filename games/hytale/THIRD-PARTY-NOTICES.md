# Third-party notices

The Takaro Hytale connector is MIT licensed (see [LICENSE](LICENSE)). It was written by
mad-001 as [Hytale-Takaro-Integration](https://github.com/mad-001/Hytale-Takaro-Integration)
and is maintained here by the Takaro team.

The plugin jar bundles these libraries, unmodified:

| Library | Version | Licence |
|---|---|---|
| [Java-WebSocket](https://github.com/TooTallNate/Java-WebSocket) | 1.5.6 | MIT |
| [SLF4J API](https://www.slf4j.org/) (pulled in by Java-WebSocket) | 2.0.x | MIT |
| [Gson](https://github.com/google/gson) | 2.10.1 | Apache-2.0 |

The Hytale server (`com.hypixel.hytale:Server`) is proprietary to Hypixel Studios. The build
compiles against it from Hypixel's Maven repository and never ships it or any part of it.
