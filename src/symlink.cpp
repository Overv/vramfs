#include "entry.hpp"
#include "util.hpp"

namespace vram {
    namespace entry {
        symlink_ref symlink_t::make(dir_ptr parent, const string& name, const string& target) {
            auto symlink = std::make_shared<symlink_t>();

            symlink->self_ref = symlink;

            symlink->parent = parent;
            symlink->name = name;
            symlink->target = target;

            if (parent) {
                parent->children[name] = symlink;
            }

            return symlink;
        }

        type::type_t symlink_t::type() const {
            return type::symlink;
        }

        size_t symlink_t::size() const {
            return target.size();
        }
    }
}
