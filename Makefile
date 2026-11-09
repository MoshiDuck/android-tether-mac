CC      ?= clang
VERSION := $(shell cat VERSION)
CFLAGS  ?= -O2 -Wall -Wextra
override CFLAGS += $(shell pkg-config --cflags libusb-1.0) -DRNDIS_VERSION='"$(VERSION)"'
LDLIBS  += $(shell pkg-config --libs libusb-1.0) -lpthread

SRC  = src/main.c src/dhcp.c src/policy.c src/ipc.c src/diagnose.c src/logsan.c src/adb.c
HDR  = $(wildcard src/*.h)
TEST_SRC = tests/test_main.c tests/test_dhcp.c tests/test_policy.c tests/test_ipc.c tests/test_diagnose.c tests/test_logsan.c tests/test_adb.c src/dhcp.c src/policy.c src/ipc.c src/diagnose.c src/logsan.c src/adb.c

# Avertissements stricts : utilisés par la CI (cible `strict`)
STRICT_FLAGS = -O2 -Wall -Wextra -Wshadow -Wformat=2 -Wstrict-prototypes -Wpointer-arith -Wcast-qual \
               -Wwrite-strings -Wmissing-prototypes -Wnull-dereference -Werror

# Tests unitaires + robustesse, compilés avec ASan/UBSan (aucun téléphone requis)
TEST_FLAGS = -g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=undefined -DRNDIS_TESTING

# Couverture de code (LLVM source-based coverage)
COV_FLAGS = -g -O0 -fprofile-instr-generate -fcoverage-mapping -DRNDIS_TESTING -DCOVERAGE_BUILD
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

tests/test_main: $(TEST_SRC) src/main.c $(HDR) tests/check.h tests/test_supervisor.h tests/test_session.h
	$(CC) $(CFLAGS) $(TEST_FLAGS) -o $@ $(TEST_SRC) $(LDLIBS)

test: tests/test_main
	./tests/test_main

tests/test_cov: $(TEST_SRC) src/main.c $(HDR) tests/check.h tests/test_supervisor.h tests/test_session.h
	$(CC) $(CFLAGS) $(COV_FLAGS) -o $@ $(TEST_SRC) $(LDLIBS)

# Rapport de couverture ; scripts/check-coverage.sh vérifie les seuils des modules purs
coverage: tests/test_cov
	rm -f tests/cov*.profraw
	LLVM_PROFILE_FILE=tests/cov-%p.profraw ./tests/test_cov
	$(LLVM_PROFDATA) merge -sparse tests/cov-*.profraw -o tests/cov.profdata
	$(LLVM_COV) report ./tests/test_cov -instr-profile=tests/cov.profdata $(SRC)

clean:
	rm -f rndis-tether tests/test_main tests/test_tsan tests/test_cov tests/cov*.profraw tests/cov.profdata
	rm -rf tests/test_main.dSYM tests/test_tsan.dSYM tests/test_cov.dSYM

# Détection de courses de données entre threads (ThreadSanitizer) : mêmes tests, y compris la session complète.
# die_after_fork=0 : les workers de test sont des fork() d'un processus qui a déjà des threads.
tests/test_tsan: $(TEST_SRC) src/main.c $(HDR) tests/check.h tests/test_supervisor.h tests/test_session.h
	$(CC) $(CFLAGS) -g -O1 -fno-omit-frame-pointer -fsanitize=thread -DRNDIS_TESTING -o $@ $(TEST_SRC) $(LDLIBS)

tsan: tests/test_tsan
	TSAN_OPTIONS="die_after_fork=0 halt_on_error=0 exitcode=66" ./tests/test_tsan

.PHONY: strict analyze test tsan coverage clean
