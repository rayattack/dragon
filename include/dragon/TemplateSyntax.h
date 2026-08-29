#ifndef DRAGON_TEMPLATE_SYNTAX_H
#define DRAGON_TEMPLATE_SYNTAX_H

#include <string>

namespace dragon {

inline std::string precedingAttrName(const std::string& body, size_t bangPos) {
    auto isNameChar = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    auto isWs = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    size_t k = bangPos;
    if (k > 0 && (body[k-1] == '"' || body[k-1] == '\'')) k--;
    while (k > 0 && isWs(body[k-1])) k--;
    if (k == 0 || body[k-1] != '=') return "";
    k--;
    while (k > 0 && isWs(body[k-1])) k--;
    size_t nameEnd = k;
    while (k > 0 && isNameChar(body[k-1])) k--;
    return body.substr(k, nameEnd - k);
}

inline bool isEventAttrContext(const std::string& body, size_t bangPos) {
    std::string attr = precedingAttrName(body, bangPos);
    return attr.size() > 2 && (attr[0] == 'o' || attr[0] == 'O') &&
           (attr[1] == 'n' || attr[1] == 'N');
}

}

#endif
