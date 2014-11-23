#!/bin/bash
mkdir -p /tmp/vram
./vramfs /tmp/vram -o big_writes && echo "mounted VRAM file system at /tmp/vram"
