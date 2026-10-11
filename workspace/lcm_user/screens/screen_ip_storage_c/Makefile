# Makefile for the C reimplementation of the px2-300d IP/storage status screen
# Build on the px2-300d (Debian 13):  make
# Run:                                sudo ./screen_ip_storage

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
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
