CC              = gcc
CFLAGS          = -g -O -pipe -Wall

SOURCES         = $(wildcard *.c)
OBJS            = $(patsubst %.c,%.o,$(SOURCES))

TARGET : $(OBJS)

clean :
	rm -f $(OBJS) depends

depends : $(SOURCES)
	$(CC) -MM $(CFLAGS) *.c > depends

include depends
