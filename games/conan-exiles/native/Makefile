.PHONY: build test windows clean
build:            ## build dist/libtakaro-conan-native.so in the buster container
	./platform/linux/build.sh
test:             ## build, then run every host test in the buster container
	./platform/linux/build.sh --tests
windows:          ## cross-compile dist-windows/winmm.dll with zig 0.13.0
	./platform/windows/build.sh
clean:
	rm -rf build dist tests/build build-windows dist-windows
