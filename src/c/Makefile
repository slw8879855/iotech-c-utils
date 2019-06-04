CFLAGS = -pthread -pipe -D_GNU_SOURCE -I../../include
OUTPUTFILE = libiotech-iot.a
SOURCES = atomic.c container.c data.c json.c logger.c os.c scheduler.c thread.c threadpool.c

OBJS = $(SOURCES:.c=.o)

.PHONY: all
all: $(OUTPUTFILE)

$(OUTPUTFILE): $(OBJS)
	ar ru $@ $^
	ranlib $@

.PHONY: clean
clean:
	rm -f *.o $(OUTPUTFILE)
