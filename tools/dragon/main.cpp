#include "dragon.h"
#include <iostream>

int main(int argc, char* argv[]) {
    dragon::initialize();

    dragon::Driver driver;

    if (!driver.parseArgs(argc, argv)) {
        return 1;
    }

    int result = driver.run();

    dragon::shutdown();

    return result;
}
