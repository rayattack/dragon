#ifndef DRAGON_H
#define DRAGON_H

/// Dragon Language Compiler
/// A typed, compiled Python superset targeting LLVM
/// Copyright (c) Tersoo Ortserga

#include "dragon/Token.h"
#include "dragon/Lexer.h"
#include "dragon/AST.h"
#include "dragon/Parser.h"
#include "dragon/Sema.h"
#include "dragon/TypeChecker.h"
#include "dragon/CodeGen.h"
#include "dragon/Driver.h"

namespace dragon {

/// Library version
constexpr const char* VERSION = "0.0.1";

/// Initialize the Dragon compiler (call once at startup)
void initialize();

/// Cleanup (call before exit)
void shutdown();

} // namespace dragon

#endif // DRAGON_H
