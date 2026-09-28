#include "Test.hpp"

#include <cstring>
#include <exception>

namespace yte_test {
std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}
} // namespace yte_test

int main(int argc, char** argv) {
    int passed = 0, failed = 0, skipped = 0;
    for (const auto& test : yte_test::registry()) {
        if (argc > 1 && std::strstr(test.name, argv[1]) == nullptr)
            continue;
        try {
            test.body();
            ++passed;
            std::cout << "[ PASS ] " << test.name << "\n";
        } catch (const yte_test::Skip& skip) {
            ++skipped;
            std::cout << "[ SKIP ] " << test.name << " (" << skip.reason << ")\n";
        } catch (const yte_test::Failure& failure) {
            ++failed;
            std::cout << "[ FAIL ] " << test.name << "\n    " << failure.message << "\n";
        } catch (const std::exception& ex) {
            ++failed;
            std::cout << "[ FAIL ] " << test.name << "\n    unexpected exception: " << ex.what() << "\n";
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << skipped << " skipped\n";
    return failed == 0 ? 0 : 1;
}
