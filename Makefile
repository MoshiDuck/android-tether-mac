CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra
override CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS  += $(shell pkg-config --libs libusb-1.0) -lpthread

SRC  = src/main.c src/dhcp.c
HDR  = $(wildcard src/*.h)
TEST_SRC = tests/test_main.c tests/test_dhcp.c src/dhcp.c

# Tests unitaires + robustesse, compilés avec ASan/UBSan (aucun téléphone requis)
TEST_FLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined

rndis-tether: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

tests/test_main: $(TEST_SRC) src/main.c $(HDR) tests/check.h
	$(CC) $(CFLAGS) $(TEST_FLAGS) -o $@ $(TEST_SRC) $(LDLIBS)

test: tests/test_main
	./tests/test_main

clean:
	rm -f rndis-tether tests/test_main
	rm -rf tests/test_main.dSYM

.PHONY: test clean
