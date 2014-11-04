// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <sqlite3.h>
#include <unistd.h>

// Standard library
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdint>

// Internal dependencies
#include "resources.hpp"

// Configuration
static const char* entries_table_sql =
    "CREATE TABLE entries(" \
        // Automatic alias of unique ROWID
        "id INTEGER PRIMARY KEY," \
        "parent INTEGER DEFAULT 0," \
        "name TEXT NOT NULL," \
        "dir INTEGER," \
        "size INTEGER DEFAULT 4096," \
        // Numeric version of CURRENT_TIMESTAMP
        "atime INTEGER DEFAULT (STRFTIME('%s'))," \
        "mtime INTEGER DEFAULT (STRFTIME('%s'))," \
        "ctime INTEGER DEFAULT (STRFTIME('%s'))" \
    ")";

static const char* root_entry_sql =
    "INSERT INTO entries (id, name, dir) VALUES (1, '', 1);";

static const int ROOT_PARENT = 0;
static const int ROOT_ENTRY = 1;

//
// Helpers
//

// Error function that can be combined with a return statement to return *ret*
template<typename T>
static T fatal_error(const char* error, T ret) {
    std::cerr << "error: " << error << std::endl;
    fuse_exit(fuse_get_context()->fuse);
    return ret;
}

static sqlite_stmt_handle prepare_query(const sqlite_handle& db, const char* query) {
    sqlite3_stmt* stmt;
    sqlite3_prepare_v2(db.get(), query, -1, &stmt, nullptr);
    return sqlite_stmt_handle(stmt);
}

// Used by each function to get a new connection to the index (thread safety)
static sqlite_handle index_open() {
    // In-memory database that is shared between multiple threads
    sqlite3* db;
    int ret = sqlite3_open("file::memory:?cache=shared", &db);

    if (ret) {
        sqlite3_close(db);
        return nullptr;
    } else {
        return sqlite_handle(db);
    }
}

// Find entry by path (starting with /)
static int64_t index_find(const sqlite_handle& db, const char* path, bool dironly = false) {
    // Prepare entry lookup query
    sqlite_stmt_handle stmt = prepare_query(db, "SELECT id, dir FROM entries WHERE parent = ? AND name = ? LIMIT 1");
    if (!stmt) return fatal_error("failed to query entry", -EAGAIN);

    // Traverse file system by hierarchically, starting from root directory
    int64_t entry = ROOT_PARENT;
    bool dir = true;

    std::stringstream stream(path);
    std::string part;

    while (getline(stream, part, '/')) {
        // If current directory is actually a file, abort
        if (!dir) {
            entry = -ENOTDIR;
            break;
        }

        // Look up corresponding entry in the current directory
        sqlite3_bind_int64(stmt.get(), 1, entry);
        sqlite3_bind_text(stmt.get(), 2, part.c_str(), -1, SQLITE_TRANSIENT);
        int r = sqlite3_step(stmt.get());

        // If entry was not found, abort
        if (r != SQLITE_ROW) {
            entry = -ENOENT;
            break;
        }

        // Continue with entry as new current directory (if not end of path)
        entry = sqlite3_column_int64(stmt.get(), 0);
        dir = sqlite3_column_int(stmt.get(), 1);

        sqlite3_reset(stmt.get());
    }

    // If a non-directory entry was found, but a dir was required, return error
    if (entry > 0 && dironly && !dir) {
        entry = -ENOTDIR;
    }

    // Return final entry or error
    return entry;
}

//
// Initialisation
//

static void* vram_init(fuse_conn_info* conn) {
    // Create file system index
    sqlite3_config(SQLITE_CONFIG_URI, true);
    sqlite_handle db = index_open();
    if (!db) return fatal_error("failed to create index db", nullptr);

    int r = sqlite3_exec(db.get(), entries_table_sql, nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create index table", nullptr);

    // Add root directory, which is its own parent
    r = sqlite3_exec(db.get(), root_entry_sql, nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create root directory", nullptr);

    return db.release();
}

//
// File attributes
//

static int vram_getattr(const char* path, struct stat* stbuf) {
    sqlite_handle db = index_open();

    int ret = 0;
    if (strcmp(path, "/") == 0) {
        // Look up root directory
        sqlite_stmt_handle stmt = prepare_query(db, "SELECT size, atime, mtime, ctime FROM entries WHERE id = 1");
        if (!stmt) return fatal_error("failed to query entry", -EAGAIN);

        int r = sqlite3_step(stmt.get());
        if (r != SQLITE_ROW) return fatal_error("no root directory", -EAGAIN);

        // Return info
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = geteuid();
        stbuf->st_gid = getegid();
        stbuf->st_size = sqlite3_column_int64(stmt.get(), 0);
        stbuf->st_atime = sqlite3_column_int64(stmt.get(), 1);
        stbuf->st_mtime = sqlite3_column_int64(stmt.get(), 2);
        stbuf->st_ctime = sqlite3_column_int64(stmt.get(), 3);
    } else {
        ret = -ENOENT;
    }

    return ret;
}

//
// Directory listing
//

static int vram_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    sqlite_handle db = index_open();

    // Look up directory
    int64_t entry = index_find(db, path, true);

    if (entry > 0) {
        // List directory contents
        sqlite_stmt_handle stmt = prepare_query(db, "SELECT name FROM entries WHERE parent = ?");
        sqlite3_bind_int64(stmt.get(), 1, entry);

        // Required default entries
        filler(buf, ".", nullptr, 0);
        filler(buf, "..", nullptr, 0);

        while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt.get(), 0);
            filler(buf, reinterpret_cast<const char*>(name), nullptr, 0);
        }

        return 0;
    } else {
        // Error instead of entry
        return entry;
    }
}

//
// Clean up
//

static void vram_destroy(void* userdata) {
    sqlite3_close(reinterpret_cast<sqlite3*>(userdata));
}

//
// FUSE setup
//

static struct vram_operations : fuse_operations {
    vram_operations() {
        init = vram_init;
        getattr = vram_getattr;
        readdir = vram_readdir;
        destroy = vram_destroy;
    }
} operations;

int main(int argc, char* argv[]) {
    return fuse_main(argc, argv, &operations, nullptr);
}
