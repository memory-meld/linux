CFLAGS_nimble.o += -static


all: nimble

# SPDX-License-Identifier: GPL-2.0
# Makefile for vm tools
#
include ../scripts/Makefile.include

TARGETS=nimble

LIB_DIR =
LIBS =

CFLAGS += -Wall -Wextra -static
LDFLAGS += $(LIBS)

all: $(TARGETS)

$(TARGETS): $(LIBS)

# $(LIBS):
# 	make -C $(LIB_DIR)

%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	$(RM) $(TARGETS)
	# make -C $(LIB_DIR) clean

# sbindir ?= /usr/sbin

# install: all
# 	install -d $(DESTDIR)$(sbindir)
# 	install -m 755 -p $(TARGETS) $(DESTDIR)$(sbindir)
