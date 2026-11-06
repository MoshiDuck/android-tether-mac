CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra
override CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS  += $(shell pkg-config --libs libusb-1.0) -lpthread

# Tests unitaires + robustesse, compilés avec ASan/UBSan (aucun téléphone requis)
TEST_FLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined

rndis-tether: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

tests/test_main: tests/test_main.c main.c
	$(CC) $(CFLAGS) $(TEST_FLAGS) -o $@ tests/test_main.c $(LDLIBS)

test: tests/test_main
	./tests/test_main

clean:
	rm -f rndis-tether tests/test_main
	rm -rf tests/test_main.dSYM

.PHONY: test clean
