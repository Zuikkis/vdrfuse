vdrfuse: vdrfuse.c
	gcc -Wall vdrfuse.c `pkg-config fuse3 --cflags --libs` -o vdrfuse

install: vdrfuse
	cp vdrfuse /usr/local/bin
