CC = g++
CFLAGS = -march=native -O2 -flto -Wall -Werror -std=c++11 `pkg-config fuse --cflags` -I include/
LDFLAGS = `pkg-config fuse --libs` -l OpenCL

ifeq ($(DEBUG), 1)
	CFLAGS += -g -DDEBUG
endif

ifeq ($(OPENCL_1_1), 1)
	CFLAGS += -DOPENCL_1_1
endif

bin/vramfs: bin/vramfs.o bin/memory.o
	$(CC) -o bin/vramfs bin/*.o $(LDFLAGS)

bin/%.o: src/%.cpp
	@mkdir -p bin
	$(CC) $(CFLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -rf bin/
