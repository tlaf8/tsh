.PHONY: all
all: engine.out

engine.out: engine.c parser.c parser.h
	gcc -Wall -g -o $@ $^

.PHONY: clean
clean: 
	rm -f *.out