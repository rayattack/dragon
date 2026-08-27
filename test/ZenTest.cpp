#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string readRepoFile(const std::string& relative) {
    const std::string path = std::string(DRAGON_ROOT_DIR) + "/" + relative;
    std::ifstream in(path);
    if (!in) return {};
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

std::string stripMarkdown(std::string line) {
    static const char* markers[] = {"**", "*", "`", "# "};
    for (const char* marker : markers) {
        const size_t width = std::string(marker).size();
        size_t at = 0;
        while ((at = line.find(marker, at)) != std::string::npos)
            line.erase(at, width);
    }
    return line;
}

std::string trimRight(std::string line) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
        line.pop_back();
    return line;
}

std::vector<std::string> zenLines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) out.push_back(trimRight(stripMarkdown(line)));
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

std::vector<std::string> quotedEntries(const std::string& source) {
    std::vector<std::string> out;
    const size_t open = source.find("const ZEN: list[str] = [");
    if (open == std::string::npos) return out;
    const size_t close = source.find("\n]", open);
    if (close == std::string::npos) return out;

    const std::string body = source.substr(open, close - open);
    size_t at = 0;
    while ((at = body.find('"', at)) != std::string::npos) {
        const size_t end = body.find('"', at + 1);
        if (end == std::string::npos) break;
        out.push_back(body.substr(at + 1, end - at - 1));
        at = end + 1;
    }
    return out;
}

TEST(ZenTest, ThisModuleMatchesZenMd) {
    const std::string zen = readRepoFile("zen.md");
    ASSERT_FALSE(zen.empty()) << "zen.md could not be read";

    const std::string module = readRepoFile("stdlib/this.dr");
    ASSERT_FALSE(module.empty()) << "stdlib/this.dr could not be read";

    const std::vector<std::string> expected = zenLines(zen);
    const std::vector<std::string> actual = quotedEntries(module);

    ASSERT_FALSE(expected.empty());
    ASSERT_EQ(expected.size(), actual.size())
        << "stdlib/this.dr has drifted from zen.md: line counts differ";

    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], actual[i])
            << "stdlib/this.dr line " << (i + 1) << " has drifted from zen.md";
    }
}

}
