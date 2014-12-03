// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <unistd.h>
#include <sqlite3.h>

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
#include "util.hpp"

using namespace vram;

static const int DEFAULT_FILE_MODE = 0664;
static const int DEFAULT_DIR_MODE = 0775;

/*
 * Globals
 */

// Lock to prevent multiple threads from manipulating the file system index and
// OpenCL buffers simultaneously. The tiny overhead is worth not having to deal
// with the uncountable amount of race conditions that would otherwise occur.
static std::mutex fslock;

// File system root that links to the rest
static shared_ptr<entry_t> root_entry;

// File blocks
static std::map<entry_off, memory::block> file_blocks;

/*
 * Helpers
 */

// Get the OpenCL buffer of the block if it exists (returning true)
static bool get_block(shared_ptr<entry_t> entry, off_t off, memory::block& buf) {
    auto it = file_blocks.find(entry_off(entry, off));

    if (it != file_blocks.end()) {
        buf = it->second;
        return true;
    } else {
        return false;
    }
}

// Allocate new block, buf is only set on success (returning true)
static bool create_block(shared_ptr<entry_t> entry, off_t off, memory::block& buf) {
    bool success;
    memory::block tmp_buf(success);

    if (success) {
        file_blocks[entry_off(entry, off)] = tmp_buf;
        buf = tmp_buf;
    }

    return success;
}

// Delete all blocks with a starting offset >= *off*
static void delete_blocks(shared_ptr<entry_t> entry, off_t off = 0) {
    // Determine lowest block within range
    off_t start_off = (off / memory::block::size) * memory::block::size;

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
    if (entry->parent) entry->parent->ctime = entry->parent->mtime = util::time();

    entry->parent->children.erase(entry->name);

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
static shared_ptr<entry_t> make_entry(entry_t* parent, string name) {
    auto entry = std::make_shared<entry_t>();

    entry->parent = parent;
    entry->name = name;

    if (parent) {
        parent->children[name] = entry;
    }

    return entry;
}

// Move an entry
static void move_entry(shared_ptr<entry_t> entry, entry_t* new_parent, const string& new_name) {
    entry->parent->children.erase(entry->name);

    entry->parent = new_parent;
    entry->name = new_name;

    if (new_parent) {
        new_parent->children[new_name] = entry;
    }
}

// Find entry by path (starting with /)
static int index_find(const string& path, shared_ptr<entry_t>& entry, int filter = entry_type::all) {
    // If filter is empty, no entry will ever match
    if ((filter & entry_type::all) == 0) return -ENOENT;

    // Traverse file system by hierarchically, starting from root directory
    entry = root_entry;

    std::stringstream stream(path.substr(1));
    string part;

    // If the path is empty, assume the root directory
    while (getline(stream, part, '/')) {
        // If current directory is actually a file, abort
        if (!entry->dir) return -ENOTDIR;

        // Navigate to next entry
        auto it = entry->children.find(part);

        if (it != entry->children.end()) {
            entry = it->second;
        } else {
            return -ENOENT;
        }
    }

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
    root_entry = make_entry(nullptr, "");
    root_entry->dir = true;

    // Check for OpenCL supported GPU
    if (!memory::is_available()) {
        return util::fatal_error("no opencl capable gpu found", nullptr);
    }

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
    entry->ctime = util::time();

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
    entry->ctime = util::time();

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

    for (auto& pair : entry->children) {
        filler(buf, pair.second->name.c_str(), nullptr, 0);
    }

    entry->ctime = entry->atime = util::time();

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

    string dir, file;
    util::split_file_path(path, dir, file);

    // Check if parent directory exists
    err = index_find(dir, entry, entry_type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = util::time();

    entry = make_entry(entry.get(), file);
    entry->mode = DEFAULT_FILE_MODE;
    entry->size = 0;

    // Open it by assigning new file handle
    fi->fh = reinterpret_cast<uint64_t>(new file_session(entry));

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

    string parent, dir;
    util::split_file_path(path, parent, dir);

    // Check if parent directory exists
    err = index_find(parent, entry, entry_type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = util::time();

    // Create new directory
    entry = make_entry(entry.get(), dir);
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
    string parent, name;
    util::split_file_path(path, parent, name);

    // Check if parent directory exists
    err = index_find(parent, entry, entry_type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = util::time();

    // Create new symlink - target is only resolved at usage
    entry = make_entry(entry.get(), name);
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
    if (entry->children.size() != 0) {
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
    string dir, new_name;
    util::split_file_path(new_path, dir, new_name);

    shared_ptr<entry_t> parent;
    err = index_find(dir, parent, entry_type::dir);
    if (err != 0) return err;
    parent->ctime = parent->mtime = util::time();

    // If the destination entry already exists, then delete it
    shared_ptr<entry_t> dest_entry;
    err = index_find(new_path, dest_entry);
    if (err == 0) delete_entry(dest_entry);

    move_entry(entry, parent.get(), new_name);

    entry->ctime = util::time();

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

    fi->fh = reinterpret_cast<uint64_t>(new file_session(entry));

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

    // Walk over blocks in read region
    off_t end_pos = off + size;
    size_t total_read = size;

    while (off < end_pos) {
        // Find block corresponding to current offset
        off_t block_start = (off / memory::block::size) * memory::block::size;
        off_t block_off = off - block_start;
        size_t read_size = std::min(memory::block::size - block_off, size);

        memory::block block;
        bool block_exists = get_block(session->entry, block_start, block);

        // Allow multiple threads to block for reading simultaneously
        fslock.unlock();
        if (block_exists) {
            //session->queue.enqueueReadBuffer(block, true, block_off, read_size, buf, nullptr, nullptr);
            block.read(block_off, read_size, buf);
        } else {
            // Non-written part of file
            memset(buf, 0, read_size);
        }
        fslock.lock();

        buf += read_size;
        off += read_size;
        size -= read_size;
    }

    session->entry->ctime = session->entry->atime = util::time();

    return total_read;
}

/*
 * Write file
 */

static int vram_write(const char* path, const char* buf, size_t size, off_t off, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);
    file_session* session = reinterpret_cast<file_session*>(fi->fh);

    // Walk over blocks in write region
    off_t end_pos = off + size;
    size_t total_write = size;

    while (off < end_pos) {
        // Find block corresponding to current offset
        off_t block_start = (off / memory::block::size) * memory::block::size;
        off_t block_off = off - block_start;
        size_t write_size = std::min(memory::block::size - block_off, size);

        memory::block block;
        bool block_exists = get_block(session->entry, block_start, block);

        if (!block_exists) {
            block_exists = create_block(session->entry, block_start, block);

            // Failed to allocate buffer, likely out of VRAM
            if (!block_exists) break;
        }

        block.write(block_off, write_size, buf, true);

        session->last_written_block = block;

        buf += write_size;
        off += write_size;
        size -= write_size;
    }

    if (session->entry->size < (size_t) off) {
        set_file_size(session->entry, off);
    }
    session->entry->ctime = session->entry->mtime = util::time();

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
    session->last_written_block.sync();

    return 0;
}

/*
 * Close file
 */

static int vram_release(const char* path, fuse_file_info* fi) {
    scoped_lock local_lock(fslock);

    delete reinterpret_cast<file_session*>(fi->fh);

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
    entry->ctime = entry->mtime = util::time();

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
