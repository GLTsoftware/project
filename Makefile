# Makefile for GLT project and endProject commands

CC = gcc
CFLAGS = -Wall -I/usr/local/include
LDFLAGS = -L/usr/local/lib
LIBS = -lhiredis

TARGETS = project endProject

all: $(TARGETS)

project: project.c
	$(CC) $(CFLAGS) -o project project.c $(LDFLAGS) $(LIBS)

endProject: endProject.c
	$(CC) $(CFLAGS) -o endProject endProject.c $(LDFLAGS) $(LIBS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
