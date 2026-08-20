#include "runtime_internal.h"

#include <cstdint>

#include "sqlite3.h"

extern "C" {

void* dragon_sqlite_open(const char* path) {
    sqlite3* db = nullptr;
    sqlite3_open(path, &db);
    return db;
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
