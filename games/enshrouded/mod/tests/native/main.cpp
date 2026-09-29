// Host tests for the native Takaro connector (src/native/). Built and run by tests/run.sh in gcc:14.
#include "testlib.h"

#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

HMODULE g_selfModule = nullptr;  // common.cpp (PluginBaseDir) expects the DLL's module handle

void RunUnitTests();
void RunParityTests(const std::string& fixtureDir);
void RunBridgeTests();

namespace t {

std::string TempDir(const std::string& tag) {
    static int n = 0;
    std::string d = "/tmp/native-test-" + std::to_string(getpid()) + "-" + std::to_string(++n) + "-" + tag;
    std::string cmd = "rm -rf '" + d + "' && mkdir -p '" + d + "'";
    if (system(cmd.c_str()) != 0) abort();
    return d;
}

void Group(const char* name) {
    S().group = name;
    fprintf(stderr, "-- %s\n", name);
}

}  // namespace t

int main(int argc, char** argv) {
    std::string fixtures = argc > 1 ? argv[1] : "tests/fixtures";
    RunUnitTests();
    RunParityTests(fixtures);
    RunBridgeTests();
    printf("native tests: %d checks, %d failures\n", t::S().checks, t::S().failures);
    return t::S().failures ? 1 : 0;
}
