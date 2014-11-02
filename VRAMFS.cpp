// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <sqlite3.h>
#include <unistd.h>

// Standard library
#include <iostream>
#include <cstring>

// Configuration
const char* entries_table_sql =
    "CREATE TABLE entries(" \
        // Automatic alias of unique ROWID
        "id INTEGER PRIMARY KEY," \
        "parent INTEGER," \
        "name TEXT NOT NULL," \
        "directory INTEGER," \
        "size INTEGER DEFAULT 4096," \
        // Numeric version of CURRENT_TIMESTAMP
        "atime INTEGER DEFAULT (STRFTIME('%s'))," \
        "mtime INTEGER DEFAULT (STRFTIME('%s'))," \
        "ctime INTEGER DEFAULT (STRFTIME('%s'))" \
    ")";

const char* root_entry_sql =
    "INSERT INTO entries (id, parent, name, directory) VALUES (1, 1, '', 1);";

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

// Used by each function to get a new connection to the index (thread safety)
static sqlite3* open_index() {
    // In-memory database that is shared between multiple threads
    sqlite3* db;
    int ret = sqlite3_open("file::memory:?cache=shared", &db);

    if (ret) {
        sqlite3_close(db);
        return nullptr;
    } else {
        return db;
    }
}

static void close_index(sqlite3* db) {
    sqlite3_close(db);
}

//
// Initialisation
//

static void* vram_init(fuse_conn_info* conn) {
    // Create file system index
    sqlite3_config(SQLITE_CONFIG_URI, 1);
    sqlite3* db = open_index();
    if (!db) return fatal_error("failed to create index db", nullptr);

    int r = sqlite3_exec(db, entries_table_sql, nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create index table", nullptr);

    // Add root directory, which is its own parent
    r = sqlite3_exec(db, root_entry_sql, nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create root directory", nullptr);

    return db;
}

//
// File attributes
//

static int vram_getattr(const char* path, struct stat* stbuf) {
    sqlite3* db = open_index();
    if (!db) return fatal_error("failed to access index db", -EAGAIN);

    int ret = 0;
    if (strcmp(path, "/") == 0) {
        // Look up root directory
        sqlite3_stmt* stmt;
        sqlite3_prepare_v2(db, "SELECT size, atime, mtime, ctime FROM entries WHERE id = 1", -1, &stmt, nullptr);
        if (!stmt) return fatal_error("failed to query entry", -EAGAIN);

        int r = sqlite3_step(stmt);
        if (r != SQLITE_ROW) return fatal_error("no root directory", -EAGAIN);

        // Return info
        memset(stbuf, 0, sizeof(struct stat));
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = geteuid();
        stbuf->st_gid = getegid();
        stbuf->st_size = sqlite3_column_int64(stmt, 0);
        stbuf->st_atime = sqlite3_column_int64(stmt, 1);
        stbuf->st_mtime = sqlite3_column_int64(stmt, 2);
        stbuf->st_ctime = sqlite3_column_int64(stmt, 3);

        sqlite3_finalize(stmt);
    } else {
        ret = -ENOENT;
    }

    close_index(db);

    return ret;
}

//
// Directory listing
//

static int vram_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info* fi) {
    if (strcmp(path, "/") == 0) {
        filler(buf, ".", nullptr, 0);
        filler(buf, "..", nullptr, 0);
        return 0;
    } else {
        return -ENOENT;
    }
}

//
// Clean up
//

static void vram_destroy(void* userdata) {
    close_index(reinterpret_cast<sqlite3*>(userdata));
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