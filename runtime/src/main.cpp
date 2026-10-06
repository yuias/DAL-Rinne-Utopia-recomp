#include <cstdio>
#include <cstring>

#ifndef DAL_VERSION
#define DAL_VERSION "0.0.0"
#endif

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: dal_runtime [--version]");
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::puts("dal_runtime " DAL_VERSION);
        return 0;
    }
    std::fprintf(stderr, "dal_runtime: unknown argument '%s'\n", argv[1]);
    return 2;
}
