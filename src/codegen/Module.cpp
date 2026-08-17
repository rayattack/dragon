#include "../CodeGenImpl.h"

namespace dragon {

void CodeGen::visit(dragon::Module& node) {
    for (auto& stmt : node.body) {
        stmt->accept(*this);
    }
}

}
