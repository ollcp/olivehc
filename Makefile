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
	mkdir -p output/bin && \
    mkdir -p output/conf && \
    mkdir -p output/log && \
    mkdir -p output/data
    
	cp olivehc output/bin && \
    cp olivehc_hit.sh output/bin &&\
    cp olivehc_control output/bin &&\
    cp make_device.sh output/bin &&\
    cp check output/bin &&\
    cp supervise output/bin
    
	test -f output/conf/olivehc.conf || \
    cp olivehc.conf.sample output/conf/olivehc.conf
    cp olivehc_control output/conf

clean :
	make -C utils clean
	rm -f $(OBJS) depends olivehc output

depends : $(SOURCES)
	$(CC) -MM $(CFLAGS) *.c > depends

include depends
