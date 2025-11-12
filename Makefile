CXX ?= g++
CXXFLAGS += -std=c++20 $(shell pkg-config fuse3 --cflags) -I include/
LDFLAGS += $(shell pkg-config fuse3 --libs) -l OpenCL

ifeq ($(DEBUG), 1)
	CXXFLAGS += -g -DDEBUG -Wall -Werror
endif

bin/vramfs: build/util.o build/memory.o build/entry.o build/file.o build/dir.o build/symlink.o build/vramfs.o | bin
	$(CXX) -o $@ $^ $(LDFLAGS)

build bin:
	@mkdir -p $@

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

.PHONY: clean
clean:
	rm -rf build/ bin/

.PHONY: install
install: bin/vramfs
	install -d $(DEST)/usr/local/bin/
	install bin/vramfs $(DEST)/usr/local/bin/
	install fuse3.vramfs $(DEST)/usr/local/bin/

.PHONY: uninstall
uninstall:
	$(RM) $(DEST)/usr/local/bin/fuse3.vramfs
	$(RM) $(DEST)/usr/local/bin/vramfs