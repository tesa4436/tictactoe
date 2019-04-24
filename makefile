all : server.run client.run
server.run : server.c
	gcc -Wall -Wextra server.c -pthread -o server.run
client.run : client.c
	gcc -Wall -Wextra client.c -o client.run
clean :
	rm server.run client.run

