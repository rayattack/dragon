#ifndef DRAGON_DUBABLE_H
#define DRAGON_DUBABLE_H

#include "dragon/TypeChecker.h"
#include <string>

namespace dragon {

bool typeIsDubable(const Type* t, std::string& why);

}

#endif
