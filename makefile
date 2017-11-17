all: a3chat

clean:
	rm -rf a3chat

a3chat: a3chat.c
	gcc -std=gnu99 -Wall a3chat.c -o a3chat

tar:
	tar cfv submit.tar makefile a3chat.c a3report.pdf