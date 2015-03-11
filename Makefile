CC              = gcc
CFLAGS          = -g -O -pipe -Wall
LINK		= gcc
LDFLAGS		= -lpthread -lssl

SOURCES         = $(wildcard *.c)
OBJS            = $(patsubst %.c,%.o,$(SOURCES))

TARGET          = olivehc

olivehc : $(OBJS)
	make -C utils
	$(LINK) $(LDFLAGS) -o $@ $^ utils/*.o
	# install:
	mkdir -p output
	cp olivehc output/
	test -f output/olivehc.conf || cp olivehc.conf.sample output/olivehc.conf

clean :
	make -C utils clean
	rm -f $(OBJS) depends olivehc

depends : $(SOURCES)
	$(CC) -MM $(CFLAGS) *.c > depends

include depends
