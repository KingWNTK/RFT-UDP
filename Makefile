CC	 	= g++
LD	 	= g++
CFLAGS	 	= -Wall -std=c++11 -g -pthread

LDFLAGS	 	= 
DEFS 	 	=

all: sender receiver

sender: sender.cc
	$(CC) $(DEFS) $(CFLAGS) $(LIB) -o sender sender.cc

receiver: receiver.cc
	$(CC) $(DEFS) $(CFLAGS) $(LIB) -o receiver receiver.cc

clean:
	rm -f *.o
	rm -f *~
	rm -f core.*.*
	rm -f sender
	rm -f receiver
	