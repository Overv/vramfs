CC = g++
CFLAGS = -Wall -Werror -std=c++11

VRAMFS: VRAMFS.cpp
	$(CC) $(CFLAGS) -o VRAMFS VRAMFS.cpp `pkg-config fuse --cflags --libs` -l sqlite3