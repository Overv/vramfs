#ifndef VRAM_UTIL_HPP
#define VRAM_UTIL_HPP

/*
 * Utility functions
 */

namespace vram {
    namespace util {
        // Error function that can be combined with a return statement to return *ret*
        template<typename T>
        T fatal_error(const string& error, T ret) {
            std::cerr << "error: " << error << std::endl;
            fuse_exit(fuse_get_context()->fuse);
            return ret;
        }

        // Get current time with nanosecond precision
        timespec time() {
            timespec tv;
            clock_gettime(CLOCK_REALTIME_COARSE, &tv);
            return tv;
        }

        // Split path/to/file.txt into "path/to" and "file.txt"
        void split_file_path(const string& path, string& dir, string& file) {
            size_t p = path.rfind("/");

            if (p == string::npos) {
                dir = "";
                file = path;
            } else {
                dir = path.substr(0, p);
                file = path.substr(p + 1);
            }

            if (dir.size() == 0) dir = "/";
        }
    }
}

#endif
