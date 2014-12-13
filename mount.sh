#!/bin/bash
mkdir -p /tmp/vram
bin/vramfs /tmp/vram 256M -f && echo "mounted VRAM file system at /tmp/vram"
