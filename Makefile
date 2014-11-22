CC = g++
CFLAGS = -O3 -flto -Wall -Werror -std=c++11

ifeq ($(DEBUG), 1)
	CFLAGS += -g -DDEBUG
endif

vramfs: vramfs.cpp types.hpp
	$(CC) $(CFLAGS) -o vramfs vramfs.cpp `pkg-config fuse --cflags --libs` -l sqlite3 -l OpenCL

.PHONY: clean
clean:
	rm -f vramfs
