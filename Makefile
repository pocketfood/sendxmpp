PKG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections -s
LIBS := $(shell $(PKG) --cflags --libs libstrophe)
LIBSTROPHE_LIBDIR := $(shell $(PKG) --variable=libdir libstrophe)
LIBSTROPHE_RPATH := -Wl,-rpath,$(LIBSTROPHE_LIBDIR)

all: sendxmpp

sendxmpp: sendxmpp.c
	$(CC) sendxmpp.c $(CFLAGS) $(LDFLAGS) $(LIBSTROPHE_RPATH) $(LIBS) -o $@
	strip $@

clean:
	rm -f sendxmpp
