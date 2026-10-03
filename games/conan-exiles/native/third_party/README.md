# Pinned native dependencies

The buster builder (`platform/linux/Dockerfile.build`) verifies each source archive before
compiling. Both libraries are built with PIC and hidden symbol visibility and linked statically
into `libtakaro-conan-native.so`.

| Library | Version | SHA-256 of source archive | License |
| --- | --- | --- | --- |
| libwebsockets | 4.5.8 | `b6ade658f4af3a823d0dc806ae5ef0623f0f4f5e2aeb895a0f77c4783840c30e` | MIT plus retained permissive component notices |
| OpenSSL | 3.5.8 | `a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2` | Apache-2.0 |

The full license texts are in `licenses/` and must accompany the release archive.
