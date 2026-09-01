#include "test_framework.h"

#include "synth/tables.h"

int main() {
    msynth::tables_init();
    return testfw::run_all();
}
