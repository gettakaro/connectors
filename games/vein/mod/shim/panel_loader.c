// Panel loader for hosts that cannot set LD_PRELOAD (stock AMP). Installed as libSDL3.so.0 in a
// directory on the server's LD_LIBRARY_PATH: steamclient.so probes for that optional library while
// the dedicated server starts, and this constructor then loads libtakaro-vein.so from the same
// directory. It exports no SDL symbols, so the caller's lookups fail and Steam carries on without SDL.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

__attribute__((constructor)) static void TakaroPanelLoader(void) {
    char exe[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0 || !strstr(exe, "VeinServer")) return;
    Dl_info self;
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
