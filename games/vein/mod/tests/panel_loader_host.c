// Stand-in for the game in tests/run-panel-loader.sh: its file name contains "VeinServer" like the
// real server. It dlopens the panel loader the way steamclient.so probes libSDL3.so.0, then reports
// which connector copies ended up mapped. Optional second argument: milliseconds to wait first.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc > 1 && !dlopen(argv[1], RTLD_NOW | RTLD_LOCAL)) fprintf(stderr, "dlopen loader: %s\n", dlerror());
    if (argc > 2) usleep((useconds_t)atoi(argv[2]) * 1000);
    FILE* f = fopen("/proc/self/maps", "r");
    char line[4096], last[4096] = "";
    while (f && fgets(line, sizeof line, f)) {
        char* p = strchr(line, '/');
        if (p && strstr(p, "libtakaro-vein.so") && strcmp(p, last) != 0) {
            printf("MAPPED %s", p);
            snprintf(last, sizeof last, "%s", p);
        }
    }
    if (f) fclose(f);
    fflush(stdout);
    // _exit: the real connector's start-up thread is not meant to run to completion in this fake
    // game, so skip exit handlers rather than wait for it.
    _exit(0);
}
