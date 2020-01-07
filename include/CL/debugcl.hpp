#ifndef VRAM_DEBUGCL_HPP
#define VRAM_DEBUGCL_HPP

/*
 * Minimal OpenCL implementation for better debugging with valgrind
 */

#include <vector>
#include <cstring>
#include <memory>

#define CL_CALLBACK

const int CL_MEM_READ_WRITE = (1 << 0);
const int CL_MEM_READ_ONLY = (1 << 2);
const int CL_MEM_COPY_HOST_PTR = (1 << 5);
const int CL_SUCCESS = 0;
const int CL_DEVICE_TYPE_GPU = 0;
const int CL_COMPLETE = 0;

const int CL_DEVICE_NAME = 0x102B;

typedef int cl_event;
typedef int cl_int;
typedef unsigned int cl_uint;

typedef CL_CALLBACK void (*callback_fn)(cl_event, cl_int, void*);

namespace cl {
    class Device {
    public:
        template <int T>
        const char* getInfo() {
            return "DEBUG DEVICE";
        }
    };

    class Platform {
    public:
        static void getDevices(int type, std::vector<Device>* devices) {
            devices->push_back(Device());
        }

        static void get(std::vector<Platform>* platforms) {
            platforms->push_back(Platform());
        }

        void* operator()() {
            return nullptr;
        }
    };

    class Context {
    public:
        Context() {}
        Context(std::vector<Device>& devices) {}
    };

    class Buffer {
    public:
        Buffer() {
            data = std::make_shared<std::vector<char>>();
        }

        Buffer(Context& ctx, int flags, int64_t size, void* host_ptr = nullptr, int* err = nullptr) {
            data = std::make_shared<std::vector<char>>();
            data->resize(size);
            if (err) *err = CL_SUCCESS;

            if (flags & CL_MEM_COPY_HOST_PTR) {
                memcpy(data->data(), host_ptr, size);
            }
        }

        std::shared_ptr<std::vector<char>> data;
    };

    class Event {
    public:
        void setCallback(int flag, callback_fn cb, void* userdata) {
            cb(0, 0, userdata);
        }

        void wait() {}
    };

    class CommandQueue {
    public:
        CommandQueue() {}
        CommandQueue(Context& ctx, Device& device) {}

        int enqueueFillBuffer(const Buffer& buf, int pattern, int off, int size, std::vector<cl::Event>* events, cl::Event* event) {
            memset(&buf.data->operator[](off), 0, size);
            return CL_SUCCESS;
        }

        int enqueueCopyBuffer(const Buffer& src, Buffer& dst, int offSrc, int offDst, int size, std::vector<cl::Event>* events, cl::Event* event) {
            memcpy(&dst.data->operator[](offDst), &src.data->operator[](offSrc), size);
            return CL_SUCCESS;
        }

        int enqueueReadBuffer(const Buffer& buf, bool block, int off, int size, void* out, std::vector<cl::Event>* events, cl::Event* event) {
            memcpy(out, &buf.data->operator[](off), size);
            return CL_SUCCESS;
        }

        int enqueueWriteBuffer(const Buffer& buf, bool block, int off, int size, const void* in, std::vector<cl::Event>* events, cl::Event* event) {
            memcpy(&buf.data->operator[](off), in, size);
            return CL_SUCCESS;
        }

        int finish() {
            return CL_SUCCESS;
        }
    };

    namespace detail {
        inline cl_uint getPlatformVersion(void* platform) {
            return 0;
        }
    }
}

#endif
