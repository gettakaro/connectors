# Third-party code in dbghelp.dll

`dbghelp.dll` is cross-compiled for x86_64 Windows with zig 0.13.0 (the catalog target
pins the tarball by SHA-256). What ends up linked into it:

| Component | Where it comes from | License | Text in the release |
| --- | --- | --- | --- |
| MinHook | `mod/third_party/minhook`, TsudaKageyu/minhook @ `8af6b4acae5a9388fd742b56fa79ece89d96f823` | BSD-2-Clause | `licenses/MinHook-LICENSE.txt` |
| Zstandard decoder | `mod/third_party/zstd`, facebook/zstd v1.5.6 release (`zstd-1.5.6.tar.gz`, sha256 `8c29e06cf42aacc1eafc4077ae2ec6c6fcb96a626157e0593d5e82a34fd403c1`): `zstddeclib.c` from `build/single_file_libs/create_single_file_decoder.sh`, `zstd.h` from `lib/` | BSD-3-Clause (dual-licensed BSD/GPLv2; used under BSD) | `licenses/zstd-LICENSE.txt` |
| zig compiler-rt | zig 0.13.0 | MIT | `licenses/zig-LICENSE.txt` |
| libc++, libc++abi, libunwind | LLVM, as bundled with zig 0.13.0 | Apache-2.0 WITH LLVM-exception | `licenses/libcxx-LICENSE.txt`, `licenses/libcxxabi-LICENSE.txt`, `licenses/libunwind-LICENSE.txt` |
| mingw-w64 runtime | as bundled with zig 0.13.0 | mingw-w64 COPYING (public domain and permissive notices) | `licenses/mingw-w64-COPYING.txt` |

WinHTTP, Crypt32 and Winsock are Windows system libraries the DLL imports at run time;
they are not shipped.
