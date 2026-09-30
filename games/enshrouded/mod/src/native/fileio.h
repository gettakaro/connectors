// File primitives for the native connector: durable atomic replace, reads, directory creation and
// the stat a log tailer needs. Win32 in the plugin; a POSIX branch lets the host tests run the same
// persistence code on Linux.
#pragma once
#include <cstdint>
#include <string>

namespace native {

// Reads a whole file. exists=false (and true returned) when there is no such file.
bool ReadWholeFile(const std::string& path, std::string& text, bool& exists, std::string& err);
// tmp file -> flush to disk -> rename over the target (Win32: FlushFileBuffers + MoveFileExW
// REPLACE_EXISTING|WRITE_THROUGH). The target is never left half-written.
bool AtomicWriteFile(const std::string& path, const std::string& text, std::string& err);
bool EnsureDirectory(const std::string& dir, std::string& err);
bool RemoveFileIfExists(const std::string& path);

struct FileStat {
    bool exists = false;
    uint64_t size = 0;
    std::string identity;  // changes when the file is replaced (inode / NTFS file index)
};
FileStat StatFile(const std::string& path);
bool ReadFileRange(const std::string& path, uint64_t offset, uint64_t length, std::string& out);

bool IsAbsolutePath(const std::string& p);
std::string JoinPath(const std::string& dir, const std::string& name);
std::string DirName(const std::string& path);
// Relative paths resolve against baseDir; absolute ones are kept.
std::string ResolvePath(const std::string& baseDir, const std::string& p);

}  // namespace native
