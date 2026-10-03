.PHONY: build test clean
build:            ## build dist/libtakaro-conan-native.so in the buster container
	./platform/linux/build.sh
test:             ## build, then run every host test in the buster container
	./platform/linux/build.sh --tests
clean:
	rm -rf build dist tests/build
