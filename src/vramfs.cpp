// Third-party libraries
#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <unistd.h>

// Standard library
#include <iostream>
#include <cstring>
#include <cstdint>
#include <limits>

// Internal dependencies
#include "vramfs.hpp"

using namespace vram;

/*
 * Configuration
 */

const size_t disk_size = 2L * 1024L * 1024L * 1024L;

/*
 * Globals
 */

// Lock to prevent multiple threads from manipulating the file system index and
// OpenCL buffers simultaneously. The tiny overhead is worth not having to deal
// with the uncountable amount of race conditions that would otherwise occur.
static std::mutex fsmutex;

// File system root that links to the rest
static entry::dir_ref root_entry;

/*
 * Initialisation
 */

static void* vram_init(fuse_conn_info* conn) {
    // Create root directory
    root_entry = entry::dir_t::make(nullptr, "");
    root_entry->user(geteuid());
    root_entry->group(getegid());

    // Check for OpenCL supported GPU
    if (!memory::is_available()) {
        return util::fatal_error("no opencl capable gpu found", nullptr);
    } else {
        memory::increase_pool(disk_size);
    }

    return nullptr;
}

/*
 * File system info
 */

static int vram_statfs(const char*, struct statvfs* vfs) {
    vfs->f_bsize = memory::block::size;
    vfs->f_blocks = memory::pool_size();
    vfs->f_bfree = memory::pool_available();
    vfs->f_bavail = memory::pool_available();
    vfs->f_files = entry::count();
    vfs->f_ffree = std::numeric_limits<fsfilcnt_t>::max();
    vfs->f_namemax = std::numeric_limits<unsigned long>::max();

    return 0;
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

    if (entry->type() == entry::type::dir) {
        stbuf->st_mode = S_IFDIR | entry->mode();
        stbuf->st_nlink = 2;
    } else if (entry->type() == entry::type::file) {
        stbuf->st_mode = S_IFREG | entry->mode();
        stbuf->st_nlink = 1;
        stbuf->st_blksize = memory::block::size;

        if (entry->size() > 0) {
            stbuf->st_blocks = 1 + (entry->size() - 1) / memory::block::size;
        }
    } else {
        stbuf->st_mode = S_IFLNK | 0777;
        stbuf->st_nlink = 1;
    }

    stbuf->st_uid = entry->user();
    stbuf->st_gid = entry->group();
    stbuf->st_size = entry->size();
    stbuf->st_atim = entry->atime();
    stbuf->st_mtim = entry->mtime();
    stbuf->st_ctim = entry->ctime();

    return 0;
}

/*
 * Get target of link
 */

static int vram_readlink(const char* path, char* buf, size_t size) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::symlink);
    if (err != 0) return err;

    auto symlink = dynamic_pointer_cast<entry::symlink_t>(entry);
    strncpy(buf, symlink->target.c_str(), size);

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

    entry->mode(mode);

    return 0;
}

/*
 * Change the owner/group of an entry
 */

static int vram_chown(const char* path, uid_t user, gid_t group) {
    lock_guard<mutex> lock_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::file | entry::type::dir);
    if (err != 0) return err;

    entry->user(user);
    entry->group(group);

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

    entry->atime(tv[0]);
    entry->mtime(tv[1]);

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
    auto dir = dynamic_pointer_cast<entry::dir_t>(entry);

    // Required default entries
    filler(buf, ".", nullptr, 0);
    filler(buf, "..", nullptr, 0);

    for (auto& pair : dir->children()) {
        filler(buf, pair.second->name().c_str(), nullptr, 0);
    }

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

    string dir, name;
    util::split_file_path(path, dir, name);

    // Check if parent directory exists
    err = root_entry->find(dir, entry, entry::type::dir);
    if (err != 0) return err;
    auto parent = dynamic_pointer_cast<entry::dir_t>(entry);

    // Create new entry with appropriate owner/group
    auto file = entry::file_t::make(parent.get(), name);

    auto context = fuse_get_context();
    file->user(context->uid);
    file->group(context->gid);

    // Open it by assigning new file handle
    fi->fh = reinterpret_cast<uint64_t>(new file_session(file));

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

    string dir, name;
    util::split_file_path(path, dir, name);

    // Check if parent directory exists
    err = root_entry->find(dir, entry, entry::type::dir);
    if (err != 0) return err;
    auto parent = dynamic_pointer_cast<entry::dir_t>(entry);

    // Create new directory with appropriate owner/group
    auto new_dir = entry::dir_t::make(parent.get(), name);

    auto context = fuse_get_context();
    new_dir->user(context->uid);
    new_dir->group(context->gid);

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
    string dir, name;
    util::split_file_path(path, dir, name);

    // Check if parent directory exists
    err = root_entry->find(dir, entry, entry::type::dir);
    if (err != 0) return err;
    auto parent = dynamic_pointer_cast<entry::dir_t>(entry);

    // Create new symlink with appropriate owner/group
    auto symlink = entry::symlink_t::make(parent.get(), name, target);

    auto context = fuse_get_context();
    symlink->user(context->uid);
    symlink->group(context->gid);

    return 0;
}

/*
 * Delete file
 */

static int vram_unlink(const char* path) {
    lock_guard<mutex> local_lock(fsmutex);

    entry::entry_ref entry;
    int err = root_entry->find(path, entry, entry::type::symlink | entry::type::file);
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
    auto dir = dynamic_pointer_cast<entry::dir_t>(entry);

    // Check if directory is empty
    if (dir->children().size() != 0) {
        return -ENOTEMPTY;
    }

    dir->unlink();

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

    entry::entry_ref parent_entry;
    err = root_entry->find(dir, parent_entry, entry::type::dir);
    if (err != 0) return err;
    auto parent = dynamic_pointer_cast<entry::dir_t>(parent_entry);

    // If the destination entry already exists, then delete it
    entry::entry_ref dest_entry;
    err = root_entry->find(new_path, dest_entry);
    if (err == 0) dest_entry->unlink();

    entry->move(parent.get(), new_name);

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
    auto file = dynamic_pointer_cast<entry::file_t>(entry);

    fi->fh = reinterpret_cast<uint64_t>(new file_session(file));

    return 0;
}

/*
 * Read file
 */

static int vram_read(const char* path, char* buf, size_t size, off_t off, fuse_file_info* fi) {
    lock_guard<mutex> local_lock(fsmutex);

    file_session* session = reinterpret_cast<file_session*>(fi->fh);
    return session->file->read(off, size, buf, fsmutex);
}

/*
 * Write file
 */

static int vram_write(const char* path, const char* buf, size_t size, off_t off, fuse_file_info* fi) {
    lock_guard<mutex> local_lock(fsmutex);

    file_session* session = reinterpret_cast<file_session*>(fi->fh);
    return session->file->write(off, size, buf);
}

/*
 * Sync writes to file
 */

static int vram_fsync(const char* path, int, fuse_file_info* fi) {
    lock_guard<mutex> local_lock(fsmutex);

    file_session* session = reinterpret_cast<file_session*>(fi->fh);
    session->file->sync();

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
    auto file = dynamic_pointer_cast<entry::file_t>(entry);

    file->size(size);

    return 0;
}

/*
 * FUSE setup
 */

static struct vram_operations : fuse_operations {
    vram_operations() {
        init = vram_init;
        statfs = vram_statfs;
        getattr = vram_getattr;
        readlink = vram_readlink;
        utimens = vram_utimens;
        chmod = vram_chmod;
        chown = vram_chown;
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
