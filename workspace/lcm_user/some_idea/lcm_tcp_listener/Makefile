CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
LDFLAGS ?= -pthread

TARGET  := lcm_tcp_listener
OBJS    := lcm_tcp_listener.o base64.o mincjson.o

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

lcm_tcp_listener.o: lcm_tcp_listener.c base64.h mincjson.h lcm_uapi.h
base64.o:           base64.c base64.h
mincjson.o:         mincjson.c mincjson.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
