#!/bin/sh

gcc -c cdecode.c
gcc -c cencode.c
gcc -c test_b64.c

gcc -o testb64 cdecode.o cencode.o test_b64.o

