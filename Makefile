CC	 	= g++
LD	 	= g++
CFLAGS	 	= -Wall -std=c++11 -g -pthread

LDFLAGS	 	= 
DEFS 	 	=

all: recvfile sendfile

sendfile: sendfile.cc
	$(CC) $(DEFS) $(CFLAGS) $(LIB) -o sendfile sendfile.cc

recvfile: recvfile.cc
	$(CC) $(DEFS) $(CFLAGS) $(LIB) -o recvfile recvfile.cc

clean:
	rm -f *.o
	rm -f *~
	rm -f core.*.*
	rm -f sendfile
	rm -f recvfile
	