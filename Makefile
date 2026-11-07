CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra
override CFLAGS += $(shell pkg-config --cflags libusb-1.0)
LDLIBS  += $(shell pkg-config --libs libusb-1.0) -lpthread

SRC  = src/main.c src/dhcp.c
HDR  = $(wildcard src/*.h)
TEST_SRC = tests/test_main.c tests/test_dhcp.c src/dhcp.c

# Avertissements stricts : utilisés par la CI (cible `strict`)
STRICT_FLAGS = -O2 -Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes -Wpointer-arith -Wcast-qual \
               -Wwrite-strings -Wmissing-prototypes -Wnull-dereference -Werror

# Tests unitaires + robustesse, compilés avec ASan/UBSan (aucun téléphone requis)
TEST_FLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined

# Couverture de code (LLVM source-based coverage)
COV_FLAGS = -g -O0 -fprofile-instr-generate -fcoverage-mapping
LLVM_COV      = $(shell xcrun --find llvm-cov 2>/dev/null || command -v llvm-cov)
LLVM_PROFDATA = $(shell xcrun --find llvm-profdata 2>/dev/null || command -v llvm-profdata)

rndis-tether: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

# Compile avec tous les avertissements en erreurs (sans produire de binaire)
strict: $(SRC) $(HDR)
	@for f in $(SRC); do echo "strict: $$f"; $(CC) $(STRICT_FLAGS) $(CFLAGS) -c $$f -o /dev/null || exit 1; done

# Analyseur statique de clang : toute alerte fait échouer la cible
analyze: $(SRC) $(HDR)
	@for f in $(SRC); do \
	  echo "analyze: $$f"; \
	  out=$$($(CC) --analyze $(CFLAGS) $$f -o /dev/null 2>&1); \
	  if [ -n "$$out" ]; then echo "$$out"; exit 1; fi; \
	done

tests/test_main: $(TEST_SRC) src/main.c $(HDR) tests/check.h
	$(CC) $(CFLAGS) $(TEST_FLAGS) -o $@ $(TEST_SRC) $(LDLIBS)

test: tests/test_main
	./tests/test_main

tests/test_cov: $(TEST_SRC) src/main.c $(HDR) tests/check.h
	$(CC) $(CFLAGS) $(COV_FLAGS) -o $@ $(TEST_SRC) $(LDLIBS)

# Rapport de couverture ; scripts/check-coverage.sh vérifie les seuils des modules purs
coverage: tests/test_cov
	rm -f tests/cov.profraw
	LLVM_PROFILE_FILE=tests/cov.profraw ./tests/test_cov
	$(LLVM_PROFDATA) merge -sparse tests/cov.profraw -o tests/cov.profdata
	$(LLVM_COV) report ./tests/test_cov -instr-profile=tests/cov.profdata $(SRC)

clean:
	rm -f rndis-tether tests/test_main tests/test_cov tests/cov.profraw tests/cov.profdata
	rm -rf tests/test_main.dSYM tests/test_cov.dSYM

.PHONY: strict analyze test coverage clean
