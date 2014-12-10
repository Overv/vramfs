#include "entry.hpp"
#include "util.hpp"

#include <sstream>

namespace vram {
    namespace entry {
        dir_ref dir_t::make(dir_ptr parent, const string& name) {
            auto dir = dir_ref(new dir_t());
            dir->link(parent, name);
            return dir;
        }

        dir_t::dir_t() {
            mode = 0755;
        }

        type::type_t dir_t::type() const {
            return type::dir;
        }

        size_t dir_t::size() const {
            return 4096;
        }

        const std::unordered_map<string, entry_ref> dir_t::children() const {
            return _children;
        }

        int dir_t::find(const string& path, entry_ref& entry, int filter) const {
            // If filter is empty, no entry will ever match
            if ((filter & type::all) == 0) return -ENOENT;

            // Traverse file system by hierarchically, starting from this entry
            entry = std::const_pointer_cast<entry_t>(shared_from_this());

            std::stringstream stream(path.substr(1));
            string part;

            // If the path is empty, assume the root directory
            while (getline(stream, part, '/')) {
                // If current entry isn't a directory, abort
                if (entry->type() != type::dir) return -ENOTDIR;

                // Navigate to next entry
                auto dir = std::dynamic_pointer_cast<dir_t>(entry);
                auto it = dir->_children.find(part);

                if (it != dir->_children.end()) {
                    entry = it->second;
                } else {
                    return -ENOENT;
                }
            }

            // If an undesired type of entry was found, return an appropriate error
            if (!(entry->type() & filter)) {
                if (entry->type() == type::file) {
                    if (filter & type::symlink) return -ENOENT;
                    if (filter & type::dir) return -EISDIR;
                } else if (entry->type() == entry::type::dir) {
                    if (filter & type::file) return -ENOTDIR;
                    if (filter & type::symlink) return -EPERM;
                } else {
                    return -EPERM;
                }
            }

            return 0;
        }
    }
}
