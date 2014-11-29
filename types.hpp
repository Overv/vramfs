#ifndef VRAM_TYPES_HPP
#define VRAM_TYPES_HPP

#include <thread>
#include <memory>

using std::shared_ptr;

/*
 * Types of objects in the file system, can be combined for index_find filter
 */

namespace entry_type {
    enum entry_type_t {
        file = 1,
        dir = 2,
        link = 4
    };

    const int all = file | dir | link;
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
 * Entry description in file system index
 */

struct entry_t {
    // Entries are in the root by default
    std::shared_ptr<entry_t> parent = nullptr;
    std::string name;
    bool dir = false;
    int mode = 0;
    // Default directory size
    size_t size = 4096;
    timespec atime;
    timespec mtime;
    timespec ctime;
    // Target if this entry is a symlink
    std::string target;

    entry_t() {
        timespec t;
        clock_gettime(CLOCK_REALTIME, &t);

        atime = t;
        mtime = t;
        ctime = t;
    }
};

/*
 * File entry and offset for block for use as key in unordered_map
 */

struct entry_off {
    shared_ptr<entry_t> entry;
    off_t off;

    entry_off(shared_ptr<entry_t> entry, off_t off) : entry(entry), off(off) {}

    bool operator<(const entry_off& other) const {
        if (entry == other.entry) {
            return off < other.off;
        } else {
            return entry < other.entry;
        }
    }
};

/*
 * Data persistent in an open() and release() session
 */

struct file_session {
    shared_ptr<entry_t> entry;
    cl::CommandQueue queue;
    // Set to true by write() so that read() knows to wait for OpenCL writes
    bool dirty = false;

    file_session(shared_ptr<entry_t> entry, cl::CommandQueue queue) : entry(entry), queue(queue) {}
};

#endif
