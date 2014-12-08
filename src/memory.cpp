#include "memory.hpp"

namespace vram {
    namespace memory {
        // Connection with OpenCL
        bool ready = false;
        cl::Context context;
        cl::Device device;
        cl::CommandQueue queue;

    #ifdef OPENCL_1_1
        cl::Buffer zero_buffer;
    #endif

        // Find platform with OpenCL capable GPU
        static bool init_opencl() {
            if (ready) return true;

            std::vector<cl::Platform> platforms;
            cl::Platform::get(&platforms);
            if (platforms.size() == 0) return false;

            for (auto& platform : platforms) {
                std::vector<cl::Device> gpu_devices;
                platform.getDevices(CL_DEVICE_TYPE_GPU, &gpu_devices);
                if (gpu_devices.size() == 0) continue;

                device = gpu_devices[0];
                context = cl::Context(gpu_devices);
                queue = cl::CommandQueue(context, device);

            #ifdef OPENCL_1_1
                char zero_data[block::size] = {};
                int r;
                zero_buffer = cl::Buffer(context, CL_MEM_READ_ONLY, block::size, nullptr, &r);
                if (r != CL_SUCCESS) return false;
                r = queue.enqueueWriteBuffer(zero_buffer, true, 0, block::size, zero_data, nullptr, nullptr);
                if (r != CL_SUCCESS) return false;
            #endif

                return true;
            }

            return false;
        }

        // Called for asynchronous writes to clean up the data copy
        static CL_CALLBACK void async_write_dealloc(cl_event, cl_int, void* data) {
            delete [] reinterpret_cast<char*>(data);
        }

        bool is_available() {
            return (ready = init_opencl());
        }

        block::block() {}

        block::block(bool& success) {
            int r;
            buffer = cl::Buffer(context, CL_MEM_READ_WRITE, size, nullptr, &r);

            if (r == CL_SUCCESS) {
            #ifdef OPENCL_1_1
                r = queue.enqueueCopyBuffer(zero_buffer, buffer, 0, 0, size, nullptr, nullptr);
            #else
                r = queue.enqueueFillBuffer(buffer, 0, 0, size, nullptr, nullptr);
            #endif
            }

            success = r == CL_SUCCESS;
        }

        void block::read(off_t offset, size_t size, void* data) const {
            // Queue is configured for in-order execution, so writes before this
            // are guaranteed to be completed first
            queue.enqueueReadBuffer(buffer, true, offset, size, data, nullptr, nullptr);
        }

        void block::write(off_t offset, size_t size, const void* data, bool async) {
            if (async) {
                char* data_copy = new char[size];
                memcpy(data_copy, data, size);
                data = data_copy;
            }

            auto event = std::make_shared<cl::Event>();
            queue.enqueueWriteBuffer(buffer, !async, offset, size, data, nullptr, event.get());

            if (async) {
                event->setCallback(CL_COMPLETE, async_write_dealloc, const_cast<void*>(data));
            }

            last_write = event;
        }

        void block::sync() {
            last_write->wait();
        }
    }
}
