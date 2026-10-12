// Aggregate test runner: runs all test suites and reports combined result.

#include <iostream>

int run_parser_tests();
int run_store_tests();
int run_tools_tests();

int main() {
    int total = 0;

    std::cout << "=== Parser tests ===\n";
    total += run_parser_tests();

    std::cout << "\n=== Store / cache / repository tests ===\n";
    total += run_store_tests();

    std::cout << "\n=== Tools / server tests ===\n";
    total += run_tools_tests();

    std::cout << "\n" << (total == 0 ? "ALL TESTS PASSED" : std::to_string(total) + " TOTAL FAILURES") << "\n";
    return total == 0 ? 0 : 1;
}
