#include "testing.h"

#include <cstring>

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (auto& c : sstest::registry()) {
        if (filter && std::strstr(c.name, filter) == nullptr) continue;
        sstest::current() = c.name;
        std::printf("...  %s\r", c.name);
        const int before = sstest::failures();
        c.fn();
        ++run;
        std::printf("%s %s\n", sstest::failures() == before ? "[ ok ]" : "[FAIL]", c.name);
    }
    std::printf("\n%d test cases, %d failed checks\n", run, sstest::failures());
    return sstest::failures() == 0 ? 0 : 1;
}

