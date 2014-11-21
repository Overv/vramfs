// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <sqlite3.h>
#include <unistd.h>
#include <CL/cl.hpp>

// Standard library
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdint>
#include <mutex>

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
        "size INTEGER DEFAULT 4096," \
        // Numeric version of CURRENT_TIMESTAMP
        "atime INTEGER DEFAULT (STRFTIME('%s'))," \
        "mtime INTEGER DEFAULT (STRFTIME('%s'))," \
        "ctime INTEGER DEFAULT (STRFTIME('%s'))," \
        // OpenCL buffer
        "buffer INTEGER DEFAULT 0" \
    ")";

static const char* root_entry_sql =
    "INSERT INTO entries (id, name, dir) VALUES (1, '', 1);";

static const int ROOT_PARENT = 0;
static const int ROOT_ENTRY = 1;

/*
 * Globals
 */

// Lock to prevent multiple threads from manipulating the file system index and
// OpenCL buffers simultaneously. The tiny overhead is worth not having to deal
// with the uncountable amount of race conditions that would otherwise occur.
std::mutex fslock;

// Connection to file system database
sqlite3* db;

// OpenCL context
cl::Context* ocl_context;
cl::CommandQueue* ocl_queue;

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

static sqlite_stmt_handle prepare_query(sqlite3* db, const char* query) {
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db, query, -1, &stmt, nullptr);
    return sqlite_stmt_handle(stmt);
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

// Retrieve current file buffer and entry
static void get_buffer(sqlite3* db, int64_t entry, cl::Buffer** buffer = nullptr, size_t* size = nullptr) {
    auto stmt = prepare_query(db, "SELECT buffer, size FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt.get(), 1, entry);
    sqlite3_step(stmt.get());

    if (buffer)
        *buffer = reinterpret_cast<cl::Buffer*>(sqlite3_column_int64(stmt.get(), 0));
    if (size)
        *size = sqlite3_column_int64(stmt.get(), 1);
}

// Set file buffer and size of entry
static void set_buffer(sqlite3* db, int64_t entry, cl::Buffer* buffer, size_t size) {
    auto stmt = prepare_query(db, "UPDATE entries SET buffer = ?, size = ? WHERE id = ?");
    sqlite3_bind_int64(stmt.get(), 1, reinterpret_cast<int64_t>(buffer));
    sqlite3_bind_int64(stmt.get(), 2, size);
    sqlite3_bind_int64(stmt.get(), 3, entry);
    sqlite3_step(stmt.get());
}

// Resize file buffer (updates database)
static int resize_buffer(sqlite3* db, int64_t entry, size_t new_size, cl::Buffer** new_buf = nullptr) {
    // Get current buffer and file size
    int r;
    cl::Buffer* ocl_buf;
    size_t current_size;
    get_buffer(db, entry, &ocl_buf, &current_size);

    // Allocate new larger buffer if size > 0
    cl::Buffer* old_ocl_buf = ocl_buf;

    if (new_size != 0) {
        ocl_buf = new cl::Buffer(*ocl_context, CL_MEM_READ_WRITE, new_size, nullptr, &r);
        if (r != CL_SUCCESS) return fatal_error("failed to allocate opencl buffer", -ENOMEM);

        // Initialise buffer with zeroes and then copy any old data
        r = ocl_queue->enqueueFillBuffer(*ocl_buf, 0, 0, new_size, nullptr, nullptr);
        if (r != CL_SUCCESS) return fatal_error("failed to initialise opencl buffer", -EIO);

        if (old_ocl_buf) {
            size_t copy_size = std::min(new_size, current_size);
            r = ocl_queue->enqueueCopyBuffer(*old_ocl_buf, *ocl_buf, 0, 0, copy_size, nullptr, nullptr);
            if (r != CL_SUCCESS) return fatal_error("failed to copy opencl buffer", -EIO);
        }
    } else {
        ocl_buf = nullptr;
    }

    // Dispose of any old buffer
    delete old_ocl_buf;

    // Update file database
    set_buffer(db, entry, ocl_buf, new_size);

    if (new_buf) *new_buf = ocl_buf;

    return 0;
}

// Delete a file entry and its buffer
static int delete_file(sqlite3* db, int64_t entry) {
    // Delete OpenCL buffer
    cl::Buffer* ocl_buf;
    get_buffer(db, entry, &ocl_buf, nullptr);
    delete ocl_buf;

    // Remove file
    auto stmt = prepare_query(db, "DELETE FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt.get(), 1, entry);
    sqlite3_step(stmt.get());

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
    // Create in-memory file system index in standard serialized mode
    int r = sqlite3_open(":memory:", &db);
    if (r) {
        sqlite3_close(db);
        return fatal_error("failed to create index db", nullptr);
    }

    r = sqlite3_exec(db, entries_table_sql, nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create index table", nullptr);

    r = sqlite3_exec(db, "CREATE INDEX idx_name ON entries (parent, name)", nullptr, nullptr, nullptr);
    if (r) return fatal_error("failed to create db index", nullptr);

    // Add root directory, which is its own parent
    r = sqlite3_exec(db, root_entry_sql, nullptr, nullptr, nullptr);
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
    auto stmt = prepare_query(db, "SELECT dir, size, atime, mtime, ctime FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt.get(), 1, entry);
    sqlite3_step(stmt.get());

    memset(stbuf, 0, sizeof(struct stat));

    if (sqlite3_column_int(stmt.get(), 0)) {
        stbuf->st_mode = S_IFDIR | 0700;
        stbuf->st_nlink = 2;
    } else {
        stbuf->st_mode = S_IFREG | 0600;
        stbuf->st_nlink = 1;
    }

    stbuf->st_uid = geteuid();
    stbuf->st_gid = getegid();
    stbuf->st_size = sqlite3_column_int64(stmt.get(), 1);
    stbuf->st_atime = sqlite3_column_int64(stmt.get(), 2);
    stbuf->st_mtime = sqlite3_column_int64(stmt.get(), 3);
    stbuf->st_ctime = sqlite3_column_int64(stmt.get(), 4);

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
    sqlite3_bind_int64(stmt.get(), 1, tv[0].tv_sec);
    sqlite3_bind_int64(stmt.get(), 2, tv[1].tv_sec);
    sqlite3_bind_int64(stmt.get(), 3, entry);
    sqlite3_step(stmt.get());

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
    auto stmt = prepare_query(db, "INSERT INTO entries (parent, name, size) VALUES (?, ?, 0)");
    sqlite3_bind_int64(stmt.get(), 1, entry);
    sqlite3_bind_text(stmt.get(), 2, file.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt.get());

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
    auto stmt = prepare_query(db, "INSERT INTO entries (parent, name, dir, size) VALUES (?, ?, 1, 0)");
    sqlite3_bind_int64(stmt.get(), 1, entry);
    sqlite3_bind_text(stmt.get(), 2, dir.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt.get());

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
    sqlite3_bind_int64(stmt.get(), 1, entry);
    sqlite3_step(stmt.get());

    if (sqlite3_column_int64(stmt.get(), 0) != 0) {
        return -ENOTEMPTY;
    }

    // Remove directory
    stmt = prepare_query(db, "DELETE FROM entries WHERE id = ?");
    sqlite3_bind_int64(stmt.get(), 1, entry);
    sqlite3_step(stmt.get());

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
    sqlite3_bind_int64(stmt.get(), 1, parent);
    sqlite3_bind_text(stmt.get(), 2, new_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt.get(), 3, entry);
    sqlite3_step(stmt.get());

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
 */

static int vram_read(const char* path, char* buf, size_t size, off_t off, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    // Get associated OpenCL buffer and file size
    cl::Buffer* ocl_buf;
    size_t entry_size;
    get_buffer(db, fi->fh, &ocl_buf, &entry_size);
    if (ocl_buf == nullptr) return 0;

    // Cut off read if it exceeds EOF
    if ((size_t) off >= entry_size) return 0;
    else if (off + size > entry_size) size = entry_size - off;

    // Read data
    int r = ocl_queue->enqueueReadBuffer(*ocl_buf, true, off, size, buf, nullptr, nullptr);
    if (r != CL_SUCCESS) return 0;

    return size;
}

/*
 * Write file
 */

static int vram_write(const char* path, const char* buf, size_t size, off_t off, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    if (size == 0) return 0;

    // Get current buffer and file size
    int r;
    cl::Buffer* ocl_buf;
    size_t current_size;
    get_buffer(db, fi->fh, &ocl_buf, &current_size);

    // Resize buffer if necessary
    if (off + size > current_size) {
        r = resize_buffer(db, fi->fh, off + size, &ocl_buf);
        if (r < 0) return r;
    }

    // Copy contents to it
    r = ocl_queue->enqueueWriteBuffer(*ocl_buf, true, off, size, buf, nullptr, nullptr);
    if (r != CL_SUCCESS) return fatal_error("failed to write to opencl buffer", -EIO);

    return size;
}

/*
 * Change file size
 */

static int vram_truncate(const char* path, off_t size) {
    scoped_lock local_lock(fslock);

    // Look up entry
    int64_t entry = index_find(db, path, entry_filter::file);
    if (entry < 0) return entry;

    // Resize file
    int r = resize_buffer(db, entry, size);
    if (r < 0) return r;

    return 0;
}

/*
 * Clean up
 */

static void vram_destroy(void* userdata) {
    // Clean up active OpenCL buffers
    auto stmt = prepare_query(db, "SELECT buffer FROM entries");

    while (sqlite3_step(stmt.get()) == SQLITE_ROW) {
        delete reinterpret_cast<cl::Buffer*>(sqlite3_column_int64(stmt.get(), 0));
    }

    // Clean up OpenCL context
    delete ocl_queue;
    delete ocl_context;

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
