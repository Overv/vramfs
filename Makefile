CC = g++
CFLAGS = -Wall -Wpedantic -Werror -std=c++11 $(shell pkg-config fuse3 --cflags) -I include/
LDFLAGS = -flto $(shell pkg-config fuse3 --libs) -l OpenCL -l fuse

ifeq ($(DEBUG), 1)
	CFLAGS += -g -DDEBUG -Wall -Werror -std=c++11
else
	CFLAGS += -march=native -O2 -flto
endif

bin/vramfs: build/util.o build/memory.o build/entry.o build/file.o build/dir.o build/symlink.o build/vramfs.o | bin
	$(CC) -o $@ $^ $(LDFLAGS)

build bin:
	@mkdir -p $@

build/%.o: src/%.cpp | build
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -rf build/ bin/
