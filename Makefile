SRCS = $(wildcard *.c)
OBJS = $(SRCS:.c=.o)
HEADERS = $(wildcard *.h)
CFLAGS = -std=c99 -Wall

test: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean

all: test

clean:
	rm -f *.o test