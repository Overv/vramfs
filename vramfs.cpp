// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <sqlite3.h>
#include <unistd.h>

// Use minimal OpenCL implementation for better debugging with valgrind
#ifdef DEBUG
#include "extras/debugcl.hpp"
#else
#include <CL/cl.hpp>
#endif

// Standard library
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <mutex>
#include <unordered_map>

// Internal dependencies
#include "types.hpp"

// Configuration
static const char* entries_table_sql =
    "CREATE TABLE entries(" \
        // Automatic alias of unique ROWID
        "id INTEGER PRIMARY KEY," \
        "parent INTEGER DEFAULT 0," \
        "name TEXT NOT NULL," \
        "dir INTEGER DEFAULT 0," \
        "mode INTEGER DEFAULT 0," \
        // Default size for directories
        "size INTEGER DEFAULT 4096," \
        // Numeric version of CURRENT_TIMESTAMP
        "atime INTEGER DEFAULT (STRFTIME('%s'))," \
        "mtime INTEGER DEFAULT (STRFTIME('%s'))," \
        "ctime INTEGER DEFAULT (STRFTIME('%s'))" \
    ")";

static const char* blocks_table_sql =
    "CREATE TABLE blocks(" \
        "entry INTEGER NOT NULL," \
        // Start offset of block
        "off INTEGER NOT NULL," \
        // OpenCL buffer
        "buffer INTEGER NOT NULL" \
    ")";

static const size_t BLOCK_SIZE = 4096;

static const int ROOT_PARENT = 0;
static const int ROOT_ENTRY = 1;

static const int DEFAULT_FILE_MODE = 0664;
static const int DEFAULT_DIR_MODE = 0775;

/*
 * Globals
 */

// Lock to prevent multiple threads from manipulating the file system index and
// OpenCL buffers simultaneously. The tiny overhead is worth not having to deal
// with the uncountable amount of race conditions that would otherwise occur.
static std::mutex fslock;

// Connection to file system database
static sqlite3* db;

// OpenCL context
static cl::Context* ocl_context;
static cl::CommandQueue* ocl_queue;

// Prepared statement cache
static std::unordered_map<std::string, sqlite3_stmt*> prepared_statements;

/*
 * Helpers
 */

// Error function that can be combined with a return statement to return *ret*
template<typename T>
static T fatal_error(const char* error, T ret) {
    std::cerr << "error: " << error << std::endl;
    fuse_exit(fuse_get_context()->fuse);
    return ret;
}

static sqlite3_stmt* prepare_query(sqlite3* db, const char* query) {
    auto it = prepared_statements.find(query);
    sqlite3_stmt* stmt = nullptr;

    if (it == prepared_statements.end()) {
        sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
        prepared_statements[query] = stmt;
    } else {
        stmt = prepared_statements[query];
    }

    sqlite3_reset(stmt);

    return stmt;
}

// Split path/to/file.txt into "path/to" and "file.txt"
static void split_file_path(const std::string& path, std::string& dir, std::string& file) {
    size_t p = path.rfind("/");

    if (p == std::string::npos) {
        dir = "";
        file = path;
    } else {
        dir = path.substr(0, p);
        file = path.substr(p + 1);
    }
}

// Delete a file entry and its blocks
static int delete_file(sqlite3* db, int64_t entry) {
    // Delete blocks and associated OpenCL buffers
    auto stmt = prepare_query(db, "SELECT buffer FROM blocks WHERE entry = ?");
    sqlite3_bind_int64(stmt, 1, entry);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        delete reinterpret_cast<cl::Buffer*>(sqlite3_column_int64(stmt, 0));
    }

    stmt = prepare_query(db, "DELETE FROM blocks WHERE entry = ?");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_step(stmt);

    // Remove file
    stmt = prepare_query(db, "DELETE FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_step(stmt);

    return 0;
}

// Find entry by path (starting with /)
static int64_t index_find(sqlite3* db, const char* path, entry_filter::entry_filter_t filter = entry_filter::all) {
    // Prepare entry lookup query
    auto stmt = prepare_query(db, "SELECT id, dir FROM entries WHERE parent = ? AND name = ? LIMIT 1");
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
        sqlite3_bind_int64(stmt, 1, entry);
        sqlite3_bind_text(stmt, 2, part.c_str(), -1, SQLITE_TRANSIENT);
        int r = sqlite3_step(stmt);

        // If entry was not found, abort
        if (r != SQLITE_ROW) {
            entry = -ENOENT;
            break;
        }

        // Continue with entry as new current directory (if not end of path)
        entry = sqlite3_column_int64(stmt, 0);
        dir = sqlite3_column_int(stmt, 1);

        sqlite3_reset(stmt);
    }

    // If the path is empty, assume the root directory
    if (!path[0]) entry = ROOT_ENTRY;

    // If an undesired type of entry was found, return an error
    if (entry > 0) {
        if (filter == entry_filter::directory && !dir) {
            entry = -ENOTDIR;
        } else if (filter == entry_filter::file && dir) {
            entry = -EISDIR;
        }
    }

    // Return final entry or error
    return entry;
}

/*
 * Initialisation
 */

static void* vram_init(fuse_conn_info* conn) {
    // Create in-memory file system database
    int r = sqlite3_open(":memory:", &db);
    if (r) {
        sqlite3_close(db);
        return fatal_error("failed to create index db", nullptr);
    }

    r = sqlite3_exec(db, entries_table_sql, nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create index table", nullptr);

    r = sqlite3_exec(db, "CREATE INDEX idx_name ON entries (parent, name)", nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create db index", nullptr);

    r = sqlite3_exec(db, blocks_table_sql, nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create blocks table", nullptr);

    r = sqlite3_exec(db, "CREATE INDEX idx_block ON blocks (entry, off)", nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create block index", nullptr);

    // Add root directory, which is its own parent
    r = sqlite3_exec(db, "INSERT INTO entries (id, name, dir) VALUES (1, '', 1)", nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create root directory", nullptr);

    // Create OpenCL context
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);
    if (platforms.size() == 0) return fatal_error("no opencl platform found", nullptr);

    std::vector<cl::Device> gpu_devices;
    platforms[0].getDevices(CL_DEVICE_TYPE_GPU, &gpu_devices);
    if (gpu_devices.size() == 0) return fatal_error("no opencl capable gpu found", nullptr);

    ocl_context = new cl::Context(gpu_devices[0]);
    ocl_queue = new cl::CommandQueue(*ocl_context, gpu_devices[0]);

    return db;
}

/*
 * Entry attributes
 */

static int vram_getattr(const char* path, struct stat* stbuf) {
    scoped_lock local_lock(fslock);

    // Look up entry
    int64_t entry = index_find(db, path);
    if (entry < 0) return entry;

    // Load all info about the entry
    auto stmt = prepare_query(db, "SELECT dir, mode, size, atime, mtime, ctime FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_step(stmt);

    memset(stbuf, 0, sizeof(struct stat));

    bool dir = sqlite3_column_int(stmt, 0);
    int mode = sqlite3_column_int(stmt, 1);

    if (dir) {
        stbuf->st_mode = S_IFDIR | mode;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | mode;
        stbuf->st_nlink = 1;
    }

    stbuf->st_uid = geteuid();
    stbuf->st_gid = getegid();
    stbuf->st_size = sqlite3_column_int64(stmt, 2);
    stbuf->st_atime = sqlite3_column_int64(stmt, 3);
    stbuf->st_mtime = sqlite3_column_int64(stmt, 4);
    stbuf->st_ctime = sqlite3_column_int64(stmt, 5);

    return 0;
}

/*
 * Set the mode bits of an entry
 */

static int vram_chmod(const char* path, mode_t mode) {
    scoped_lock local_lock(fslock);

    // Look up entry
    int64_t entry = index_find(db, path);
    if (entry < 0) return entry;

    // Update mode
    auto stmt = prepare_query(db, "UPDATE entries SET mode = ? WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, mode);
    sqlite3_bind_int64(stmt, 2, entry);
    sqlite3_step(stmt);

    return 0;
}

/*
 * Set the last access and last modified times of an entry
 */

static int vram_utimens(const char* path, const timespec tv[2]) {
    scoped_lock local_lock(fslock);

    // Look up entry
    int64_t entry = index_find(db, path);
    if (entry < 0) return entry;

    // Update times (ignore nanosecond part)
    auto stmt = prepare_query(db, "UPDATE entries SET atime = ?, mtime = ?, ctime = STRFTIME('%s') WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, tv[0].tv_sec);
    sqlite3_bind_int64(stmt, 2, tv[1].tv_sec);
    sqlite3_bind_int64(stmt, 3, entry);
    sqlite3_step(stmt);

    return 0;
}

/*
 * Directory listing
 */

static int vram_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, fuse_file_info*) {
    scoped_lock local_lock(fslock);

    // Look up directory
    int64_t entry = index_find(db, path, entry_filter::directory);

    if (entry > 0) {
        // List directory contents
        auto stmt = prepare_query(db, "SELECT name FROM entries WHERE parent = ?");
        sqlite3_bind_int64(stmt, 1, entry);

        // Required default entries
        filler(buf, ".", nullptr, 0);
        filler(buf, "..", nullptr, 0);

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt, 0);
            filler(buf, reinterpret_cast<const char*>(name), nullptr, 0);
        }

        return 0;
    } else {
        // Error instead of entry
        return entry;
    }
}

/*
 * Create file
 */

static int vram_create(const char* path, mode_t, struct fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    // Fail if file already exists
    int64_t entry = index_find(db, path);
    if (entry > 0) return -EEXIST;

    // Split path in directory and file name parts
    std::string dir, file;
    split_file_path(path, dir, file);

    // Check if directory exists
    entry = index_find(db, dir.c_str(), entry_filter::directory);
    if (entry < 0) return entry;

    // Create new file
    auto stmt = prepare_query(db, "INSERT INTO entries (parent, name, mode, size) VALUES (?, ?, ?, 0)");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_bind_text(stmt, 2, file.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, DEFAULT_FILE_MODE);
    sqlite3_step(stmt);

    // Open it by assigning new file handle
    fi->fh = sqlite3_last_insert_rowid(db);

    return 0;
}

/*
 * Create directory
 */

static int vram_mkdir(const char* path, mode_t) {
    scoped_lock local_lock(fslock);

    // Fail if directory already exists
    int64_t entry = index_find(db, path);
    if (entry > 0) return -EEXIST;

    // Split path in parent directory and new directory name parts
    std::string parent, dir;
    split_file_path(path, parent, dir);

    // Check if parent directory exists
    entry = index_find(db, parent.c_str(), entry_filter::directory);
    if (entry < 0) return entry;

    // Create new directory
    auto stmt = prepare_query(db, "INSERT INTO entries (parent, name, mode, dir, size) VALUES (?, ?, ?, 1, 0)");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_bind_text(stmt, 2, dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, DEFAULT_DIR_MODE);
    sqlite3_step(stmt);

    return 0;
}

/*
 * Delete file
 *
 * NOTE: FUSE will only call this function once the last handle to a file has
 * been closed.
 */

static int vram_unlink(const char* path) {
    scoped_lock local_lock(fslock);

    // Fail if entry doesn't exist or is a directory
    int64_t entry = index_find(db, path, entry_filter::file);
    if (entry < 0) return entry;

    delete_file(db, entry);

    return 0;
}

/*
 * Delete directory
 */

static int vram_rmdir(const char* path) {
    scoped_lock local_lock(fslock);

    // Fail if entry doesn't exist or is a file
    int64_t entry = index_find(db, path, entry_filter::directory);
    if (entry < 0) return entry;

    // Check if directory is empty
    auto stmt = prepare_query(db, "SELECT COUNT(*) FROM entries WHERE parent = ?");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_step(stmt);

    if (sqlite3_column_int64(stmt, 0) != 0) {
        return -ENOTEMPTY;
    }

    // Remove directory
    stmt = prepare_query(db, "DELETE FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_step(stmt);

    return 0;
}

/*
 * Rename entry
 */

static int vram_rename(const char* path, const char* new_path) {
    scoped_lock local_lock(fslock);

    // Look up entry
    int64_t entry = index_find(db, path);
    if (entry < 0) return entry;

    // Check if destination directory exists
    std::string dir, new_name;
    split_file_path(new_path, dir, new_name);

    int64_t parent = index_find(db, dir.c_str(), entry_filter::directory);
    if (parent < 0) return parent;

    // If the destination file already exists, then delete it
    int64_t dest_entry = index_find(db, new_path);
    if (dest_entry >= 0) delete_file(db, dest_entry);

    // Update index
    auto stmt = prepare_query(db, "UPDATE entries SET parent = ?, name = ? WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, parent);
    sqlite3_bind_text(stmt, 2, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, entry);
    sqlite3_step(stmt);

    return 0;
}

/*
 * Open file
 */

static int vram_open(const char* path, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    // Look up file
    int64_t entry = index_find(db, path, entry_filter::file);

    if (entry > 0) {
        fi->fh = entry;
        return 0;
    } else {
        // Return entry find error if it wasn't found
        return entry;
    }
}

/*
 * Read file
 *
 * TODO: Handle multiple blocks
 */

static int vram_read(const char* path, char* buf, size_t size, off_t off, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    // Get file size to determine how much to read
    auto stmt = prepare_query(db, "SELECT size FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, fi->fh);
    sqlite3_step(stmt);
    size_t file_size = sqlite3_column_int64(stmt, 0);

    if ((size_t) off >= file_size) return 0;
    size_t read_size = std::min(file_size - off, size);

    // Find block corresponding to read region
    off_t block_start = (off / BLOCK_SIZE) * BLOCK_SIZE;
    off_t block_off = off - block_start;

    // Get buffer for block
    stmt = prepare_query(db, "SELECT buffer FROM blocks WHERE entry = ? AND off = ?");
    sqlite3_bind_int64(stmt, 1, fi->fh);
    sqlite3_bind_int64(stmt, 2, block_start);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        cl::Buffer* ocl_buf = reinterpret_cast<cl::Buffer*>(sqlite3_column_int64(stmt, 0));
        ocl_queue->enqueueReadBuffer(*ocl_buf, true, block_off, read_size, buf, nullptr, nullptr);
    } else {
        // Non-written part of file
        memset(buf, 0, read_size);
    }

    return read_size;
}

/*
 * Write file
 *
 * TODO: Handle multiple blocks
 */

static int vram_write(const char* path, const char* buf, size_t size, off_t off, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    // Find block corresponding to write region
    off_t block_start = (off / BLOCK_SIZE) * BLOCK_SIZE;
    off_t block_off = off - block_start;

    // Get buffer for block or allocate a new one
    auto stmt = prepare_query(db, "SELECT buffer FROM blocks WHERE entry = ? AND off = ?");
    sqlite3_bind_int64(stmt, 1, fi->fh);
    sqlite3_bind_int64(stmt, 2, block_start);

    cl::Buffer* ocl_buf = nullptr;
    int r;

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        ocl_buf = reinterpret_cast<cl::Buffer*>(sqlite3_column_int64(stmt, 0));
    } else {
        ocl_buf = new cl::Buffer(*ocl_context, CL_MEM_READ_WRITE, BLOCK_SIZE, nullptr, &r);
        if (r != CL_SUCCESS) return fatal_error("failed to allocate opencl buffer", -ENOMEM);

        r = ocl_queue->enqueueFillBuffer(*ocl_buf, 0, 0, BLOCK_SIZE, nullptr, nullptr);
        if (r != CL_SUCCESS) return fatal_error("failed to initialise opencl buffer", -EIO);

        stmt = prepare_query(db, "INSERT INTO blocks (entry, off, buffer) VALUES (?, ?, ?)");
        sqlite3_bind_int64(stmt, 1, fi->fh);
        sqlite3_bind_int64(stmt, 2, block_start);
        sqlite3_bind_int64(stmt, 3, reinterpret_cast<int64_t>(ocl_buf));
        sqlite3_step(stmt);
    }

    // Write to block
    ocl_queue->enqueueWriteBuffer(*ocl_buf, true, block_off, size, buf, nullptr, nullptr);

    // Update file size
    stmt = prepare_query(db, "UPDATE entries SET size = MAX(size, ?) WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, size + off);
    sqlite3_bind_int64(stmt, 2, fi->fh);
    sqlite3_step(stmt);

    return size;
}

/*
 * Change file size
 */

static int vram_truncate(const char* path, off_t size) {
    scoped_lock local_lock(fslock);

    // Look up file
    int64_t entry = index_find(db, path);
    if (entry < 0) return entry;

    // Update size
    auto stmt = prepare_query(db, "UPDATE entries SET size = ? WHERE id = ?");
    sqlite3_bind_int64(stmt, 1, size);
    sqlite3_bind_int64(stmt, 2, entry);
    sqlite3_step(stmt);

    // Discard blocks beyond the new file size
    stmt = prepare_query(db, "SELECT buffer FROM blocks WHERE entry = ? AND off >= ?");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_bind_int64(stmt, 2, size);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        delete reinterpret_cast<cl::Buffer*>(sqlite3_column_int64(stmt, 0));
    }

    stmt = prepare_query(db, "DELETE FROM blocks WHERE entry = ? AND off >= ?");
    sqlite3_bind_int64(stmt, 1, entry);
    sqlite3_bind_int64(stmt, 2, size);
    sqlite3_step(stmt);

    return 0;
}

/*
 * Clean up
 */

static void vram_destroy(void* userdata) {
    // Clean up active OpenCL buffers
    auto stmt = prepare_query(db, "SELECT buffer FROM blocks");

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        delete reinterpret_cast<cl::Buffer*>(sqlite3_column_int64(stmt, 0));
    }

    // Clean up OpenCL context
    delete ocl_queue;
    delete ocl_context;

    // Clean up prepared statements
    for (auto& pair : prepared_statements) {
        sqlite3_finalize(pair.second);
    }

    // Clean up index database
    sqlite3_close(reinterpret_cast<sqlite3*>(userdata));
}

/*
 * FUSE setup
 */

static struct vram_operations : fuse_operations {
    vram_operations() {
        init = vram_init;
        getattr = vram_getattr;
        utimens = vram_utimens;
        chmod = vram_chmod;
        readdir = vram_readdir;
        create = vram_create;
        mkdir = vram_mkdir;
        unlink = vram_unlink;
        rmdir = vram_rmdir;
        rename = vram_rename;
        open = vram_open;
        read = vram_read;
        write = vram_write;
        truncate = vram_truncate;
        destroy = vram_destroy;
    }
} operations;

int main(int argc, char* argv[]) {
    return fuse_main(argc, argv, &operations, nullptr);
}
