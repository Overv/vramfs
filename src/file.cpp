#include "entry.hpp"
#include "util.hpp"

namespace vram {
    namespace entry {
        file_ref file_t::make(dir_ptr parent, const string& name) {
            auto file = std::make_shared<file_t>();

            file->self_ref = file;

            file->parent = parent;
            file->name = name;

            if (parent) {
                parent->children[name] = file;
            }

            return file;
        }

        file_t::file_t() {
            mode = 0644;
        }

        type::type_t file_t::type() const {
            return type::file;
        }

        size_t file_t::size() const {
            return _size;
        }

        void file_t::size(size_t new_size) {
            if (new_size < _size) {
                delete_blocks(new_size);
            }

            _size = new_size;
        }

        int file_t::read(off_t off, size_t size, char* data, std::mutex& wait_mutex) {
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

        int file_t::write(off_t off, size_t size, const char* data, bool async) {
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

        void file_t::sync() {
            // Waits for all asynchronous writes to finish, because they must
            // complete before the last write does (OpenCL guarantee)
            last_written_block.sync();
        }

        bool file_t::get_block(off_t off, memory::block& buf) const {
            auto it = file_blocks.find(off);

            if (it != file_blocks.end()) {
                buf = it->second;
                return true;
            } else {
                return false;
            }
        }

        bool file_t::create_block(off_t off, memory::block& buf) {
            bool success;
            memory::block tmp_buf(success);

            if (success) {
                file_blocks[off] = tmp_buf;
                buf = tmp_buf;
            }

            return success;
        }

        void file_t::delete_blocks(off_t off) {
            // Determine first block just beyond the range
            off_t start_off = (off / memory::block::size) * memory::block::size;
            if (off % memory::block::size != 0) start_off += memory::block::size;

            for (auto it = file_blocks.find(start_off); it != file_blocks.end();) {
                it = file_blocks.erase(it);
            }
        }
    }
}
