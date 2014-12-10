#include "entry.hpp"
#include "util.hpp"

namespace vram {
    namespace entry {
        symlink_ref symlink_t::make(dir_ptr parent, const string& name, const string& target) {
            auto symlink = symlink_ref(new symlink_t(target));
            symlink->link(parent, name);
            return symlink;
        }

        symlink_t::symlink_t(const string& target) : target(target) {}

        type::type_t symlink_t::type() const {
            return type::symlink;
        }

        size_t symlink_t::size() const {
            return target.size();
        }
    }
}
