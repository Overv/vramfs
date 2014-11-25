#ifndef VRAM_TYPES_HPP
#define VRAM_TYPES_HPP

#include <thread>
#include <memory>
#include <sqlite3.h>

/*
 * Desired result from index search functions
 *
 * These values are not bitflags, because that makes the logic for determining
 * the appropriate error code in case the type is different more complex.
 */

namespace entry_filter {
    enum entry_filter_t {
        file,
        directory,
        link,
        all,
        nonlink
    };
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

    file_session(int64_t entry, cl::CommandQueue queue) : entry(entry), queue(queue), dirty(false) {}
};

#endif
