CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -Iinclude
FUSE_CFLAGS = $(shell pkg-config --cflags fuse3)
FUSE_LIBS = $(shell pkg-config --libs fuse3)
KDIR ?= /lib/modules/$(shell uname -r)/build

all: userspace module

userspace: mkfs.logfs liblogfs.a test_lfs mount.logfs

module:
	$(MAKE) -C kernel KDIR="$(KDIR)"

# The Phase 1 library and driver still build without FUSE installed.
phase1: mkfs.logfs liblogfs.a test_lfs

mount.logfs: tools/mount.cpp liblogfs.a include/lfs.h
	@pkg-config --exists fuse3 || (echo "Install libfuse3-dev and pkg-config first"; exit 1)
	$(CXX) $(CXXFLAGS) $(FUSE_CFLAGS) tools/mount.cpp liblogfs.a $(FUSE_LIBS) -o mount.logfs

mkfs.logfs: tools/mkfs.cpp src/disk.cpp include/logfs.h
	$(CXX) $(CXXFLAGS) tools/mkfs.cpp src/disk.cpp -o mkfs.logfs

disk.o: src/disk.cpp include/logfs.h
	$(CXX) $(CXXFLAGS) -c src/disk.cpp -o disk.o

cache.o: src/cache.cpp include/cache.h include/logfs.h
	$(CXX) $(CXXFLAGS) -c src/cache.cpp -o cache.o

lfs.o: src/lfs.cpp include/lfs.h include/cache.h include/logfs.h
	$(CXX) $(CXXFLAGS) -c src/lfs.cpp -o lfs.o

gc.o: src/gc.cpp include/lfs.h include/cache.h include/logfs.h
	$(CXX) $(CXXFLAGS) -c src/gc.cpp -o gc.o

liblogfs.a: disk.o cache.o lfs.o gc.o
	ar rcs liblogfs.a disk.o cache.o lfs.o gc.o

test_lfs: tests/test_lfs.cpp liblogfs.a include/lfs.h include/cache.h
	$(CXX) $(CXXFLAGS) tests/test_lfs.cpp liblogfs.a -o test_lfs

check: phase1
	./test_lfs

clean:
	rm -f mkfs.logfs test_lfs mount.logfs disk.o cache.o lfs.o gc.o liblogfs.a
	$(MAKE) -C kernel clean

.PHONY: all userspace module phase1 check clean
