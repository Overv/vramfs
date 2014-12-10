#!/bin/bash
mkdir -p /tmp/vram
bin/vramfs /tmp/vram -o big_writes -o auto_unmount -o default_permissions && echo "mounted VRAM file system at /tmp/vram"
