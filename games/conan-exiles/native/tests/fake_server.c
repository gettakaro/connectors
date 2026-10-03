/* Stand-in for ConanSandboxServer-Linux-Shipping in tests/so_test.py: the real library is
 * LD_PRELOADed into it. It is not build 25639945, so the library must take the unknown-build path.
 * Runs until stdin closes, then exits normally so the library's destructor runs. */
#include <stdio.h>

int main(void) {
    char line[256];
    while (fgets(line, sizeof line, stdin)) {
    }
    return 0;
}
