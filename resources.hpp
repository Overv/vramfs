#ifndef RESOURCES_HPP
#define RESOURCES_HPP

/*
 * Wrappers around SQLite types for memory management
 *
 * The type is unique_ptr because these resources are not intented to be shared
 * beyond the scope of the function that allocates them. That's because SQLite
 * resources are not thread safe.
 */

#include <memory>
#include <sqlite3.h>

struct sqlite_finalizer {
    void operator()(sqlite3*& db) const {
        sqlite3_close(db);
    }
};

struct sqlite_stmt_finalizer {
    void operator()(sqlite3_stmt*& stmt) const {
        sqlite3_finalize(stmt);
    }
};

using sqlite_handle = std::unique_ptr<sqlite3, sqlite_finalizer>;
using sqlite_stmt_handle = std::unique_ptr<sqlite3_stmt, sqlite_stmt_finalizer>;

#endif
