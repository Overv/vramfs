#ifndef VRAM_ENTRY_HPP
#define VRAM_ENTRY_HPP

/*
 * Entry types
 */

#include <string>
#include <memory>
#include <map>
#include <unordered_map>

#include "memory.hpp"

using std::string;

namespace vram {
    namespace entry {
        class entry_t;

        typedef std::shared_ptr<entry_t> entry_ref;
        typedef entry_t* entry_ptr;

        // Type of entry, can be combined for filtering in find()
        namespace type {
            enum type_t {
                file = 1,
                dir = 2,
                link = 4
            };

            const int all = file | dir | link;
        }

        // Full description of entry
        class entry_t {
        public:
            // Non-owning pointer, parent is guaranteed to exist if entry exists
            entry_ptr parent = nullptr;
            std::unordered_map<string, entry_ref> children;

            string name;
            bool dir = false; // TODO: type flag instead

            int mode = 0;

            timespec atime;
            timespec mtime;
            timespec ctime;

            // Target if this entry is a symlink
            string target;

            // Data blocks if this entry is a file
            // TODO: Make all block related fields/methods private and expose only read() / write()
            std::map<off_t, memory::block> file_blocks;

            entry_t(const entry_t& other) = delete;

            // Constructor that takes care of memory management
            static entry_ref make(entry_ptr parent, const string& name);

            size_t size() const;

            // Blocks beyond the new file size are immediately deallocated
            void size(size_t new_size);

            // Get the OpenCL buffer of the block if it exists (returning true)
            bool get_block(off_t off, memory::block& buf) const;

            // Allocate new block, buf is only set on success (returning true)
            bool create_block(off_t off, memory::block& buf);

            // Delete all blocks with a starting offset >= *off*
            void delete_blocks(off_t off = 0);

            // Find entry by path relative to this entry
            int find(const string& path, entry_ref& entry, int filter = type::all);

            // Remove link with parent directory
            void unlink();

            // Move entry
            void move(entry_ptr new_parent, const string& new_name);

        private:
            // Default directory size
            size_t _size = 4096;

            // Reference to itself created in make()
            std::weak_ptr<entry_t> self_ref;

            entry_t();
        };
    }
}

#endif
