CFLAGS=-Wall
LDFLAGS=-lrt

all: network_demo

clean:
	rm -f network_demo

network_demo: main.c
	gcc -o network_demo main.c $(CFLAGS) $(LDFLAGS)
