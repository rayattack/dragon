#ifndef DRAGON_VALUE_TAGS_H
#define DRAGON_VALUE_TAGS_H

#include <cstdint>

// The boxed-value tag ABI shared by the runtime (container elem_tag, DragonBox.tag)
// and codegen (which emits these values as constants). This header is the single
// source of truth; renumbering here is an ABI break for every compiled program.
enum DragonValueTag : int8_t {
    TAG_INT = 0,
    TAG_STR = 1,
    TAG_FLOAT = 2,
    TAG_BOOL = 3,
    TAG_NONE = 4,
    TAG_LIST = 5,
    TAG_DICT = 6,
    TAG_BYTES = 7,
};

// Value-tag slot 10 carries a Callable payload (a DragonClosure or bare fn
// ptr); numerically equal to the runtime object tag DRAGON_TAG_CLOSURE.
constexpr int8_t TAG_CALLABLE = 10;

#endif  // DRAGON_VALUE_TAGS_H
