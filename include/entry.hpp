#ifndef VRAM_ENTRY_HPP
#define VRAM_ENTRY_HPP

/*
 * Entry types
 */

#include <string>
#include <memory>
#include <mutex>
#include <map>
#include <unordered_map>

#include "memory.hpp"

using std::string;

namespace vram {
    namespace entry {
        // Types of references to entries
        class entry_t;
        class file_t;
        class dir_t;
        class symlink_t;

        typedef std::shared_ptr<entry_t> entry_ref;
        typedef std::shared_ptr<file_t> file_ref;
        typedef std::shared_ptr<dir_t> dir_ref;
        typedef std::shared_ptr<symlink_t> symlink_ref;

        typedef entry_t* entry_ptr;
        typedef file_t* file_ptr;
        typedef dir_t* dir_ptr;
        typedef symlink_t* symlink_ptr;

        // Type of entry, can be combined for filtering in find()
        namespace type {
            enum type_t {
                none = 0,
                file = 1,
                dir = 2,
                symlink = 4
            };

            const int all = file | dir | symlink;
        }

        // Full description of entry
        // TODO: parent and children private?
        class entry_t {
        public:
            // Non-owning pointer, parent is guaranteed to exist if entry exists
            dir_ptr parent = nullptr;

            string name;

            int mode = 0;

            timespec atime;
            timespec mtime;
            timespec ctime;

            entry_t(const entry_t& other) = delete;

            virtual ~entry_t() {};

            virtual type::type_t type() const = 0;

            virtual size_t size() const = 0;

            // Remove link with parent directory
            void unlink();

            // Move entry
            void move(dir_ptr new_parent, const string& new_name);

        protected:
            // Reference to itself
            std::weak_ptr<entry_t> self_ref;

            entry_t();
        };

        // File entry
        class file_t : public entry_t {
        public:
            // Constructor that takes care of memory management
            static file_ref make(dir_ptr parent, const string& name);

            file_t();

            type::type_t type() const;

            size_t size() const;

            // Blocks beyond the new file size are immediately deallocated
            void size(size_t new_size);

            // Read data from file, returns total bytes read <= size
            //
            // The specified mutex is unlocked while blocking to read, because
            // that's a non-critical section.
            int read(off_t off, size_t size, char* data, std::mutex& wait_mutex);

            // Write data to file, returns -error or total bytes written
            int write(off_t off, size_t size, const char* data, bool async = true);

            // Sync writes to file
            void sync();

        private:
            // Data blocks if this entry is a file
            std::map<off_t, memory::block> file_blocks;

            // Last block touched by write()
            memory::block last_written_block;

            // File size
            size_t _size = 0;

            // Get the OpenCL buffer of the block if it exists (returning true)
            bool get_block(off_t off, memory::block& buf) const;

            // Allocate new block, buf is only set on success (returning true)
            bool create_block(off_t off, memory::block& buf);

            // Delete all blocks with a starting offset >= *off*
            void delete_blocks(off_t off = 0);
        };

        // Directory entry
        class dir_t : public entry_t {
        public:
            std::unordered_map<string, entry_ref> children;

            // Constructor that takes care of memory management
            static dir_ref make(dir_ptr parent, const string& name);

            dir_t();

            type::type_t type() const;

            // Always returns size of 4096
            size_t size() const;

            // Find entry by path relative to this entry
            int find(const string& path, entry_ref& entry, int filter = type::all);
        };

        // Symlink entry
        class symlink_t : public entry_t {
        public:
            // Target of symlink
            string target;

            // Constructor that takes care of memory management
            static symlink_ref make(dir_ptr parent, const string& name, const string& target);

            type::type_t type() const;

            size_t size() const;
        };
    }
}

#endif
