#include "entry.hpp"
#include "util.hpp"

namespace vram {
    namespace entry {
        entry_t::entry_t() {
            auto t = util::time();

            _atime = t;
            _mtime = t;
            _ctime = t;
        }

        void entry_t::link(dir_ptr parent, const string& name) {
             _parent = parent;
            this->name = name;

            if (parent) {
                parent->_children[name] = shared_from_this();
            }
        }

        dir_ptr entry_t::parent() const {
            return _parent;
        }

        timespec entry_t::atime() const {
            return _atime;
        }

        timespec entry_t::mtime() const {
            return _mtime;
        }

        timespec entry_t::ctime() const {
            return _ctime;
        }

        void entry_t::atime(timespec t) {
            _atime = t;
            _ctime = util::time();
        }

        void entry_t::mtime(timespec t) {
            _mtime = t;
            _ctime = util::time();
        }

        void entry_t::ctime(timespec t) {
            _ctime = t;
        }

        void entry_t::unlink() {
            if (_parent) {
                _parent->mtime(util::time());
                _parent->_children.erase(name);
            }
        }

        void entry_t::move(dir_ptr new_parent, const string& new_name) {
            if (_parent) _parent->_children.erase(name);

            _parent = new_parent;
            name = new_name;

            new_parent->_children[new_name] = shared_from_this();
        }
    }
}
