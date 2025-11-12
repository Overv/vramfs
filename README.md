vramfs
======

Unused RAM is wasted RAM, so why not put some of that VRAM in your graphics card
to work?

vramfs is a utility that uses the [FUSE library](http://fuse.sourceforge.net/)
to create a file system in VRAM. The idea is pretty much the same as a ramdisk,
except that it uses the video RAM of a discrete graphics card to store
files. It is not intented for serious use, but it does actually work fairly
well, especially since consumer GPUs with 4GB or more VRAM are now available.

On the developer's system, the continuous read performance is ~2.4 GB/s and
write performance 2.0 GB/s, which is about 1/3 of what is achievable with a
ramdisk. That is already decent enough for a device not designed for large data
transfers to the host, but future development should aim to get closer to the
PCI-e bandwidth limits. See the *benchmarks* section for more info.

#### Requirements

- Linux with kernel 2.6+
- FUSE development files
- A graphics card with support for OpenCL 1.2

#### Building

First, install the OpenCL driver for your graphics card and verify that it's
recognized as an OpenCL device by running `clinfo`. Then install the `libfuse3-dev`
package or build it from source. You will also need `pkg-config` and OpenCL 
development files, (`opencl-dev`, `opencl-clhpp-headers` package or equivalent), 
with version 1.2 of the OpenCL headers at least.

Just run `make` to build `vramfs`.

If you want to debug with valgrind, you should compile with the minimal fake
OpenCL implementation to avoid filling your screen with warnings caused by the
OpenCL driver:

* **valgrind:** `make DEBUG=1`

#### Installing

For a recommended way to install see the Makefile’s `install` target. You can
invoke the install target directly be running `sudo make install` after
finishing the build.

#### Mounting

Mount a disk by running `bin/vramfs <mountdir> <size>`. The `mountdir` can be
any empty directory. The `size` is the disk size in bytes. For more information,
run `bin/vramfs` without arguments.

The recommended maximum size of a vramdisk is 50% of your VRAM. If you go over
that, your driver or system may become unstable because it has to start
swapping. For example, webpages in Chrome will stop rendering properly.

If the disk has been inactive for a while, the graphics card will likely lower
its memory clock, which means it'll take a second to get up to speed again.

Implementation
--------------

The FUSE library is used to implement vramfs as a user space file system. This
eases development and makes working with APIs such as OpenCL straightforward.

#### Basic architecture

![Architecture overview](http://i.imgur.com/e8tQ168.png)

When the program is started, it checks for an OpenCL capable GPU and attempts to
allocate the specified amount of memory. Once the memory has been allocated, the
root entry object is created and a global reference to it is stored.

FUSE then forwards calls like `stat`, `readdir` and `write` to the file system
functions. These will then locate the entry through the root entry using the
specified path. The required operations will then be performed on the entry
object. If the entry is a file object, the operation may lead to OpenCL
`cvEnqueueReadBuffer` or `cvEnqueueWriteBuffer` calls to manipulate the data.

When a file is created or opened, a `file_session` object is created to store
the reference to the file object and any other data that is persistent between
an `fopen` and `fclose` call.

#### VRAM block allocation

OpenCL is used to allocate blocks of memory on the graphics card by creating
buffer objects. When a new disk is mounted, a pool of `disk size / block size`
buffers is created and initialised with zeros. That is not just a good practice,
but it's also required with some OpenCL drivers to check if the VRAM required
for the block is actually available. Unfortunately Nvidia cards don't support
OpenCL 1.2, which means the `cvEnqueueFillBuffer` call has to be simulated by
copying from a preallocated buffer filled with zeros. Somewhat interestingly, it
doesn't seem to make a difference in performance on cards that support both.

Writes to blocks are generally asynchronous, whereas reads are synchronous.
Luckily, OpenCL guarantees in-order execution of commands by default, which
means reads of a block will wait for the writes to complete. OpenCL 1.1 is
completely thread safe, so no special care is required when sending commands.

Block objects are managed using a `shared_ptr` so that they can automatically
reinsert themselves into the pool on deconstruction.

#### File system

The file system is a tree of `entry_t` objects with members for attributes like
the parent directory, mode and access time. Each type of entry has its own
subclass that derives from it: `file_t`, `dir_t` and `symlink_t`. The main file
that implements all of the FUSE callbacks has a permanent reference to the root
directory entry.

The `file_t` class contains extra `write`, `read` and `size` methods and manages
the blocks to store the file data.

The `dir_t` class has an extra `unordered_map` that maps names to `entry_t`
references for quick child lookup using its member function `find`.

Finally, the `symlink_t` class has an extra `target` string member that stores
the pointer of the symlink.

All of the entry objects are also managed using `shared_ptr` so that an object
and its data (e.g. file blocks) are automatically deallocated when they're
unlinked and no process holds a file handle to them anymore. This can also be
used to easily implement hard links later on.

The classes use getter/setter functions to automatically update the access,
modification and change times at the appropriate moment. For example, calling
the `children` member function of `dir_t` changes the access time and change
time of the directory.

#### Thread safety

Unfortunately most of the operations are not thread safe, so all of the FUSE
callbacks share a mutex to ensure that only one thread is mutating the file
system at a time. The exceptions are `read` and `write`, which will temporarily
release the lock while waiting for a read or write to complete.

Benchmarks
----------

The system used for testing has the following specifications:

* **OS:** Ubuntu 14.04.01 LTS (64 bit)
* **CPU:** Intel Core i5-2500K @ 4.0 Ghz
* **RAM:** 8GB DDR3-1600
* **GPU:** AMD R9 290 4GB (Sapphire Tri-X)

Performance of continuous read, write and write+sync has been measured for
different block allocation sizes by creating a new 2GiB disk for each new size
and reading/writing a 2GiB file.

The disk is created using:

    bin/vramfs /tmp/vram 2G

And the file is written and read using the `dd` command:

    # write
    dd if=/dev/zero of=/tmp/vram/test bs=128K count=16000

    # write+sync
    dd if=/dev/zero of=/tmp/vram/test bs=128K count=16000 conv=fdatasync

    # read
    dd if=/tmp/vram/test of=/dev/null bs=128K count=16000

These commands were repeated 5 times for each block size and then averaged to
produce the results shown in the graph. No block sizes lower than 32KiB could
be tested because the driver would fail to allocate that many OpenCL buffers.
This may be solved in the future by using subbuffers.

![Performance for different block sizes](http://i.imgur.com/93UNs1u.png)

Although 128KiB blocks offers the highest performance, 64KiB may be preferable
because of the lower space overhead.

Future ideas
------------

- Implement RAID-0 for SLI/Crossfire setups

License
-------

    The MIT License (MIT)

    Copyright (c) 2014 Alexander Overvoorde

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.
