HEADERS = $(wildcard *.h)
CFLAGS = -std=c99 -Wall

#everything except the two files that define main()
LIBSRCS = closure.c functional.c gc.c list.c
LIBOBJS = $(LIBSRCS:.c=.o)

all: test gctest

#the demo program
test: $(LIBOBJS) main.o
	$(CC) $(LDFLAGS) $(LIBOBJS) main.o -o $@

#the gc test suite (gctest.c has its own main())
gctest: $(LIBOBJS) gctest.o
	$(CC) $(LDFLAGS) $(LIBOBJS) gctest.o -o $@

%.o: %.c $(HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: all clean check

check: gctest
	./gctest

clean:
	rm -f *.o test gctest
