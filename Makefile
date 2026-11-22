VERSION := 0.38.0
.PHONY: all test audit audit-transfer audit-sanitize fuzz-parsers package clean
all:
	$(MAKE) -C src
test:
	$(MAKE) -C src listi-self-test
audit: all
	python3 tests/run-native-audit.py $(TESTS)
# Standalone POSIX fixture; does not replace the native Haiku audit.
audit-transfer:
	mkdir -p build
	$(CXX) -std=c++17 -Isrc tests/virtual-operation-faults.cpp \
		src/VirtualOperation.cpp src/VirtualFile.cpp src/VirtualLocation.cpp src/Sha256.cpp -o build/transfer-faults
	build/transfer-faults
# Portable checks; ASan/UBSan require a compiler with those runtimes.
audit-sanitize:
	mkdir -p build
	$(CXX) -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
		tests/virtual-operation-faults.cpp src/VirtualOperation.cpp src/VirtualFile.cpp \
		src/VirtualLocation.cpp src/Sha256.cpp -o build/transfer-sanitized
	UBSAN_OPTIONS=halt_on_error=1 build/transfer-sanitized
fuzz-parsers:
	mkdir -p build
	$(CXX) -std=c++17 -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc \
		tests/parsing-fuzz.cpp src/VirtualLocation.cpp src/Sha256.cpp -o build/parsing-fuzz
	UBSAN_OPTIONS=halt_on_error=1 build/parsing-fuzz
package: all
	sh packaging/package.sh $(VERSION)
clean:
	$(MAKE) -C src clean
