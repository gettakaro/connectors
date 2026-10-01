# Pinned native dependencies

The Bookworm builder verifies each source archive before compiling. All compiled
libraries use PIC and hidden symbol visibility; the plugin links them statically.

| Library | Version | SHA-256 of source archive | License |
| --- | --- | --- | --- |
| libwebsockets | 4.5.8 | `b6ade658f4af3a823d0dc806ae5ef0623f0f4f5e2aeb895a0f77c4783840c30e` | MIT plus retained permissive component notices |
| OpenSSL | 3.5.8 | `a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2` | Apache-2.0 |
| nlohmann/json | 3.12.0 | `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa` | MIT |
| PCRE2 | 10.47 | `47fe8c99461250d42f89e6e8fdaeba9da057855d06eb7fc08d9ca03fd08d7bc7` | BSD-3-Clause WITH PCRE2-exception |

Archive URLs and build options are in `Dockerfile.build`. Full license texts are
in `third_party/licenses` and must accompany the plugin release archive.
