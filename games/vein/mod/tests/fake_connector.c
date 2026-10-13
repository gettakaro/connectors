// Fake libtakaro-vein.so for tests/run-panel-loader.sh: announces that its constructor ran.
#include <stdio.h>
__attribute__((constructor)) static void FakeConnector(void) { printf("FAKE CONNECTOR %s LOADED\n", FAKE_TAG); fflush(stdout); }
