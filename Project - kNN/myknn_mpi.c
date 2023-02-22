#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include "func_mpi.h"
#include <stddef.h>

#ifndef PROBDIM
#define PROBDIM 2
#endif

// TODO : maybe allocate these inside main and pass them as args to find_knn_value (?)
static double **xdata;
static double *ydata;

// double find_knn_value(double *p, int n, int knn)
double find_knn_value(query_t *q, int knn)
{
#if defined(SIMD)
	__attribute__((aligned(32))) double fd[knn];	// function values for the knn neighbors
#else 
	double fd[knn];
#endif	

	for (int i = 0; i < knn; i++)
		fd[i] = q->nn_val[i];

	return predict_value(fd, knn);
}

int main(int argc, char *argv[])
{
	/* Load all data from files in memory */
	if (argc != 3)
	{
		printf("usage: %s <trainfile> <queryfile>\n", argv[0]);
		exit(1);
	}
	char *trainfile = argv[1];
	char *queryfile = argv[2];


        // MPI Init
        int rank, nprocs;
        MPI_Init(&argc, &argv);
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
        MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

#if defined(SIMD)
        if (rank == 0)
	        printf("Running with SIMD\n");
#endif 
        
        int vector_size = PROBDIM + 1;
        int local_ntrainelems = TRAINELEMS / nprocs;
        int trainelem_offset = rank * local_ntrainelems * vector_size;
        
        // correction for the last process : add the remaining training elements
        if(rank == nprocs - 1)
                local_ntrainelems += TRAINELEMS % nprocs;

	int trainelems_chunk = local_ntrainelems * vector_size; // chunk size in scalar elements (i.e. doubles)

	double *mem = (double *)malloc(trainelems_chunk * sizeof(double));
	ydata = (double *)malloc(local_ntrainelems * sizeof(double));
	double *query_ydata = malloc(QUERYELEMS * sizeof(double));
	double *query_mem = (double *)malloc(QUERYELEMS * vector_size * sizeof(double));
	query_t *queries = (query_t *)malloc(QUERYELEMS * sizeof(query_t));

#if defined(SIMD)
	int posix_res;
        // Malloc aligned space for query.x data 
        for (int i = 0; i < QUERYELEMS; i++)
        {
                posix_res = posix_memalign((void **)(&(queries[i].x)), 32, PROBDIM * sizeof(double));
                assert(posix_res == 0);
        }
#endif

	/* Create a handler array that will be used to separate xdata's PROBDIM vectors
	 * and the corresponding surrogate values, since we never need both
	 * in order to perform a computation.
	 * We either going to use the xdata of two points (ex. when calculating distance from one another)
	 * or use ydata (surrogates) (ex. when predicting the value of a query point)
	 */
	xdata = (double **)malloc(local_ntrainelems * sizeof(double*));
        
        // read a part of training data
        load_binary_data_mpi(trainfile, mem, NULL, trainelems_chunk, trainelem_offset);

        // read all of the query data
        load_binary_data_mpi(queryfile, query_mem, queries, QUERYELEMS * vector_size, 0);        

#if defined(SIMD)
	// Allocate new memory for the handler arrays, so that it is aligned and copy the data there
	// Align each xdata[i] to a 32 byte boundary so you may later use SIMD
	for (int i = 0; i < local_ntrainelems; i++)
	{
		posix_res = posix_memalign((void **)(&(xdata[i])), 32, PROBDIM * sizeof(double));
		assert(posix_res == 0);
	}
	copy_to_aligned(mem, xdata, vector_size, PROBDIM, local_ntrainelems);
#else
	// Assign to the handler array, pointers to the already allocated mem
	for (int i = 0; i < local_ntrainelems; i++)
		xdata[i] = &mem[i * vector_size];
#endif

	/* Configure and Initialize the ydata handler arrays */
	for (int i = 0; i < local_ntrainelems; i++)
	{
#if defined(SURROGATES)
		ydata[i] = mem[i * vector_size + PROBDIM];
#else
		ydata[i] = 0;
#endif
	}

	for (int i = 0; i < QUERYELEMS; i++)
	{
#if defined(SURROGATES)
		query_ydata[i] = query_mem[i * vector_size + PROBDIM];
#else
		query_ydata[i] = 0;
#endif
	}

	/* DECIDE ON SIZE OF IN-RANK BLOCK */
	// TODO: Think about last rank, which may have more local training elements, the assert may not succeed
	//       you probably only need to correct the last iteration (?)
	// int L1d_size, train_block_size = 1;
	// get_L1d_size(&L1d_size); // get L1d cache size
	// // calculate the appropriate train block size as the previous power of 2
	// if(L1d_size > 0)
	// 	train_block_size = pow(2, floor(log2((L1d_size * 1000) / (PROBDIM * sizeof(double)))));
	// assert(local_ntrainelems % train_block_size == 0);

	/* COMPUTATION PART */
	double t0, t1, t2 = 0.0, t_first = 0.0, t_sum = 0.0;
	double sse = 0.0;
	double err_sum = 0.0;

	int queryelems_blocksize = QUERYELEMS / nprocs;
	int rank_in_charge;
	
	// we actually need an MPI_Request object per asynchronous communication call
	MPI_Request request[nprocs - 1];
	MPI_Status status;

	int first_query = rank * queryelems_blocksize;
	int last_query = (rank + 1) * queryelems_blocksize - 1;

	if (rank == nprocs - 1)
		last_query = QUERYELEMS - 1;
	
        /* Configure the Derived Datatype for the query_t struct */
        // https://mpi.deino.net/mpi_functions/MPI_Type_create_struct.html
        // https://stackoverflow.com/questions/33618937/trouble-understanding-mpi-type-create-struct

	/* Each query_t object contains: 
	 * - a pointer to a double vector of size PROBDIM (we don't need to send this vector, 
	 *   so don't account its size)
	 * - an integer array of size NNBS -> size_int_arr
	 * - two double arrays of size NNBS -> 2 * size_double_arr .
	 * Therefore, the total buffer size is equal to :
	 */
	MPI_Datatype mpi_query_t;                                 // The name of the Derived Datatype
        MPI_Datatype type[3] = {MPI_INT, MPI_DOUBLE, MPI_DOUBLE}; // The MPI_Datatype of each struct member
        int blocklen[3] = {NNBS, NNBS, NNBS};                     // The size of each array (use 1 if scalar)
        MPI_Aint disp[3];                                         // MPI Array of displacements

        disp[0] = offsetof(query_t, nn_idx);
        disp[1] = offsetof(query_t, nn_dist);
        disp[2] = offsetof(query_t, nn_val);

        MPI_Type_create_struct(3, blocklen, disp, type, &mpi_query_t);
        MPI_Type_commit(&mpi_query_t);

	// Need enough space to store all query_t structs sent by every other rank.
	query_t *rcv_buf = (query_t *)malloc((nprocs - 1) * sizeof(query_t));

	int global_block_offset = rank * local_ntrainelems;
	/* Each rank is responsible for calculating the k neighbors of each query point,
	 * using only the training elements block it has been assigned. The block's boundaries are defined as:
	 * start = rank * local_ntrainelems * vector_size (i.e. global_train_offset) // NOT entirely correct...
	 * end = (rank + 1) * local_ntrainelems * vector_size .
	 * The calculation of each query point's neighbors, occurs inside compute_knn_brute_force.
	 *
	 * Within each block, each rank is responsible for :
	 * a) Calculating the k neighbors of **all** query points
	 * b) Sending each query that it is not responsible for, to the correct rank.
	 * c) Gathering collections of k neighbors for the subset of query points defined by [query_chunk_start, query_chunk_end].
	 *    These collections are calculated (step a) and sent (step b) by the other ranks (i.e. from other training elements blocks).
	 * d) Calculating the final k nearest neighbors, using the collections gathered at step (c). (reduction)
	 */
	t0 = gettime();

	// (a) and (b) Calculate and send the k neighbors found in the training block for **all** query points.

	// TODO: one more loop here for the case of "cache blocking"
	for (int i = 0; i < QUERYELEMS; i++)
	{
                if (i == first_query)
                        t2 = gettime();

		compute_knn_brute_force(xdata, ydata, &(queries[i]), PROBDIM, NNBS, global_block_offset, 0, local_ntrainelems);

		rank_in_charge = get_rank_in_charge_of(i, queryelems_blocksize, nprocs);
		if (rank_in_charge != rank)
			MPI_Isend(&(queries[i]), 1, mpi_query_t, rank_in_charge, i, MPI_COMM_WORLD, &request[rank_in_charge]);

                if (i == first_query)
                        t_first += gettime() - t2;
	}

	int rcv_buf_offset = 0;
        t2 = gettime();
	for (int i = first_query; i <= last_query; i++)
	{
		/* (c) Gather the collections of k nearest neighbors sent by the other ranks.
                 * TODO(?): Make the Recvs async, once you receive a collection (for any query you are in charge of)
                 *          reduce_in_struct immediately.
                 * Discussion about callback function on an async request: 
                 * https://stackoverflow.com/questions/49763675/is-it-possible-to-attach-a-callback-to-be-executed-on-a-request-completion
                 *
                 * NOTE: Is all this "query blocking" even worth it in order to reduce communication delays?
                 * I think it is since you are not sending all training data to all ranks, only a block of them. Thus you need at some point to reduce the knns
                 * and certainly you want to avoid collecting all data at the root rank and performing the reduction there in a serial fashion.
		 */ 
		rcv_buf_offset = 0;
		for (int j = 0; j < nprocs; j++)
		{
			if (j == rank)
				continue;

                        assert(rcv_buf_offset < nprocs);
			MPI_Recv(&(rcv_buf[rcv_buf_offset++]), 1, mpi_query_t, j, i, MPI_COMM_WORLD, &status);
		}
		// (d) Update the k neighbors of each query points under our control using the data we received from the other ranks.
		reduce_in_struct(&(queries[i]), rcv_buf, nprocs - 1);

                if (i == first_query)
                        t_first += gettime() - t2;
	}
	t1 = gettime();
	t_sum = t1 - t0;
        
        // Initialize environment for metric calculations
#if defined(DEBUG)
        // Preserve yp, err for each query, so you may later write the output file
        char *filename = "output.knn_mpi.txt";

        // Open and initialize the ouput file for all ranks
        MPI_File f;
        MPI_Offset base;
        MPI_File_open(MPI_COMM_WORLD, filename, MPI_MODE_WRONLY | MPI_MODE_CREATE, MPI_INFO_NULL, &f);
        MPI_File_get_position(f, &base);

	double yp[last_query - first_query + 1], err[last_query - first_query + 1];
	int local_idx = 0;
#else
        /* We only care about current err and yp. err will be accumulated in err_sum, thus we do not need a helper variable.
         * On the other hand yp will be used to calculate the err and sse and thus in next iteration may be overwritten.
	 */
	double yp;
#endif
        /* Calculate yp and the errors/metrics for all queries under the rank's responsibility
         * Should preserve the values in DEBUG mode.
	 */
	for (int i = first_query; i <= last_query; i++)
	{
		t0 = gettime();
#if defined(DEBUG)
		yp[local_idx] = find_knn_value(&(queries[i]), NNBS);
#else
		yp = find_knn_value(&(queries[i]), NNBS);
#endif
		t1 = gettime();
		t_sum += t1 - t0;
		if(i == first_query)
			t_first += t1 - t0;

#if defined(DEBUG)
		sse += (query_ydata[i] - yp[local_idx]) * (query_ydata[i] - yp[local_idx]);
		err[local_idx] = 100.0 * fabs((yp[local_idx] - query_ydata[i]) / query_ydata[i]);
		err_sum += err[local_idx++];		
#else
		sse += (query_ydata[i] - yp) * (query_ydata[i] - yp);
		err_sum += 100.0 * fabs((yp - query_ydata[i]) / query_ydata[i]);
#endif
	}

#if defined(DEBUG)
        // Write the output file
	int curr_char_offset = 0, global_char_offset = 0;

        const int max_len = 30; // we assume an upped bound on the char length of each double
	/* buf is a temp char buffer used to gather the triplets of values that must appear 
	 * in one line of the text output file.
	 */
	char buf[max_len * (last_query - first_query + 1)];

	// Collect all local data you want to write to the file into a char buffer
        for (int i = first_query; i <= last_query; i++)
        {
                local_idx = i - first_query; // "convert" global query index to buf-bound local index
		/* We use snprintf to store the string representation of 3 double values.
		 * However, when snprintf writes the "%.5f %.5f %.2f\n" string, after the newline character,
		 * it places the NULL string terminating character, '\0'.
		 * However, since the output file is a text file, we don't want that character to be visible.
		 * 
		 * Therefore, we use strlen(buf) which calculates the actual length of the string contents of the
		 * buf buffer. The key is that strlen(buf) does not take into account the '\0' character, so the
		 * value it returns, is actually one less that the characters written by the snprintf call.
		 * 
		 * We store the strlen(buf) returned value into curr_char_offset, which we use as an offset from
		 * the start of the buf char array, and since it "points" to the exact index where the last '\0'
		 * was placed, we overwrite that character with the new string placed onto the buffer by snprintf.
		 */
                snprintf(buf + curr_char_offset, max_len, "%.5f %.5f %.2f\n", query_ydata[i], yp[local_idx], err[local_idx]);
                curr_char_offset = strlen(buf);
        }
        /* At this point each rank has determined the amount of bytes it needs to write in the output file (last value of curr_char_offset)
         * Each rank is in charge of a set of queries in a blocking fashion 
         * (i.e. rank 0 is in charge of queries [0, queryelems_blocksize),
	 *       rank 1 -> [queryelems_blocksize, 2*queryelems_blocksize - 1), ...).
         * We use Exscan in order to calculate each rank's **global** offset (in number of chars), in the shared file.
	 */
        MPI_Exscan(&curr_char_offset, &global_char_offset, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        
        // Collectively write data to the output text file, using the rank-specific offsets 
        MPI_File_write_at_all(f, base + global_char_offset, buf, strlen(buf), MPI_CHAR, MPI_STATUS_IGNORE);
#endif
        
	/* CALCULATE AND DISPLAY RESULTS */

	// Reduce all metrics to the root rank (i.e. rank 0)
        MPI_Reduce(rank == 0 ? MPI_IN_PLACE : &t_sum, &t_sum, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD); // set max time as total time
	MPI_Reduce(rank == 0 ? MPI_IN_PLACE : &t_first, &t_first, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD); // sum time for first queries
	MPI_Reduce(rank == 0 ? MPI_IN_PLACE : &err_sum, &err_sum, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
	MPI_Reduce(rank == 0 ? MPI_IN_PLACE : &sse, &sse, 1, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);

        if(rank == 0) // only rank 0 prints
        {
                double mse = sse / QUERYELEMS;
                double ymean = compute_mean(query_ydata, QUERYELEMS);
                double var = compute_var(query_ydata, QUERYELEMS, ymean);
                double r2 = 1 - (mse / var);

                printf("Results for %d query points\n", QUERYELEMS);
                printf("APE = %.2f %%\n", err_sum / QUERYELEMS);
                printf("MSE = %.6f\n", mse);
                printf("R2 = 1 - (MSE/Var) = %.6lf\n", r2);

                printf("Total Computing time = %lf secs\n", t_sum);                
		printf("Average time for 1st query = %lf secs\n", t_first / nprocs);
		printf("Time for 2..N queries = %lf secs\n", t_sum - t_first);
		printf("Average time/query = %lf secs\n", t_sum / QUERYELEMS);
        }

	/* CLEANUP */
        // MPI_Reduce does not guarantee that a non-root process will not reach here
        // while the root process is still Receiving the messages.
        // Thus I need to wait for all messages to be received. A better way would probably be to wait on the requests
        // https://stackoverflow.com/questions/8596774/is-open-mpis-reduce-syncrhonized
        MPI_Barrier(MPI_COMM_WORLD);
#if defined(DEBUG)
	MPI_File_close(&f);
#endif

#if defined(SIMD)
	for (int i = 0; i < QUERYELEMS; i++)
		free(queries[i].x);
#endif
	free(queries);
	free(query_ydata);
	free(query_mem);

#if defined(SIMD)
	for (int i = 0; i < local_ntrainelems; i++)
		free(xdata[i]);
#endif
	free(xdata);
	free(ydata);
	free(mem);

	free(rcv_buf);

	MPI_Finalize();
	return 0;
}
