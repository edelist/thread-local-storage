CC=gcc
CFLAGS=-Werror -Wall -c
LDFLAGS=-lpthread

main: tls.o main.o
        $(CC) -o main tls.o main.o $(LDFLAGS)

tls.o: tls.c
        $(CC) $(CFLAGS) -o tls.o tls.c

main.o: main.c
        $(CC) $(CFLAGS) -o main.o main.c

clean:
        rm -f tls.o main.o main