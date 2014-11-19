CC = g++
CFLAGS = -Wall -Werror -std=c++11

vramfs: vramfs.cpp types.hpp
	$(CC) $(CFLAGS) -o vramfs vramfs.cpp `pkg-config fuse --cflags --libs` -l sqlite3 -l OpenCL

.PHONY: clean
clean:
	rm -f vramfs
