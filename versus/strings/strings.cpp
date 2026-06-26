#include <iostream>
#include <string>

int main() {
    std::string s;
    for (int i = 0; i < 10000; i++) {
        s += "hello";
    }
    std::cout << s.size() << std::endl;
    return 0;
}
