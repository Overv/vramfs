#ifndef VRAM_DEBUGCL_HPP
#define VRAM_DEBUGCL_HPP

/*
 * Minimal OpenCL implementation for better debugging with valgrind
 */

#include <vector>
#include <cstring>

#define CL_CALLBACK

const int CL_MEM_READ_WRITE = 0;
const int CL_SUCCESS = 0;
const int CL_DEVICE_TYPE_GPU = 0;
const int CL_COMPLETE = 0;

typedef int cl_event;
typedef int cl_int;

typedef CL_CALLBACK void (*callback_fn)(cl_event, cl_int, void*);

namespace cl {
    class Device {

    };

    class Platform {
    public:
        static void getDevices(int type, std::vector<Device>* devices) {
            devices->push_back(Device());
        }

        static void get(std::vector<Platform>* platforms) {
            platforms->push_back(Platform());
        }
    };

    class Context {
    public:
        Context() {}
        Context(Device& device) {}
    };

    class Buffer {
    public:
        Buffer(Context& ctx, int flags, int64_t size, void* host_ptr = nullptr, int* err = nullptr) {
            data = new char[size];
            if (err) *err = CL_SUCCESS;
        }

        ~Buffer() {
            delete [] data;
        }

        char* data;
    };

    class CommandQueue {
    public:
        CommandQueue() {}
        CommandQueue(Context& ctx, Device& device) {}

        int enqueueFillBuffer(Buffer& buf, int pattern, int off, int size, void* events, void* event) {
            memset(&buf.data[off], 0, size);
            return CL_SUCCESS;
        }

        int enqueueCopyBuffer(Buffer& src, Buffer& dst, int offSrc, int offDst, int size, void* events, void* event) {
            memcpy(&dst.data[offDst], &src.data[offSrc], size);
            return CL_SUCCESS;
        }

        int enqueueReadBuffer(Buffer& buf, bool block, int off, int size, char* out, void* events, void* event) {
            memcpy(out, &buf.data[off], size);
            return CL_SUCCESS;
        }

        int enqueueWriteBuffer(Buffer& buf, bool block, int off, int size, const char* in, void* events, void* event) {
            memcpy(&buf.data[off], in, size);
            return CL_SUCCESS;
        }

        int finish() {
            return CL_SUCCESS;
        }
    };

    class Event {
    public:
        void setCallback(int flag, callback_fn cb, void* userdata) {
            cb(0, 0, userdata);
        }
    };
}

#endif
