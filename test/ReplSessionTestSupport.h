#pragma once

#include <gtest/gtest.h>

#include <cstdio>
#include <memory>
#include <string>
#include <unistd.h>

#include "CodeBlock.h"
#include "dragon.h"
#include "dragon/Repl.h"

namespace dragon::test {

class CapturedStdout {
public:
    CapturedStdout() {
        fflush(stdout);
        saved_ = dup(fileno(stdout));
        file_ = tmpfile();
        dup2(fileno(file_), fileno(stdout));
    }

    std::string stop() {
        if (saved_ < 0) return text_;
        fflush(stdout);
        dup2(saved_, fileno(stdout));
        close(saved_);
        saved_ = -1;

        rewind(file_);
        char buffer[4096];
        size_t got = 0;
        while ((got = fread(buffer, 1, sizeof(buffer), file_)) > 0)
            text_.append(buffer, got);
        fclose(file_);
        file_ = nullptr;
        return text_;
    }

    ~CapturedStdout() { stop(); }

private:
    int saved_ = -1;
    FILE* file_ = nullptr;
    std::string text_;
};

inline std::string describeTurn(const TurnResult& result) {
    std::string out = "turn " + std::to_string(result.turnIndex) + ": ";
    if (!result.exceptionText.empty()) out += result.exceptionText + " ";
    for (const auto& diag : result.diagnostics) out += diag.message + "; ";
    return out;
}

class ReplSessionTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() { dragon::initialize(); }

    virtual ReplOptions replOptions() { return ReplOptions{}; }
    virtual std::string caseFile() const = 0;

    void SetUp() override {
        session = std::make_unique<ReplSession>(replOptions());
        std::string error;
        ASSERT_TRUE(session->bringUp(error)) << error;
    }

    std::string cell(const std::string& block) {
        return extractCode(caseFile(), block);
    }

    TurnResult run(const std::string& block) {
        return session->evaluate(cell(block));
    }

    [[nodiscard]] ::testing::AssertionResult runOk(const std::string& block) {
        CapturedStdout capture;
        auto result = session->evaluate(cell(block));
        capture.stop();
        if (result.status == TurnStatus::Ok) return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << block << " -> " << describeTurn(result);
    }

    std::string echoOf(const std::string& block) {
        CapturedStdout capture;
        auto result = session->evaluate(cell(block));
        std::string out = capture.stop();
        if (result.status != TurnStatus::Ok) {
            ADD_FAILURE() << block << " did not run: " << describeTurn(result);
            return {};
        }
        return out;
    }

    [[nodiscard]] ::testing::AssertionResult repeat(const std::string& block, int times) {
        const std::string source = cell(block);
        for (int i = 0; i < times; ++i) {
            auto result = session->evaluate(source);
            if (result.status != TurnStatus::Ok)
                return ::testing::AssertionFailure()
                       << block << " iteration " << i << ": " << describeTurn(result);
        }
        return ::testing::AssertionSuccess();
    }

    std::unique_ptr<ReplSession> session;
};

}
