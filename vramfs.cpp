// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <unistd.h>
#include <sqlite3.h>

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
#include <map>
#include <unordered_set>

// Internal dependencies
#include "types.hpp"

// Nicely fits FUSE read/write size
static const size_t BLOCK_SIZE = 128 * 1024;

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

// File system index
static std::unordered_set<shared_ptr<entry_t>> entries;
static shared_ptr<entry_t> root_entry;

// OpenCL context, GPU device and prototype queue to create others from
static cl::Context ocl_context;
static cl::Device ocl_device;
static cl::CommandQueue ocl_proto_queue;

// File blocks
static std::map<entry_off, cl::Buffer> file_blocks;

/*
 * Helpers
 */

// Error function that can be combined with a return statement to return *ret*
template<typename T>
static T fatal_error(const std::string& error, T ret) {
    std::cerr << "error: " << error << std::endl;
    fuse_exit(fuse_get_context()->fuse);
    return ret;
}

// Get current time with nanosecond precision
static timespec get_time() {
    timespec tv;
    clock_gettime(CLOCK_REALTIME_COARSE, &tv);
    return tv;
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

    if (dir.size() == 0) dir = "/";
}

// Get the OpenCL buffer of the block if it exists (returning true)
static bool get_block(shared_ptr<entry_t> entry, off_t off, cl::Buffer& buf) {
    auto it = file_blocks.find(entry_off(entry, off));

    if (it != file_blocks.end()) {
        buf = it->second;
        return true;
    } else {
        return false;
    }
}

// Allocate new block, buf is only set on success (returning true)
static bool create_block(cl::CommandQueue& queue, shared_ptr<entry_t> entry, off_t off, cl::Buffer& buf) {
    int r;
    cl::Buffer ocl_buf(ocl_context, CL_MEM_READ_WRITE, BLOCK_SIZE, nullptr, &r);
    if (r != CL_SUCCESS) return false;

    // Initialise with zeros (out-of-memory error usually occurs at this point)
    r = queue.enqueueFillBuffer(ocl_buf, 0, 0, BLOCK_SIZE, nullptr, nullptr);
    if (r != CL_SUCCESS) return false;

    file_blocks[entry_off(entry, off)] = ocl_buf;
    buf = ocl_buf;

    return true;
}

// Delete all blocks with a starting offset >= *off*
static void delete_blocks(shared_ptr<entry_t> entry, off_t off = 0) {
    // Determine lowest block within range
    off_t start_off = (off / BLOCK_SIZE) * BLOCK_SIZE;

    for (auto it = file_blocks.find(entry_off(entry, start_off)); it != file_blocks.end();) {
        if (it->first.entry == entry && it->first.off >= off) {
            it = file_blocks.erase(it);
        } else if (it->first.entry != entry) {
            break;
        } else {
            it++;
        }
    }
}

// Delete an entry and its blocks (in case it's a file)
static int delete_entry(shared_ptr<entry_t> entry) {
    if (entry->parent) entry->parent->ctime = entry->parent->mtime = get_time();

    entries.erase(entry);

    delete_blocks(entry);

    return 0;
}

// Change file size, any blocks beyond the size will be automatically removed
static void set_file_size(shared_ptr<entry_t> entry, size_t size) {
    if (entry->size > size) {
        delete_blocks(entry, size);
    }

    entry->size = size;
}

// Create a new entry
static shared_ptr<entry_t> make_entry(shared_ptr<entry_t> parent) {
    auto e = std::make_shared<entry_t>();
    e->parent = parent;
    entries.insert(e);

    return e;
}

// Find entry by parent and name
static shared_ptr<entry_t> find_entry(shared_ptr<entry_t> parent, std::string name) {
    for (auto& e : entries) {
        if (e->parent == parent && e->name == name) {
            return e;
        }
    }

    return nullptr;
}

// Find entries by parent
static std::vector<shared_ptr<entry_t>> find_entries(shared_ptr<entry_t> parent) {
    std::vector<shared_ptr<entry_t>> results;

    for (auto& e : entries) {
        if (e->parent == parent) {
            results.push_back(e);
        }
    }

    return results;
}

// Find entry by path (starting with /)
static int index_find(const std::string& path, shared_ptr<entry_t>& entry, int filter = entry_type::all) {
    // If filter is empty, no entry will ever match
    if ((filter & entry_type::all) == 0) return -ENOENT;

    // Traverse file system by hierarchically, starting from root directory
    entry = root_entry;

    std::stringstream stream(path.substr(1));
    std::string part;

    // If the path is empty, assume the root directory
    //if (path.size() != 0) {
        while (getline(stream, part, '/')) {
            // If current directory is actually a file, abort
            if (!entry->dir) return -ENOTDIR;

            entry = find_entry(entry, part);

            // If entry was not found, abort
            if (!entry) return -ENOENT;
        }
    //}

    // If an undesired type of entry was found, return an appropriate error
    int actual_type = entry->target.size() > 0 ? entry_type::link :
                      entry->dir ? entry_type::dir :
                      entry_type::file;

    if (!(actual_type & filter)) {
        if (actual_type == entry_type::file) {
            if (filter & entry_type::link) return -ENOENT;
            if (filter & entry_type::dir) return -EISDIR;
        } else if (actual_type == entry_type::dir) {
            if (filter & entry_type::file) return -ENOTDIR;
            if (filter & entry_type::link) return -EPERM;
        } else {
            return -EPERM;
        }
    }

    return 0;
}

/*
 * Initialisation
 */

static void* vram_init(fuse_conn_info* conn) {
    // Create root directory
    root_entry = make_entry(nullptr);
    root_entry->dir = true;

    // Create OpenCL context
    std::vector<cl::Platform> platforms;
    cl::Platform::get(&platforms);
    if (platforms.size() == 0) return fatal_error("no opencl platform found", nullptr);

    std::vector<cl::Device> gpu_devices;
    platforms[0].getDevices(CL_DEVICE_TYPE_GPU, &gpu_devices);
    if (gpu_devices.size() == 0) return fatal_error("no opencl capable gpu found", nullptr);

    ocl_device = gpu_devices[0];
    ocl_context = cl::Context(ocl_device);
    ocl_proto_queue = cl::CommandQueue(ocl_context, ocl_device);

    return nullptr;
}

/*
 * Entry attributes
 */

static int vram_getattr(const char* path, struct stat* stbuf) {
    scoped_lock local_lock(fslock);

    // Look up entry
    shared_ptr<entry_t> entry;
    int err = index_find(path, entry);
    if (err != 0) return err;

    memset(stbuf, 0, sizeof(struct stat));

    if (entry->dir) {
        stbuf->st_mode = S_IFDIR | entry->mode;
        stbuf->st_nlink = 2;
    } else if (entry->target.size() == 0) {
        stbuf->st_mode = S_IFREG | entry->mode;
        stbuf->st_nlink = 1;
    } else {
        stbuf->st_mode = S_IFLNK | 0777;
        stbuf->st_nlink = 1;
    }

    stbuf->st_uid = geteuid();
    stbuf->st_gid = getegid();
    stbuf->st_size = entry->size;
    stbuf->st_atim = entry->atime;
    stbuf->st_mtim = entry->mtime;
    stbuf->st_ctim = entry->ctime;

    return 0;
}

/*
 * Get target of link
 */

static int vram_readlink(const char* path, char* buf, size_t size) {
    scoped_lock local_lock(fslock);

    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::link);
    if (err != 0) return err;

    strncpy(buf, entry->target.c_str(), size);

    return 0;
}

/*
 * Set the mode bits of an entry
 */

static int vram_chmod(const char* path, mode_t mode) {
    scoped_lock local_lock(fslock);

    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::file | entry_type::dir);
    if (err != 0) return err;

    entry->mode = mode;
    entry->ctime = get_time();

    return 0;
}

/*
 * Set the last access and last modified times of an entry
 */

static int vram_utimens(const char* path, const timespec tv[2]) {
    scoped_lock local_lock(fslock);

    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::file | entry_type::dir);
    if (err != 0) return err;

    entry->atime = tv[0];
    entry->mtime = tv[1];
    entry->ctime = get_time();

    return 0;
}

/*
 * Directory listing
 */

static int vram_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t, fuse_file_info*) {
    scoped_lock local_lock(fslock);

    // Look up directory
    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::dir);
    if (err != 0) return err;

    // Required default entries
    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);

    for (auto e : find_entries(entry)) {
        filler(buf, e->name.c_str(), nullptr, 0);
    }

    entry->ctime = entry->atime = get_time();

    return 0;
}

/*
 * Create file
 */

static int vram_create(const char* path, mode_t, struct fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    // Fail if entry already exists
    shared_ptr<entry_t> entry;
    int err = index_find(path, entry);
    if (err == 0) return -EEXIST;

    std::string dir, file;
    split_file_path(path, dir, file);

    // Check if parent directory exists
    err = index_find(dir, entry, entry_type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = get_time();

    entry = make_entry(entry);
    entry->name = file;
    entry->mode = DEFAULT_FILE_MODE;
    entry->size = 0;

    // Open it by assigning new file handle
    fi->fh = reinterpret_cast<uint64_t>(new file_session(entry, ocl_proto_queue));

    return 0;
}

/*
 * Create directory
 */

static int vram_mkdir(const char* path, mode_t) {
    scoped_lock local_lock(fslock);

    // Fail if entry with that name already exists
    shared_ptr<entry_t> entry;
    int err = index_find(path, entry);
    if (err == 0) return -EEXIST;

    std::string parent, dir;
    split_file_path(path, parent, dir);

    // Check if parent directory exists
    err = index_find(parent, entry, entry_type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = get_time();

    // Create new directory
    entry = make_entry(entry);
    entry->name = dir;
    entry->dir = true;
    entry->mode = DEFAULT_DIR_MODE;

    return 0;
}

/*
 * Create symlink
 */

static int vram_symlink(const char* target, const char* path) {
    scoped_lock local_lock(fslock);

    // Fail if an entry with that name already exists
    shared_ptr<entry_t> entry;
    int err = index_find(path, entry);
    if (err == 0) return -EEXIST;

    // Split path in parent directory and new symlink name
    std::string parent, name;
    split_file_path(path, parent, name);

    // Check if parent directory exists
    err = index_find(parent, entry, entry_type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = get_time();

    // Create new symlink - target is only resolved at usage
    entry = make_entry(entry);
    entry->name = name;
    entry->target = target;
    entry->size = entry->target.size();

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

    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::link | entry_type::file);
    if (err != 0) return err;

    delete_entry(entry);

    return 0;
}

/*
 * Delete directory
 */

static int vram_rmdir(const char* path) {
    scoped_lock local_lock(fslock);

    // Fail if entry doesn't exist or is not a directory
    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::dir);
    if (err != 0) return err;

    // Check if directory is empty
    if (find_entries(entry).size() != 0) {
        return -ENOTEMPTY;
    }

    delete_entry(entry);

    return 0;
}

/*
 * Rename entry
 */

static int vram_rename(const char* path, const char* new_path) {
    scoped_lock local_lock(fslock);

    // Look up entry
    shared_ptr<entry_t> entry;
    int err = index_find(path, entry);
    if (err != 0) return err;

    // Check if destination directory exists
    std::string dir, new_name;
    split_file_path(new_path, dir, new_name);

    shared_ptr<entry_t> parent;
    err = index_find(dir, parent, entry_type::dir);
    if (err != 0) return err;
    parent->ctime = parent->mtime = get_time();

    // If the destination entry already exists, then delete it
    shared_ptr<entry_t> dest_entry;
    err = index_find(new_path, dest_entry);
    if (err == 0) delete_entry(dest_entry);

    entry->parent = parent;
    entry->name = new_name;
    entry->ctime = get_time();

    return 0;
}

/*
 * Open file
 */

static int vram_open(const char* path, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::file);
    if (err != 0) return err;

    fi->fh = reinterpret_cast<uint64_t>(new file_session(entry, ocl_proto_queue));

    return 0;
}

/*
 * Read file
 */

static int vram_read(const char* path, char* buf, size_t size, off_t off, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);
    file_session* session = reinterpret_cast<file_session*>(fi->fh);

    size_t file_size = session->entry->size;
    if ((size_t) off >= file_size) return 0;
    size = std::min(file_size - off, size);

    if (session->dirty) {
        session->queue.finish();
        session->dirty = false;
    }

    // Walk over blocks in read region
    off_t end_pos = off + size;
    size_t total_read = size;

    while (off < end_pos) {
        // Find block corresponding to current offset
        off_t block_start = (off / BLOCK_SIZE) * BLOCK_SIZE;
        off_t block_off = off - block_start;
        size_t read_size = std::min(BLOCK_SIZE - block_off, size);

        cl::Buffer ocl_buf;
        bool buf_exists = get_block(session->entry, block_start, ocl_buf);

        // Allow multiple threads to block for reading simultaneously
        fslock.unlock();
        if (buf_exists) {
            session->queue.enqueueReadBuffer(ocl_buf, true, block_off, read_size, buf, nullptr, nullptr);
        } else {
            // Non-written part of file
            memset(buf, 0, read_size);
        }
        fslock.lock();

        buf += read_size;
        off += read_size;
        size -= read_size;
    }

    session->entry->ctime = session->entry->atime = get_time();

    return total_read;
}

/*
 * Write file
 */

static CL_CALLBACK void write_cb(cl_event ev, cl_int status, void* buf_copy) {
    delete [] reinterpret_cast<char*>(buf_copy);
}

static int vram_write(const char* path, const char* buf, size_t size, off_t off, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);
    file_session* session = reinterpret_cast<file_session*>(fi->fh);

    // Walk over blocks in write region
    off_t end_pos = off + size;
    size_t total_write = size;

    while (off < end_pos) {
        // Find block corresponding to current offset
        off_t block_start = (off / BLOCK_SIZE) * BLOCK_SIZE;
        off_t block_off = off - block_start;
        size_t write_size = std::min(BLOCK_SIZE - block_off, size);

        cl::Buffer ocl_buf;
        bool buf_exists = get_block(session->entry, block_start, ocl_buf);

        if (!buf_exists) {
            buf_exists = create_block(session->queue, session->entry, block_start, ocl_buf);

            // Failed to allocate buffer, likely out of VRAM
            if (!buf_exists) break;
        }

        // Make copy of buffer for OpenCL to do asynchronous write
        char* buf_copy = new char[write_size];
        memcpy(buf_copy, buf, write_size);

        cl::Event event;
        session->queue.enqueueWriteBuffer(ocl_buf, false, block_off, write_size, buf_copy, nullptr, &event);

        // Callback on completion to clean up the buffer copy
        event.setCallback(CL_COMPLETE, write_cb, buf_copy);

        buf += write_size;
        off += write_size;
        size -= write_size;
    }

    if (session->entry->size < (size_t) off) {
        set_file_size(session->entry, off);
    }
    session->entry->ctime = session->entry->mtime = get_time();

    session->dirty = true;

    if (off < end_pos) {
        return -ENOSPC;
    } else {
        return total_write;
    }
}

/*
 * Sync writes to file
 */

static int vram_fsync(const char* path, int, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    file_session* session = reinterpret_cast<file_session*>(fi->fh);
    session->queue.finish();

    return 0;
}

/*
 * Close file
 */

static int vram_release(const char* path, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    file_session* session = reinterpret_cast<file_session*>(fi->fh);
    session->queue.finish();
    delete session;

    return 0;
}

/*
 * Change file size
 */

static int vram_truncate(const char* path, off_t size) {
    scoped_lock local_lock(fslock);

    shared_ptr<entry_t> entry;
    int err = index_find(path, entry, entry_type::file);
    if (err != 0) return err;

    set_file_size(entry, size);
    entry->ctime = entry->mtime = get_time();

    return 0;
}

/*
 * FUSE setup
 */

static struct vram_operations : fuse_operations {
    vram_operations() {
        init = vram_init;
        getattr = vram_getattr;
        readlink = vram_readlink;
        utimens = vram_utimens;
        chmod = vram_chmod;
        readdir = vram_readdir;
        create = vram_create;
        mkdir = vram_mkdir;
        symlink = vram_symlink;
        unlink = vram_unlink;
        rmdir = vram_rmdir;
        rename = vram_rename;
        open = vram_open;
        read = vram_read;
        write = vram_write;
        fsync = vram_fsync;
        release = vram_release;
        truncate = vram_truncate;
    }
} operations;

int main(int argc, char* argv[]) {
    return fuse_main(argc, argv, &operations, nullptr);
}
