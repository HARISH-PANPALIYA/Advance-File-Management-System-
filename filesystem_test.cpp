#include <iostream>
#include <filesystem>

int main() {
    std::filesystem::path p = ".";
    std::cout << p.string() << std::endl;
    return 0;
}
