UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	CC=gcc
endif
ifeq ($(UNAME_S),Darwin)
	CC=gcc-12
endif
MPICC?=mpicc

perf?=0
ifeq "$(perf)" "1"
	CPPFLAGS += -D_PERF_
endif

CFLAGS+=-Wall -lm -g -O2
CFLAGS_THREADS=$(CFLAGS) -fopenmp

all: diffusion2d_mpi_nb_io diffusion2d_mpi_square_nb_io diffusion2d_mpi_square_nb_io_row diffusion2d_mpi_square_nb_io_view

diffusion2d_mpi_nb_io: diffusion2d_mpi_nb_io.c
	$(MPICC) $(CFLAGS) -o $@ $<

diffusion2d_mpi_square_nb_io: diffusion2d_mpi_square_nb_io.c
	$(MPICC) $(CFLAGS) -o $@ $<

diffusion2d_mpi_square_nb_io_row: diffusion2d_mpi_square_nb_io_row.c
	$(MPICC) $(CFLAGS) -o $@ $<
	
diffusion2d_mpi_square_nb_io_view: diffusion2d_mpi_square_nb_io_view.c
	$(MPICC) $(CFLAGS) -o $@ $<
clean:
	rm -f diffusion2d_mpi_nb_io *.dat
	rm -f diffusion2d_mpi_square_nb_io
	rm -f diffusion2d_mpi_square_nb_io_row
	rm -f diffusion2d_mpi_square_nb_io_view
	rm -rf *.dSYM


# Execution examples
# mpirun -n 1 ./diffusion2d_mpi_nb 1 1 1024 1000 0.00000001
# mpirun -n 4 ./diffusion2d_mpi_nb 1 1 1024 1000 0.00000001
