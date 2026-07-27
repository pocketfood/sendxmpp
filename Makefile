PKG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections -s
LIBS := $(shell $(PKG) --cflags --libs libstrophe)

all: sendxmpp

sendxmpp: sendxmpp.c
	$(CC) sendxmpp.c $(CFLAGS) $(LDFLAGS) $(LIBS) -o $@
	strip $@

clean:
	rm -f sendxmpp
