#include "entry.hpp"
#include "util.hpp"

namespace vram {
    namespace entry {
        file_ref file_t::make(dir_ptr parent, const string& name) {
            auto file = file_ref(new file_t());
            file->link(parent, name);
            return file;
        }

        file_t::file_t() {
            mode(0644);
        }

        type::type_t file_t::type() const {
            return type::file;
        }

        size_t file_t::size() const {
            return _size;
        }

        void file_t::size(size_t new_size) {
            if (new_size < _size) {
                free_blocks(new_size);
            }

            _size = new_size;

            mtime(util::time());
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

                auto block = get_block(block_start);

                // Allow multiple threads to block for reading simultaneously
                wait_mutex.unlock();
                if (block) {
                    block->read(block_off, read_size, data);
                } else {
                    // Non-written part of file
                    memset(data, 0, read_size);
                }
                wait_mutex.lock();

                data += read_size;
                off += read_size;
                size -= read_size;
            }

            atime(util::time());

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

                auto block = get_block(block_start);

                if (!block) {
                    block = alloc_block(block_start);

                    // Failed to allocate buffer, likely out of VRAM
                    if (!block) break;
                }

                block->write(block_off, write_size, data, async);

                last_written_block = block;

                data += write_size;
                off += write_size;
                size -= write_size;
            }

            if (_size < (size_t) off) {
                _size = off;
            }
            mtime(util::time());

            if (off < end_pos) {
                return -ENOSPC;
            } else {
                return total_write;
            }
        }

        void file_t::sync() {
            // Waits for all asynchronous writes to finish, because they must
            // complete before the last write does (OpenCL guarantee)
            last_written_block->sync();
        }

        memory::block_ref file_t::get_block(off_t off) const {
            auto it = file_blocks.find(off);

            if (it != file_blocks.end()) {
                return it->second;
            } else {
                return nullptr;
            }
        }

        memory::block_ref file_t::alloc_block(off_t off) {
            auto block = memory::allocate();

            if (block) {
                file_blocks[off] = block;
            }

            return block;
        }

        void file_t::free_blocks(off_t off) {
            // Determine first block just beyond the range
            off_t start_off = (off / memory::block::size) * memory::block::size;
            if (off % memory::block::size != 0) start_off += memory::block::size;

            for (auto it = file_blocks.find(start_off); it != file_blocks.end();) {
                it = file_blocks.erase(it);
            }
        }
    }
}
