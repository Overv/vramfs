#ifndef VRAM_TYPES_HPP
#define VRAM_TYPES_HPP

#include <thread>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "memory.hpp"
#include "entry.hpp"

using std::shared_ptr;
using std::lock_guard;
using std::mutex;
using std::string;

namespace vram {
    /*
     * File entry and offset for block for use as key in unordered_map
     */

    struct entry_off {
        entry::entry_ref entry;
        off_t off;

        entry_off(entry::entry_ref entry, off_t off) : entry(entry), off(off) {}

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
        entry::entry_ref entry;
        memory::block last_written_block;

        file_session(entry::entry_ref entry) : entry(entry) {}
    };
}

#endif
