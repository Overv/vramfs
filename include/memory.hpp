#ifndef VRAM_MEMORY_HPP
#define VRAM_MEMORY_HPP

/*
 * VRAM block allocation
 */


#ifdef DEBUG
    // Use minimal OpenCL implementation for better debugging with valgrind
    #include "CL/debugcl.hpp"
#elif OPENCL_1_1
    // OpenCL 1.1 header uses deprecated APIs
    #define CL_USE_DEPRECATED_OPENCL_1_1_APIS
    #include <CL/cl_1_1.hpp>
#else
    #include <CL/cl.hpp>
#endif

#include <memory>

namespace vram {
    namespace memory {
        // Check if current machine supports VRAM allocation
        bool is_available();

        /*
         * Block of allocated VRAM
         */

        class block {
        public:
            // Nicely fits FUSE read/write size
            static const size_t size = 128 * 1024;

            // Block constructed this way can only be used as placeholder
            block();

            block(bool& success);

            void read(off_t offset, size_t size, void* data) const;

            // Data is internally copied if called with async = true
            void write(off_t offset, size_t size, const void* data, bool async = false);

            // Wait for all writes to this block to complete
            void sync();

        private:
            // OpenCL C++ API objects are shared_ptrs internally
            cl::Buffer buffer;
            std::shared_ptr<cl::Event> last_write;
        };
    }
}

#endif
