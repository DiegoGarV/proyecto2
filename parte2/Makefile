CC      = mpicc
CFLAGS  = -O2 -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS = -lm

all: algoritmo_mpi

algoritmo_mpi: algoritmo_mpi.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

debug: CFLAGS += -g -O0
debug: clean all

clean:
	rm -f algoritmo_mpi