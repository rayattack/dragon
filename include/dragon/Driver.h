#ifndef DRAGON_DRIVER_H
#define DRAGON_DRIVER_H

#include <string>
#include <vector>
#include <memory>

namespace dragon {

struct DriverOptions {
    enum class Action {
        Run,
        Build,
        Check,
        Emit,
        FfiSync,
        Migrate
    };

    Action action = Action::Build;
    std::vector<std::string> inputFiles;
    std::vector<std::string> programArgs;
    std::string outputFile;
    int optimizationLevel = 0;
    bool verbose = false;
    bool debugInfo = false;
    bool forcePython = false;
    bool dumpAst = false;
    bool dumpTokens = false;
    std::vector<std::string> searchPaths;
    bool enableSitePackages = false;
    std::vector<std::string> linkedLibraries;
    std::vector<std::string> librarySearchPaths;
    std::vector<std::string> ccSources;
    std::string gcMode = "rc";
    bool checkOverflow = false;
    bool ffiCheck = false;
};

class Driver {
public:
    Driver();
    ~Driver();

    bool parseArgs(int argc, char* argv[]);
    int run();
    int run(const DriverOptions& options);

    static void printUsage();
    static void printVersion();

private:
    int runFile(const std::string& filename);
    int buildFile(const std::string& filename);
    int checkFile(const std::string& filename);
    int migrateFile(const std::string& filename);

    std::string readFile(const std::string& filename);
    bool isDragonFile(const std::string& filename);
    bool isPythonFile(const std::string& filename);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}

#endif
