#ifndef VRAM_TYPES_HPP
#define VRAM_TYPES_HPP

#include <thread>
#include <memory>
#include <sqlite3.h>

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
 * Data persistent in an open() and release() session
 */

struct file_session {
    int64_t entry;
    cl::CommandQueue queue;
    // Set to true by write() so that read() knows to wait for OpenCL writes
    bool dirty;
    // Set to true when read() / write() are called to update file times
    bool read = false;
    bool write = false;

    file_session(int64_t entry, cl::CommandQueue queue) : entry(entry), queue(queue), dirty(false) {}
};

#endif
