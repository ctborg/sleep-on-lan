CC ?= cc
CFLAGS ?= -std=c99 -Wall -Wextra -pedantic -O2
CPPFLAGS ?=
LDFLAGS ?=
LDLIBS ?=

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(OS),Windows_NT)
  EXEEXT := .exe
  LDLIBS += -lws2_32 -liphlpapi
else
ifeq ($(UNAME_S),Linux)
  LDLIBS += -pthread
endif
ifeq ($(UNAME_S),Darwin)
  LDLIBS += -pthread
endif
endif

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

SRC := src/sol.c
BIN := sol$(EXEEXT)

.PHONY: all clean install check

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(LDLIBS)

check: $(BIN)
	./$(BIN) --version
	./$(BIN) generate-configuration >/tmp/sol-generated.json

install: $(BIN)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(BIN) "$(DESTDIR)$(BINDIR)/$(BIN)"

clean:
	rm -f sol sol.exe
