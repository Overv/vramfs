#include "entry.hpp"
#include "util.hpp"

namespace vram {
    namespace entry {
        entry_t::entry_t() {
            timespec t;
            clock_gettime(CLOCK_REALTIME, &t);

            atime = t;
            mtime = t;
            ctime = t;
        }

        void entry_t::unlink() {
            if (parent) {
                parent->ctime = parent->mtime = util::time();
                parent->children.erase(name);
            }
        }

        void entry_t::move(dir_ptr new_parent, const string& new_name) {
            parent->children.erase(name);

            parent = new_parent;
            name = new_name;

            new_parent->children[new_name] = entry_ref(self_ref);
        }
    }
}
