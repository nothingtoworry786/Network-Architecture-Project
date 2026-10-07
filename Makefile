# HTTP, in binary - BH/1 server (observe) and client (bcurl).
#
#   make          build ./observe and ./bcurl
#   make test     build everything and run all tests
#   make debug    rebuild with -O0 -g3
#   make asan     rebuild and run all tests under AddressSanitizer
#   make ubsan    rebuild and run all tests under UndefinedBehaviorSanitizer
#   make clean

CC      = gcc
OPT     = -O2
# _XOPEN_SOURCE=700 asks for POSIX.1-2008 (getaddrinfo, sigaction, realpath...).
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -Werror -D_XOPEN_SOURCE=700 -Iinclude $(OPT)

LIB     = src/protocol.c src/net.c src/hexdump.c
HEADERS = include/protocol.h include/net.h include/hexdump.h
TESTS   = tests/test_protocol tests/test_conn

all: observe bcurl

observe: src/observe.c $(LIB) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/observe.c $(LIB)

bcurl: src/bcurl.c $(LIB) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ src/bcurl.c $(LIB)

tests/%: tests/%.c tests/testutil.h $(LIB) $(HEADERS)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(LIB)

test: all $(TESTS)
	./tests/test_protocol
	sh tests/test_server.sh
	sh tests/test_client.sh
	@echo "ALL TESTS PASSED"

debug:
	$(MAKE) clean
	$(MAKE) all OPT="-O0 -g3"

asan:
	$(MAKE) clean
	$(MAKE) test OPT="-O1 -g -fno-omit-frame-pointer -fsanitize=address"

ubsan:
	$(MAKE) clean
	$(MAKE) test OPT="-O1 -g -fsanitize=undefined -fno-sanitize-recover=all"

clean:
	rm -f observe bcurl $(TESTS)

.PHONY: all test debug asan ubsan clean
