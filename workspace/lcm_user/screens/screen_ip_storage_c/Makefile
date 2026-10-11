# Makefile for the C reimplementation of the px2-300d IP/storage status screen
# Build on the px2-300d (Debian 13):  make
# Run:                                sudo ./screen_ip_storage

CC      ?= cc
# -D_GNU_SOURCE：让 -std=c11 严格模式下也能看到 struct ifreq/IFNAMSIZ/
# IFF_UP（net/if.h）与 popen/pclose（stdio.h）等 POSIX/GNU 声明。
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
TARGET  := screen_ip_storage
OBJS    := screen_ip_storage.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c lcm_uapi.h glyph.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(OBJS)

.PHONY: all clean
