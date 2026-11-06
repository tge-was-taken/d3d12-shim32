PREFIX ?= $(HOME)/.local/opt/mesa-dzn32
DX ?= $(HOME)/src/mesa-dzn/subprojects/DirectX-Headers-1.0/include
SERVER = $(PREFIX)/libexec/d3d12-shim-server
ICD_DIR = $(HOME)/.local/share/vulkan/icd.d

INCLUDES = -I$(DX) -I$(DX)/wsl/stubs -I$(DX)/directx
CFLAGS = -std=gnu11 -O2 -g -Wall
CXXFLAGS = -std=c++17 -O2 -g -Wall

CLIENT = $(patsubst client/%,build/client/%.o,$(wildcard client/*.c client/*.cpp))
TESTS = build/egltest build/depthflip build/glxtest build/vktest build/vkwsi

all: build/libd3d12.so build/d3d12-shim-server

build/client/%.c.o: client/%.c client/shim.h client/cache.h proto.h | build
	$(CC) -m32 -fPIC -fvisibility=hidden $(CFLAGS) $(INCLUDES) -DSERVER_PATH='"$(SERVER)"' -c -o $@ $<

build/client/%.cpp.o: client/%.cpp client/cache.h | build
	$(CXX) -m32 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti $(CXXFLAGS) -c -o $@ $<

# libstdc++ is linked statically so apps without it can load the library.
build/libd3d12.so: $(CLIENT)
	$(CXX) -m32 -shared -Wl,--no-undefined -static-libstdc++ -static-libgcc -o $@ $^ -lpthread

build/d3d12-shim-server: server/server.cpp proto.h | build
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< -ldl -lpthread

build/egltest: tests/egltest.c | build
	$(CC) -m32 $(CFLAGS) -o $@ $< -lEGL -lGLESv2

build/depthflip: tests/depthflip.c | build
	$(CC) -m32 $(CFLAGS) -o $@ $< -lEGL -lOpenGL

build/glxtest: tests/glxtest.c | build
	$(CC) -m32 $(CFLAGS) -o $@ $< -lGL -lX11

build/vktest: tests/vktest.c build/compute.h
	$(CC) -m32 $(CFLAGS) -Ibuild -o $@ $< -lvulkan

build/vkwsi: tests/vkwsi.c | build
	$(CC) -m32 $(CFLAGS) -o $@ $< -lvulkan -lX11

build/compute.h: tests/compute.comp | build
	glslangValidator -V --quiet --vn compute_spv -o $@ $<

tests: $(TESTS)

build:
	mkdir -p build/client

install: all
	install -D -m755 build/libd3d12.so $(PREFIX)/lib/libd3d12.so
	ln -sf libd3d12.so $(PREFIX)/lib/libdxcore.so
	install -D -m755 build/d3d12-shim-server $(SERVER)
	install -m644 install-readme.md $(PREFIX)/README.md
	mkdir -p $(ICD_DIR)
	ln -sf $(PREFIX)/share/vulkan/icd.d/dzn_icd.i686.json $(ICD_DIR)/

check: tests
	./build/egltest
	./build/depthflip
	./build/vktest
	./build/glxtest
	./build/vkwsi

clean:
	rm -rf build

.PHONY: all tests install check clean
