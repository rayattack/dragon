// D052 `dragon ffi sync` tool tests: stub provenance guard + idempotence
#include <gtest/gtest.h>
#include "../src/FfiSync.h"
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace dragon;
namespace fs = std::filesystem;

namespace {

void writeFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

fs::path freshDir(const std::string& name) {
    fs::path dir = fs::temp_directory_path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

const char* kImgClass =
    "class Img {\n"
    "    def(w: int, h: int) {\n"
    "        self.w = w\n"
    "        self.h = h\n"
    "    }\n"
    "}\n";

}  // namespace

TEST(FfiSyncTest, GeneratesAndStaysIdempotent) {
    fs::path dir = freshDir("dragon-ffisync-idem");
    writeFile(dir / "tools_a.dr",
              std::string("extern \"golang\" def resize(w: int, h: int) -> Img "
                          "from \"gen/imgtool\"\n\n") + kImgClass);
    ASSERT_EQ(runFfiSync((dir / "tools_a.dr").string(), false), 0);
    const std::string stub = slurp(dir / "gen" / "imgtool_stub.go");
    ASSERT_NE(stub.find("AUTO-GENERATED"), std::string::npos);
    ASSERT_NE(stub.find("from tools_a.dr"), std::string::npos);
    // Re-sync from the same owner: unchanged, exit 0, --check clean.
    ASSERT_EQ(runFfiSync((dir / "tools_a.dr").string(), false), 0);
    EXPECT_EQ(slurp(dir / "gen" / "imgtool_stub.go"), stub);
    EXPECT_EQ(runFfiSync((dir / "tools_a.dr").string(), true), 0);
}

TEST(FfiSyncTest, ProvenanceGuardRefusesForeignOwner) {
    fs::path dir = freshDir("dragon-ffisync-owner");
    writeFile(dir / "tools_a.dr",
              std::string("extern \"golang\" def resize(w: int, h: int) -> Img "
                          "from \"gen/imgtool\"\n\n") + kImgClass);
    // Same binary, different param names: the silent zero-fill shape.
    writeFile(dir / "tools_b.dr",
              std::string("extern \"golang\" def resize(width: int, height: int) -> Img "
                          "from \"gen/imgtool\"\n\n") + kImgClass);
    ASSERT_EQ(runFfiSync((dir / "tools_a.dr").string(), false), 0);
    const std::string owned = slurp(dir / "gen" / "imgtool_stub.go");
    EXPECT_EQ(runFfiSync((dir / "tools_b.dr").string(), false), 1);
    EXPECT_EQ(slurp(dir / "gen" / "imgtool_stub.go"), owned);  // untouched
}

TEST(FfiSyncTest, RefusesToClobberHumanFile) {
    fs::path dir = freshDir("dragon-ffisync-human");
    writeFile(dir / "tools_a.dr",
              std::string("extern \"python\" def score(n: int) -> Img "
                          "from \"gen/score.py\"\n\n") + kImgClass);
    // A file already sits at the stub path with no AUTO-GENERATED marker.
    writeFile(dir / "gen" / "score_stub.py", "# my precious handwritten helper\n");
    EXPECT_EQ(runFfiSync((dir / "tools_a.dr").string(), false), 1);
    EXPECT_EQ(slurp(dir / "gen" / "score_stub.py"), "# my precious handwritten helper\n");
}

TEST(FfiSyncTest, SkeletonIsWrittenOnceAndKept) {
    fs::path dir = freshDir("dragon-ffisync-skel");
    writeFile(dir / "tools_a.dr",
              std::string("extern \"golang\" def resize(w: int, h: int) -> Img "
                          "from \"gen/imgtool\"\n\n") + kImgClass);
    ASSERT_EQ(runFfiSync((dir / "tools_a.dr").string(), false), 0);
    writeFile(dir / "gen" / "imgtool.go", "// edited by a human\n");
    ASSERT_EQ(runFfiSync((dir / "tools_a.dr").string(), false), 0);
    EXPECT_EQ(slurp(dir / "gen" / "imgtool.go"), "// edited by a human\n");
}
