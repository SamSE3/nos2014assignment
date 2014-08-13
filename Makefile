all:	test sample

LOPT=`uname | grep SunOS | sed 's/SunOS/-lnsl -lsocket/'`

test:	test.c Makefile
	gcc -Wall -g -o test test.c $(LOPT)

sample:	sample.c Makefile
	gcc -Wall -g -o sample sample.c $(LOPT)

