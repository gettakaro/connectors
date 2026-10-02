.PHONY: all build native test clean shell
all: build
build:            ## build the .so in the debian:bookworm container
	./build.sh
native:           ## build on the host toolchain
	./build.sh --native
test:             ## build and run every test suite in the container
	./build.sh --tests
symbols:          ## dump the wanted symbol table for the installed server
	python3 tools/symdump.py
clean:
	rm -rf build dist tests/build
