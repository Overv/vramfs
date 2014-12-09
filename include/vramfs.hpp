#ifndef VRAM_TYPES_HPP
#define VRAM_TYPES_HPP

#include <thread>
#include <memory>
#include <mutex>

#include "util.hpp"
#include "memory.hpp"
#include "entry.hpp"

using std::lock_guard;
using std::mutex;
using std::string;
using std::dynamic_pointer_cast;

namespace vram {
    // Data persistent in an open() and release() session
    struct file_session {
        entry::file_ref file;

        file_session(entry::file_ref file) : file(file) {}
    };
}

#endif
