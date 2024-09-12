smol: smol.c
	$(CC) smol.c -o smol -Wall -O0 -g -Wextra -pedantic -std=c99

debug: 
	valgrind --leak-check=yes ./smol
