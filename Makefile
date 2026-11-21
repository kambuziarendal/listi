VERSION := 0.38.0
.PHONY: all test audit package clean
all:
	$(MAKE) -C src
test:
	$(MAKE) -C src listi-self-test
audit: all
	python3 tests/run-native-audit.py $(TESTS)
package: all
	sh packaging/package.sh $(VERSION)
clean:
	$(MAKE) -C src clean
