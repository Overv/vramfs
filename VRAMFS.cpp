// FUSE library
#define FUSE_USE_VERSION 26
#include <fuse.h>

// Implemented file system operations
static struct vram_operations : fuse_operations {
    vram_operations() {
        // TODO: Implement operations
    }
} operations;

// FUSE initialisation
int main(int argc, char* argv[]) {
    return fuse_main(argc, argv, &operations, nullptr);
}