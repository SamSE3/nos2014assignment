all:	test sample

test:	test.c
	gcc -Wall -g -o test test.c

sample:	sample.c
	gcc -Wall -g -o sample sample.c

