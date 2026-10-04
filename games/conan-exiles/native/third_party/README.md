# Pinned native dependencies

The buster builder (`platform/linux/Dockerfile.build`) verifies each source archive before
compiling. Both libraries are built with PIC and hidden symbol visibility and linked statically
into `libtakaro-conan-native.so`.

| Library | Version | SHA-256 of source archive | License |
| --- | --- | --- | --- |
| libwebsockets | 4.5.8 | `b6ade658f4af3a823d0dc806ae5ef0623f0f4f5e2aeb895a0f77c4783840c30e` | MIT plus retained permissive component notices |
| OpenSSL | 3.5.8 | `a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2` | Apache-2.0 |

The full license texts are in `licenses/` and must accompany the release archive.

The Linux release archive also carries `ca-certificates.crt`, the CA bundle of the builder's
Debian `ca-certificates` package (from the dated snapshot in `Dockerfile.build`; Mozilla's CA list,
MPL-2.0). The library trusts only the CA file it is configured with.

## Windows DLL (`winmm.dll`, `platform/windows/build.sh`)

Cross-compiled for x86_64 Windows with zig 0.13.0 (`x86_64-windows-gnu`, the same toolchain as
the Enshrouded native connector). Linked in statically:

| Component | Where it comes from | License |
| --- | --- | --- |
| MinHook | `third_party/minhook`, TsudaKageyu/minhook @ `8af6b4acae5a9388fd742b56fa79ece89d96f823` (file hashes in `minhook.sha256`, checked by the build) | BSD-2-Clause (`licenses/MinHook-LICENSE.txt`) |
| zig compiler-rt, libc++, libc++abi, libunwind, mingw-w64 runtime | zig 0.13.0 | MIT / Apache-2.0 WITH LLVM-exception / mingw-w64 notices |

WinHTTP, Crypt32, Winsock (`ws2_32.dll`) and the system `winmm.dll` are Windows system libraries
loaded at run time and are not shipped.
