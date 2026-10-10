// Panel loader for hosts that cannot set LD_PRELOAD (stock AMP). Installed as libSDL3.so.0 in a
// directory on the server's LD_LIBRARY_PATH: steamclient.so probes for that optional library while
// the dedicated server starts, and this constructor then loads libtakaro-vein.so from the same
// directory. It exports no SDL symbols, so the caller's lookups fail and Steam carries on without SDL.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void Say(const char* text) {
    ssize_t w = write(STDOUT_FILENO, text, strlen(text));
    (void)w;
}

// 1 when /proc/self/maps already lists a libtakaro-vein.so (for example from LD_PRELOAD).
static int ConnectorAlreadyMapped(void) {
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    static const char kName[] = "/libtakaro-vein.so\n";
    char buf[65536 + sizeof kName];
    size_t keep = 0;
    int found = 0;
    for (;;) {
        ssize_t n = read(fd, buf + keep, sizeof buf - 1 - keep);
        if (n <= 0) break;
        size_t len = keep + (size_t)n;
        buf[len] = '\0';
        if (strstr(buf, kName)) { found = 1; break; }
        keep = len < sizeof kName ? len : sizeof kName - 1;  // a match may straddle two reads
        memmove(buf, buf + len - keep, keep);
    }
    close(fd);
    return found;
}

__attribute__((constructor)) static void TakaroPanelLoader(void) {
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || !strstr(exe, "VeinServer")) return;
    Dl_info self;
    if (ConnectorAlreadyMapped()) {
        Say("[Takaro] panel loader: the connector is already loaded (LD_PRELOAD?); not loading it a second time\n");
        return;
    }
    if (!dladdr((void*)&TakaroPanelLoader, &self) || !self.dli_fname) return;
    char path[PATH_MAX];
    const char* slash = strrchr(self.dli_fname, '/');
    int dir = slash ? (int)(slash - self.dli_fname) : 0;
    snprintf(path, sizeof path, "%.*s%slibtakaro-vein.so", dir, self.dli_fname, slash ? "/" : "");
    // RTLD_NODELETE: the connector must stay mapped even if the prober dlcloses this loader.
    if (!dlopen(path, RTLD_NOW | RTLD_GLOBAL | RTLD_NODELETE)) {
        char line[PATH_MAX + 256];
        int len = snprintf(line, sizeof line, "[Takaro] ERROR: could not load %s: %s\n", path, dlerror());
        if (len > 0) { ssize_t w = write(STDOUT_FILENO, line, (size_t)len); (void)w; }
    }
}
