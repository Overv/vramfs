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
#include "util.hpp"

using std::string;
using std::shared_ptr;

namespace vram {
    namespace entry {
        // Types of references to entries
        class entry_t;
        class file_t;
        class dir_t;
        class symlink_t;

        typedef shared_ptr<entry_t> entry_ref;
        typedef shared_ptr<file_t> file_ref;
        typedef shared_ptr<dir_t> dir_ref;
        typedef shared_ptr<symlink_t> symlink_ref;

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

        // Total entry count
        int count();

        // Full description of entry
        class entry_t : public std::enable_shared_from_this<entry_t> {
        public:
            entry_t(const entry_t& other) = delete;

            dir_ptr parent() const;

            const string& name() const;

            virtual ~entry_t();

            virtual type::type_t type() const = 0;

            virtual size_t size() const = 0;

            // Change file attributes (automatically updates change time)
            timespec atime() const;
            timespec mtime() const;
            timespec ctime() const;

            mode_t mode() const;
            uid_t user() const;
            gid_t group() const;

            void atime(timespec t);
            void mtime(timespec t);
            void ctime(timespec t);

            void mode(mode_t mode);
            void user(uid_t user);
            void group(gid_t group);

            // Remove link with parent directory
            void unlink();

            // Move entry
            void move(dir_ptr new_parent, const string& new_name);

        protected:
            entry_t();

            // Associate entry with a parent directory after construction
            void link(dir_ptr parent, const string& name);

        private:
            // Non-owning pointer, parent is guaranteed to exist if entry exists
            dir_ptr _parent = nullptr;

            string _name;

            mode_t _mode = 0;
            uid_t _user = 0;
            gid_t _group = 0;

            timespec _atime;
            timespec _mtime;
            timespec _ctime;
        };

        // File entry
        class file_t : public entry_t {
        public:
            // Constructor that takes care of memory management
            static file_ref make(dir_ptr parent, const string& name);

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
            std::map<off_t, memory::block_ref> file_blocks;

            // Last block touched by write()
            memory::block_ref last_written_block;

            // File size
            size_t _size = 0;

            file_t();

            // Get the OpenCL buffer of the block if it exists or a nullptr
            memory::block_ref get_block(off_t off) const;

            // Allocate new block, buf is only set on success (returning true)
            memory::block_ref alloc_block(off_t off);

            // Delete all blocks with a starting offset >= *off*
            void free_blocks(off_t off = 0);
        };

        // Directory entry
        class dir_t : public entry_t {
            friend class entry_t;

        public:
            // Constructor that takes care of memory management
            static dir_ref make(dir_ptr parent, const string& name);

            type::type_t type() const;

            // Always returns size of 4096
            size_t size() const;

            // Not const, because it changes access time
            const std::unordered_map<string, entry_ref> children();

            // Find entry by path relative to this entry
            int find(const string& path, entry_ref& entry, int filter = type::all) const;

        protected:
            std::unordered_map<string, entry_ref> _children;

        private:
            dir_t();
        };

        // Symlink entry
        class symlink_t : public entry_t {
        public:
            // Target of symlink
            const string target;

            // Constructor that takes care of memory management
            static symlink_ref make(dir_ptr parent, const string& name, const string& target);

            type::type_t type() const;

            size_t size() const;

        private:
            symlink_t(const string& target);
        };
    }
}

#endif
