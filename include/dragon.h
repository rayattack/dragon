#ifndef DRAGON_H
#define DRAGON_H

/// Dragon Language Compiler. Copyright (c) Tersoo Ortserga

#include "dragon/Token.h"
#include "dragon/Lexer.h"
#include "dragon/AST.h"
#include "dragon/Parser.h"
#include "dragon/Sema.h"
#include "dragon/TypeChecker.h"
#include "dragon/CodeGen.h"
#include "dragon/Driver.h"

namespace dragon {

constexpr const char* VERSION = "0.0.4";

void initialize();

void shutdown();

}

#endif
