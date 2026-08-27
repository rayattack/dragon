#ifndef DRAGON_REPL_TURN_H
#define DRAGON_REPL_TURN_H

#include <memory>
#include <string>
#include <vector>

#include "dragon/AST.h"
#include "dragon/CodeGen.h"
#include "dragon/Repl.h"

namespace dragon {

std::unique_ptr<Module> replParseCell(const std::string& source,
                                      const std::string& turnFilename,
                                      bool isDragonSurface,
                                      TurnResult& result);

void replFilterToTurn(TurnResult& result, const std::string& turnFilename);

bool replCallTurnEntry(void* entry, std::string& raiseText);

void replAddError(TurnResult& result,
                  const std::string& filename,
                  const std::string& message);

void replAddCodeGenErrors(TurnResult& result,
                          const std::string& filename,
                          const std::vector<CodeGenDiagnostic>& diagnostics);

}

#endif
