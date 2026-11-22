VERSION := 0.38.0
.PHONY: all test audit audit-transfer package clean
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
		src/VirtualOperation.cpp src/VirtualFile.cpp src/Sha256.cpp -o build/transfer-faults
	build/transfer-faults
package: all
	sh packaging/package.sh $(VERSION)
clean:
	$(MAKE) -C src clean
