#include "mpi.h"
#include "func.h"

void load_binary_data_mpi(const char *filename, double *data, query_t *queries, const int N, int offset)
{
	int rank, nprocs;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

	// Open the file (collective call)
	MPI_File f;
        MPI_File_open(MPI_COMM_WORLD, filename, MPI_MODE_RDONLY, MPI_INFO_NULL, &f);

        // Calculate the offset for each rank
        MPI_Offset base;
        MPI_File_get_position(f, &base);

        MPI_Offset data_offset = offset * sizeof(double);

	// Collectively Read the data
        MPI_Status status;
        MPI_File_read_at_all(f, base + data_offset, data, N, MPI_DOUBLE, &status); // blocking collective call

        // If queries are loaded, initialize the queries structs
	if (queries != NULL)
	{
		size_t posix_res;
		for (int i = 0; i < QUERYELEMS; i++)
		{
			posix_res = posix_memalign((void **)(&(queries[i].x)), 32, PROBDIM * sizeof(double));
			assert(posix_res == 0);

			for (int k = 0; k < PROBDIM; k++)
				queries[i].x[k] = data[i * (PROBDIM + 1) + k];

			for (int j = 0; j < NNBS; j++)
				queries[i].nn_idx[j] = -1;

			for (int j = 0; j < NNBS; j++)
				queries[i].nn_d[j] = 1e99 - j;
		}
	}

        // Close the file
        MPI_File_close(&f);        
}

// Store mpi
// mode == 0 -> write trainelements
// mode == 1 -> write queryelements
void store_binary_data_mpi(const char *filename, double *data, const int N, int offset, int mode)
{
        int rank, nprocs;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	MPI_Comm_size(MPI_COMM_WORLD, &nprocs);

	// Open the file (collective call)
	MPI_File f;
        MPI_File_open(MPI_COMM_WORLD, filename, MPI_MODE_WRONLY | MPI_MODE_CREATE, MPI_INFO_NULL, &f);
        MPI_File_preallocate(f, (mode == 0 ? TRAINELEMS : QUERYELEMS) * sizeof(double));

        // Calculate the offset for each rank
        MPI_Offset base;
        MPI_File_get_position(f, &base);

        MPI_Offset data_offset = offset * sizeof(double);

	// Collectively Read the data
        MPI_Status status;
        MPI_File_write_at_all(f, base + data_offset, data, N, MPI_DOUBLE, &status); // blocking collective call
        
        MPI_File_close(&f); 
}

void compute_knn_brute_force_mpi(double **xdata, query_t *q, int dim, int k, int train_data_offset, int num_train_data)
{
	int i, gi, max_i;
	double max_d, new_d;

	// find K neighbors
	max_d = compute_max_pos(q->nn_d, k, &max_i);
	for (i = 0; i < num_train_data; i++) // i runs inside each training block's boundaries
	{
		gi = i + train_data_offset;
		new_d = compute_dist(q->x, xdata[i], dim); // euclidean		
		if (new_d < max_d) // add point to the list of knns, replace element max_i
		{	
			q->nn_idx[max_i] = gi;
			q->nn_d[max_i] = new_d;
		}
		max_d = compute_max_pos(q->nn_d, k, &max_i);
	}

	/* sort the knn list */ // bubble sort
	// int temp_x, j;
	// double temp_d;
	// for (i = (knn - 1); i > 0; i--)
	// {
	// 	for (j = 1; j <= i; j++)
	// 	{
	// 		if (nn_d[j-1] > nn_d[j])
	// 		{
	// 			temp_d = nn_d[j-1];
	// 			nn_d[j-1] = nn_d[j];
	// 			nn_d[j] = temp_d;
				
	// 			temp_x = nn_x[j-1];
	// 			nn_x[j-1] = nn_x[j];
	// 			nn_x[j] = temp_x;
	// 		}
	// 	}
	// }
}

// TODO: Check if inline is useful or even helpfull
// https://stackoverflow.com/questions/1932311/when-to-use-the-inline-function-and-when-not-to-use-it
inline int get_rank_in_charge_of(int query_idx, int query_blocksize, int mpi_comm_size)
{
	int rank = query_idx / query_blocksize;
	int max_rank = mpi_comm_size - 1;
	return rank > max_rank ? max_rank : rank;
}

int get_max_dist_neighbor(double *nn_d, int k)
{
	int max_idx = 0;
	for (int i = 1; i < k; i++)
	{
		if (nn_d[max_idx] < nn_d[i])
			max_idx = i;
	}

	return max_idx;
}

void reduce_in_struct(query_t *query, query_t *received, int received_size)
{
	int max_idx;

	for (int i = 0; i < received_size; i++)
	{
		// TODO: Find a better way to calculate the k nearest neighbors
		max_idx = get_max_dist_neighbor(query->nn_d, NNBS);
		for (int j = 0; j < NNBS; j++)
		{
			if (query->nn_d[max_idx] > received[i].nn_d[j])
			{
				query->nn_d[max_idx] = received[i].nn_d[j];
				query->nn_idx[max_idx] = received[i].nn_idx[j];
				max_idx = get_max_dist_neighbor(query->nn_d, NNBS);
			}
		}
	}
}