COMPILE = gcc

INCS=-I/usr/include/gdk-pixbuf-2.0 -I/usr/include/libmount -I/usr/include/blkid -I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include
LIBS=-lnotify -lgdk_pixbuf-2.0 -lgio-2.0 -lgobject-2.0 -lglib-2.0

all: clean jabra

jabra: 
	$(COMPILE) -g -Wall -pthread $(INCS) -Iinc -Llib -o jabra jabra.c $(LIBS) -ljabra 

clean:
	rm -f jabra $(OBJECTS)
