#ifndef DRAGON_VALUE_TAGS_H
#define DRAGON_VALUE_TAGS_H

#include <cstdint>

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

constexpr int8_t TAG_CALLABLE = 10;

constexpr int8_t TAG_TASK_HANDLE = 100;

#endif
