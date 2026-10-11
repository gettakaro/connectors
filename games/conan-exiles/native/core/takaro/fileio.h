// File primitives for the native connector (from the Enshrouded native connector): durable atomic
// replace, reads, directory creation and the stat a log tailer needs. The path helpers are portable
// (core/takaro/fileio.cpp); the primitives are per platform (platform/linux/fileio_posix.cpp,
// platform/windows/fileio_win32.cpp).
#pragma once
#include <cstdint>
#include <string>

namespace takaro {

// Reads a whole file. exists=false (and true returned) when there is no such file.
bool ReadWholeFile(const std::string& path, std::string& text, bool& exists, std::string& err);
// tmp file -> flush to disk -> rename over the target (Win32: FlushFileBuffers + MoveFileExW
// REPLACE_EXISTING|WRITE_THROUGH). The target is never left half-written.
bool AtomicWriteFile(const std::string& path, const std::string& text, std::string& err);
// Replaces a file a person edits (takaro.json) the same atomic way, but keeps the old file's
// permissions and owner (POSIX mode + uid/gid; Win32 ReplaceFileW keeps the ACL). A new file gets
// `newMode` (POSIX) and, when the process runs as root, the owner of its directory, so a host user
// is never locked out of their own config.
bool ReplaceUserFile(const std::string& path, const std::string& text, unsigned newMode, std::string& err);
bool EnsureDirectory(const std::string& dir, std::string& err);
bool RemoveFileIfExists(const std::string& path);

struct FileStat {
    bool exists = false;
    uint64_t size = 0;
    std::string identity;  // changes when the file is replaced (inode / NTFS file index)
};
FileStat StatFile(const std::string& path);
bool ReadFileRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out);

// Cryptographically random bytes (/dev/urandom, BCryptGenRandom). False when none are available.
bool RandomBytes(unsigned char* out, size_t n);
// One write to the server's standard output (what a panel or `docker logs` shows). Never blocks
// for long and never throws; does nothing when there is no stdout.
void ConsoleWrite(const std::string& text);

bool IsAbsolutePath(const std::string& p);
std::string JoinPath(const std::string& dir, const std::string& name);
std::string DirName(const std::string& path);
// Relative paths resolve against baseDir; absolute ones are kept.
std::string ResolvePath(const std::string& baseDir, const std::string& p);

}  // namespace takaro
