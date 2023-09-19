CC = gcc
CFLAGS = -Wall -Wextra -O2
OBJECTS = Client.o Server.o bw_template.o db.o
all: server create_link

server: Client.o Server.o bw_template.o
	$(CC) $(CFLAGS) $(OBJECTS) main.c -o server -libverbs

Client.o: Client.h Client.c bw_template.o
	$(CC) $(CFLAGS) -c Client.c

Server.o: Server.h Server.c bw_template.o db.o
	$(CC) $(CFLAGS) -c Server.c

bw_template.o: bw_template.h bw_template.c
	$(CC) $(CFLAGS) -c bw_template.c

b.o: b.h db.c
	$(CC) $(CFLAGS) -c db.c

create_link:
	ln -s server client

clean:
	rm -f server client *.o
