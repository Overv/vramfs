#include "entry.hpp"
#include "types.hpp"
#include "util.hpp"

#include <map>
#include <sstream>

namespace vram {
    namespace entry {
        entry_t::entry_t() {
            timespec t;
            clock_gettime(CLOCK_REALTIME, &t);

            atime = t;
            mtime = t;
            ctime = t;
        }

        entry_ref entry_t::make(entry_ptr parent, const string& name) {
            entry_ref entry(new entry_t());
            entry->self_ref = entry;

            entry->parent = parent;
            entry->name = name;

            if (parent) {
                parent->children[name] = entry;
            }

            return entry;
        }

        size_t entry_t::size() const {
            return _size;
        }

        void entry_t::size(size_t new_size) {
            if (new_size < _size) {
                delete_blocks(new_size);
            }

            _size = new_size;
        }

        int entry_t::find(const string& path, entry_ref& entry, int filter) {
            // If filter is empty, no entry will ever match
            if ((filter & type::all) == 0) return -ENOENT;

            // Traverse file system by hierarchically, starting from this entry
            entry = self_ref.lock();

            std::stringstream stream(path.substr(1));
            string part;

            // If the path is empty, assume the root directory
            while (getline(stream, part, '/')) {
                // If current directory is actually a file, abort
                if (!entry->dir) return -ENOTDIR;

                // Navigate to next entry
                auto it = entry->children.find(part);

                if (it != entry->children.end()) {
                    entry = it->second;
                } else {
                    return -ENOENT;
                }
            }

            // If an undesired type of entry was found, return an appropriate error
            int actual_type = entry->target.size() > 0 ? type::link :
                              entry->dir ? type::dir :
                              type::file;

            if (!(actual_type & filter)) {
                if (actual_type == type::file) {
                    if (filter & type::link) return -ENOENT;
                    if (filter & type::dir) return -EISDIR;
                } else if (actual_type == entry::type::dir) {
                    if (filter & type::file) return -ENOTDIR;
                    if (filter & type::link) return -EPERM;
                } else {
                    return -EPERM;
                }
            }

            return 0;
        }

        void entry_t::unlink() {
            if (parent) {
                parent->ctime = parent->mtime = util::time();
                parent->children.erase(name);
            }
        }

        void entry_t::move(entry_ptr new_parent, const string& new_name) {
            parent->children.erase(name);

            parent = new_parent;
            name = new_name;

            new_parent->children[new_name] = entry_ref(self_ref);
        }

        int entry_t::read(off_t off, size_t size, char* data, std::mutex& wait_mutex) {
            if ((size_t) off >= _size) return 0;
            size = std::min(_size - off, size);

            // Walk over blocks in read region
            off_t end_pos = off + size;
            size_t total_read = size;

            while (off < end_pos) {
                // Find block corresponding to current offset
                off_t block_start = (off / memory::block::size) * memory::block::size;
                off_t block_off = off - block_start;
                size_t read_size = std::min(memory::block::size - block_off, size);

                memory::block block;
                bool block_exists = get_block(block_start, block);

                // Allow multiple threads to block for reading simultaneously
                wait_mutex.unlock();
                if (block_exists) {
                    block.read(block_off, read_size, data);
                } else {
                    // Non-written part of file
                    memset(data, 0, read_size);
                }
                wait_mutex.lock();

                data += read_size;
                off += read_size;
                size -= read_size;
            }

            ctime = atime = util::time();

            return total_read;
        }

        int entry_t::write(off_t off, size_t size, const char* data, bool async) {
            // Walk over blocks in write region
            off_t end_pos = off + size;
            size_t total_write = size;

            while (off < end_pos) {
                // Find block corresponding to current offset
                off_t block_start = (off / memory::block::size) * memory::block::size;
                off_t block_off = off - block_start;
                size_t write_size = std::min(memory::block::size - block_off, size);

                memory::block block;
                bool block_exists = get_block(block_start, block);

                if (!block_exists) {
                    block_exists = create_block(block_start, block);

                    // Failed to allocate buffer, likely out of VRAM
                    if (!block_exists) break;
                }

                block.write(block_off, write_size, data, async);

                last_written_block = block;

                data += write_size;
                off += write_size;
                size -= write_size;
            }

            if (_size < (size_t) off) {
                _size = off;
            }
            ctime = mtime = util::time();

            if (off < end_pos) {
                return -ENOSPC;
            } else {
                return total_write;
            }
        }

        void entry_t::sync() {
            // Waits for all asynchronous writes to finish, because they must
            // complete before the last write does (OpenCL guarantee)
            last_written_block.sync();
        }

        bool entry_t::get_block(off_t off, memory::block& buf) const {
            auto it = file_blocks.find(off);

            if (it != file_blocks.end()) {
                buf = it->second;
                return true;
            } else {
                return false;
            }
        }

        bool entry_t::create_block(off_t off, memory::block& buf) {
            bool success;
            memory::block tmp_buf(success);

            if (success) {
                file_blocks[off] = tmp_buf;
                buf = tmp_buf;
            }

            return success;
        }

        void entry_t::delete_blocks(off_t off) {
            // Determine first block just beyond the range
            off_t start_off = (off / memory::block::size) * memory::block::size;
            if (off % memory::block::size != 0) start_off += memory::block::size;

            for (auto it = file_blocks.find(start_off); it != file_blocks.end();) {
                it = file_blocks.erase(it);
            }
        }
    }
}
