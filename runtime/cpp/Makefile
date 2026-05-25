# Uzbek-PY — Native runtime Makefile
# ==================================
#
# Targets:
#   make          - build the optimised binary -> ./uzbek
#   make debug    - build with -O0 -g (for gdb / valgrind)
#   make test     - run the example programs and verify output
#   make clean    - remove build artefacts
#   make install  - copy ./uzbek into $(PREFIX)/bin (default: /usr/local)
#
# Compiler is auto-detected from $CXX (defaults to g++). Override with
# any C++17 toolchain:    make CXX=clang++

CXX        ?= g++
CXXFLAGS   ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS    ?=
PREFIX     ?= /usr/local

SRC        := uzbek.cpp
BIN        := uzbek
EXAMPLES   := $(wildcard examples/*.uz)


.PHONY: all debug test clean install help
.DEFAULT_GOAL := all

all: $(BIN)

$(BIN): $(SRC)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $<

debug: CXXFLAGS := -std=c++17 -O0 -g -Wall -Wextra
debug: clean $(BIN)

test: $(BIN)
	@echo "==> Running example programs"
	@set -e; for f in $(EXAMPLES); do \
	    echo "---- $$f ----"; \
	    ./$(BIN) "$$f"; \
	done
	@echo "==> All examples completed."

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)

help:
	@grep '^# ' Makefile | head -20 | sed 's/^# //'
