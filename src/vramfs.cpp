// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <unistd.h>

// Standard library
#include <iostream>
#include <cstring>
#include <cstdint>

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
static std::mutex fsmutex;

// File system root that links to the rest
static entry::entry_ref root_entry;

/*
 * Initialisation
 */

static void* vram_init(fuse_conn_info* conn) {
    // Create root directory
    root_entry = entry::entry_t::make(nullptr, "");
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
    lock_guard<mutex> local_lock(fsmutex);

    // Look up entry
    entry::entry_ref entry;
    int err = root_entry->find(path, entry);
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
    stbuf->st_size = entry->size();
    stbuf->st_atim = entry->atime;
    stbuf->st_mtim = entry->mtime;
    stbuf->st_ctim = entry->ctime;

    return 0;
}

/*
 * Get target of link
 */

static int vram_readlink(const char* path, char* buf, size_t size) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::link);
    if (err != 0) return err;

    strncpy(buf, entry->target.c_str(), size);

    return 0;
}

/*
 * Set the mode bits of an entry
 */

static int vram_chmod(const char* path, mode_t mode) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::file | entry::type::dir);
    if (err != 0) return err;

    entry->mode = mode;
    entry->ctime = util::time();

    return 0;
}

/*
 * Set the last access and last modified times of an entry
 */

static int vram_utimens(const char* path, const timespec tv[2]) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::file | entry::type::dir);
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
    lock_guard<mutex> local_lock(fsmutex);

    // Look up directory
    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::dir);
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
    lock_guard<mutex> local_lock(fsmutex);

    // Truncate any existing file entry or fail if it's another type
    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::file);
    if (err == -EISDIR) return err;
    else if (err == 0) entry->unlink();

    string dir, file;
    util::split_file_path(path, dir, file);

    // Check if parent directory exists
    err = root_entry->find(dir, entry, entry::type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = util::time();

    entry = entry::entry_t::make(entry.get(), file);
    entry->mode = DEFAULT_FILE_MODE;
    entry->size(0);

    // Open it by assigning new file handle
    fi->fh = reinterpret_cast<uint64_t>(new file_session(entry));

    return 0;
}

/*
 * Create directory
 */

static int vram_mkdir(const char* path, mode_t) {
    lock_guard<mutex> local_lock(fsmutex);

    // Fail if entry with that name already exists
    entry::entry_ref entry;
    int err = root_entry->find(path, entry);
    if (err == 0) return -EEXIST;

    string parent, dir;
    util::split_file_path(path, parent, dir);

    // Check if parent directory exists
    err = root_entry->find(parent, entry, entry::type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = util::time();

    // Create new directory
    entry = entry::entry_t::make(entry.get(), dir);
    entry->dir = true;
    entry->mode = DEFAULT_DIR_MODE;

    return 0;
}

/*
 * Create symlink
 */

static int vram_symlink(const char* target, const char* path) {
    lock_guard<mutex> local_lock(fsmutex);

    // Fail if an entry with that name already exists
    entry::entry_ref entry;
    int err = root_entry->find(path, entry);
    if (err == 0) return -EEXIST;

    // Split path in parent directory and new symlink name
    string parent, name;
    util::split_file_path(path, parent, name);

    // Check if parent directory exists
    err = root_entry->find(parent, entry, entry::type::dir);
    if (err != 0) return err;
    entry->ctime = entry->mtime = util::time();

    // Create new symlink - target is only resolved at usage
    entry = entry::entry_t::make(entry.get(), name);
    entry->target = target;
    entry->size(entry->target.size());

    return 0;
}

/*
 * Delete file
 *
 * NOTE: FUSE will only call this function once the last handle to a file has
 * been closed. Setting the flag to disable that breaks the file system.
 */

static int vram_unlink(const char* path) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::link | entry::type::file);
    if (err != 0) return err;

    entry->unlink();

    return 0;
}

/*
 * Delete directory
 */

static int vram_rmdir(const char* path) {
    lock_guard<mutex> local_lock(fsmutex);

    // Fail if entry doesn't exist or is not a directory
    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::dir);
    if (err != 0) return err;

    // Check if directory is empty
    if (entry->children.size() != 0) {
        return -ENOTEMPTY;
    }

    entry->unlink();

    return 0;
}

/*
 * Rename entry
 */

static int vram_rename(const char* path, const char* new_path) {
    lock_guard<mutex> local_lock(fsmutex);

    // Look up entry
    entry::entry_ref entry;
    int err = root_entry->find(path, entry);
    if (err != 0) return err;

    // Check if destination directory exists
    string dir, new_name;
    util::split_file_path(new_path, dir, new_name);

    entry::entry_ref parent;
    err = root_entry->find(dir, parent, entry::type::dir);
    if (err != 0) return err;
    parent->ctime = parent->mtime = util::time();

    // If the destination entry already exists, then delete it
    entry::entry_ref dest_entry;
    err = root_entry->find(new_path, dest_entry);
    if (err == 0) dest_entry->unlink();

    entry->move(parent.get(), new_name);

    entry->ctime = util::time();

    return 0;
}

/*
 * Open file
 */

static int vram_open(const char* path, fuse_file_info* fi) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::file);
    if (err != 0) return err;

    fi->fh = reinterpret_cast<uint64_t>(new file_session(entry));

    return 0;
}

/*
 * Read file
 */

static int vram_read(const char* path, char* buf, size_t size, off_t off, fuse_file_info* fi) {
    lock_guard<mutex> local_lock(fsmutex);
    file_session* session = reinterpret_cast<file_session*>(fi->fh);

    size_t file_size = session->entry->size();
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
        bool block_exists = session->entry->get_block(block_start, block);

        // Allow multiple threads to block for reading simultaneously
        fsmutex.unlock();
        if (block_exists) {
            block.read(block_off, read_size, buf);
        } else {
            // Non-written part of file
            memset(buf, 0, read_size);
        }
        fsmutex.lock();

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
    lock_guard<mutex> local_lock(fsmutex);
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
        bool block_exists = session->entry->get_block(block_start, block);

        if (!block_exists) {
            block_exists = session->entry->create_block(block_start, block);

            // Failed to allocate buffer, likely out of VRAM
            if (!block_exists) break;
        }

        block.write(block_off, write_size, buf, true);

        session->last_written_block = block;

        buf += write_size;
        off += write_size;
        size -= write_size;
    }

    if (session->entry->size() < (size_t) off) {
        session->entry->size(off);
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
    lock_guard<mutex> local_lock(fsmutex);

    file_session* session = reinterpret_cast<file_session*>(fi->fh);
    session->last_written_block.sync();

    return 0;
}

/*
 * Close file
 */

static int vram_release(const char* path, fuse_file_info* fi) {
    lock_guard<mutex> local_lock(fsmutex);

    delete reinterpret_cast<file_session*>(fi->fh);

    return 0;
}

/*
 * Change file size
 */

static int vram_truncate(const char* path, off_t size) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::file);
    if (err != 0) return err;

    entry->size(size);
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
