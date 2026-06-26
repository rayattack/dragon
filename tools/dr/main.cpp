/// Dragon Compiler - CLI Entry Point
/// Usage: dr <command> [options] <files>

#include "dragon.h"
#include <iostream>

int main(int argc, char* argv[]) {
    // Initialize Dragon compiler
    dragon::initialize();
    
    // Create driver and parse arguments
    dragon::Driver driver;
    
    if (!driver.parseArgs(argc, argv)) {
        return 1;
    }
    
    // Run the compiler
    int result = driver.run();
    
    // Cleanup
    dragon::shutdown();
    
    return result;
}
