#include "runtime_internal.h"

#include <cstdint>

#include "sqlite3.h"

extern "C" {

void* dragon_sqlite_open(const char* path) {
    sqlite3* db = nullptr;
    sqlite3_open(path, &db);
    return db;
}

int64_t dragon_sqlite_bind_text(void* stmt, int64_t index, const char* value) {
    if (!stmt) return SQLITE_MISUSE;
    char* owned = nullptr;
    int64_t blen = 0;
    const char* utf8 = dragon_cstr_open(value, &owned, &blen);
    if (!utf8) { dragon_cstr_close(owned); return sqlite3_bind_null((sqlite3_stmt*)stmt, (int)index); }
    if (blen > INT32_MAX) {
        dragon_cstr_close(owned);
        dragon_raise_exc_cstr(90, "ValueError: sqlite text parameter exceeds 2 GiB");
        return SQLITE_MISUSE;
    }
    int rc = sqlite3_bind_text((sqlite3_stmt*)stmt, (int)index, utf8, (int)blen,
                               SQLITE_TRANSIENT);
    dragon_cstr_close(owned);
    return rc;
}

int64_t dragon_sqlite_close(void* db) {
    if (!db) return SQLITE_OK;
    return sqlite3_close_v2((sqlite3*)db);
}

void* dragon_sqlite_prepare(void* db, const char* sql, int64_t nbytes) {
    if (!db) return nullptr;
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2((sqlite3*)db, sql, (int)nbytes, &stmt, nullptr);
    return stmt;
}

int64_t dragon_sqlite_finalize(void* stmt) {
    if (!stmt) return SQLITE_OK;
    return sqlite3_finalize((sqlite3_stmt*)stmt);
}

}
