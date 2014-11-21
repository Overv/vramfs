#ifndef VRAM_TYPES_HPP
#define VRAM_TYPES_HPP

#include <thread>
#include <memory>
#include <sqlite3.h>

/*
 * Desired result from index search functions
 */

namespace entry_filter {
    enum entry_filter_t {
        file,
        directory,
        all
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

#endif
