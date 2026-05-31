CFLAGS = -g -Wall #-Wextra -Wshadow -Wunreachable-code -Wredundant-decls -Wmissing-declarations -Wold-style-definition -Wmissing-prototypes -Wdeclaration-after-statement -Wno-return-local-addr -Wunsafe-loop-optimizations -Wuninitialized -Werror -Wno-unused-parameter 

PROGS = eoraptor_server eoraptor_client

all: $(PROGS)

eoraptor_server: eoraptor_server.o
	gcc $(CFLAGS) -o eoraptor_server eoraptor_server.o

eoraptor_server.o: eoraptor_server.c eoraptor_ui.h
	gcc $(CFLAGS) -c -g -o eoraptor_server.o eoraptor_server.c

eoraptor_client: eoraptor_client.o eoraptor_ui.o
	gcc $(CFLAGS) -o eoraptor_client eoraptor_client.o eoraptor_ui.o -lncurses

eoraptor_client.o: eoraptor_client.c eoraptor_ui.h
	gcc $(CFLAGS) -c -o eoraptor_client.o eoraptor_client.c 

eoraptor_ui.o: eoraptor_ui.c eoraptor_ui.h
	gcc $(CFLAGS) -c -o eoraptor_ui.o eoraptor_ui.c

clean cls: 
	rm -f $(PROGS) *.o *~ \#* .*.swp

run:
	./eoraptor_server -v

