#!/bin/bash
mkdir -p /tmp/vram
bin/vramfs /tmp/vram -o big_writes && echo "mounted VRAM file system at /tmp/vram"
