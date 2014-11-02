VRAMFS
======

VRAMFS is a utility that uses the [FUSE library](http://fuse.sourceforge.net/)
to implement a file system in VRAM. The idea is pretty much the same as a RAM
disk, except that it uses the video RAM of a discrete graphics card to store
files. It is not intented for serious use, but it does actually work fairly
well, especially since consumer GPUs with 4GB or more VRAM are now available.

TODO: Write about implementation

Requirements
------------

- Linux (kernel 2.4.* or 2.6.*)
- FUSE + SQLite development files
- A graphics card with support for OpenCL 1.1+

Instructions
------------

TODO

Benchmarks
----------

TODO

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