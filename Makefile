# de-tnfsd - POSIX only. There is no Windows build and no portability layer;
# the resolver is built directly on openat/fstatat/unlinkat with O_NOFOLLOW.

CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings
CFLAGS  += -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS ?=

BIN     = bin/de-tnfsd
SRC     = $(wildcard src/*.c)
OBJ     = $(SRC:.c=.o)
PREFIX ?= /usr/local

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# A debug build with the sanitizers on; the test suite is worth running
# against this at least once before a release.
debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE" \
	        LDFLAGS="-fsanitize=address,undefined"

check: $(BIN)
	python3 tests/test_confinement.py $(BIN)
	python3 tests/test_readonly.py $(BIN)
	python3 tests/test_dropbox.py $(BIN)

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/sbin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/sbin/de-tnfsd

clean:
	rm -f $(OBJ) $(BIN)

.PHONY: all debug check install clean
