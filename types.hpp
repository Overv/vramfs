#ifndef VRAM_TYPES_HPP
#define VRAM_TYPES_HPP

#include <thread>
#include <memory>
#include <sqlite3.h>

/*
 * Desired result from index search functions
 */

namespace entry_filter {
    enum entry_filter_t {
        file,
        directory,
        all
    };
}

/*
 * Object for automatically acquiring and releasing a mutex
 */

class scoped_lock {
    std::mutex& mutex;
public:
    scoped_lock(std::mutex& mutex) : mutex(mutex) { mutex.lock(); }
    ~scoped_lock() { mutex.unlock(); }
};

/*
 * Wrappers around SQLite types for memory management
 */

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
