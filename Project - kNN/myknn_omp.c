#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <time.h>
#include <omp.h>
#include "func.h"

#ifndef PROBDIM
#define PROBDIM 2
#endif

#define DEBUG 1

static double **xdata;
static double ydata[TRAINELEMS];

// #define MAX_NNB	256

double find_knn_value(double *p, int n, int knn)
{
	// int nn_x[MAX_NNB];
	// double nn_d[MAX_NNB];	
	int nn_x[knn];
	double nn_d[knn];

	compute_knn_brute_force(xdata, p, TRAINELEMS, PROBDIM, knn, nn_x, nn_d); // brute-force / linear search

	double xd[knn * PROBDIM];     // the knn neighboring points/vectors of size PROBDIM
	double fd[knn];	      	      // function values for the knn neighbors

	for (int i = 0; i < knn; i++)
		fd[i] = ydata[nn_x[i]];

	for (int i = 0; i < knn; i++) 
		for (int j = 0; j < PROBDIM; j++)
			xd[i * PROBDIM + j] = xdata[nn_x[i]][j];

	return predict_value(PROBDIM, knn, xd, fd, p, nn_d);
}

int main(int argc, char *argv[])
{
	if (argc != 3)
	{
		printf("usage: %s <trainfile> <queryfile>\n", argv[0]);
		exit(1);
	}

	omp_set_dynamic(0); // set OpenMP dynamic mode to false, i.e. use the explicitly defined number of threads
	omp_set_num_threads(omp_get_max_threads()); // run using the maximum supported number of threads

	char *trainfile = argv[1];
	char *queryfile = argv[2];

	double *mem = (double *)malloc(TRAINELEMS * (PROBDIM + 1) * sizeof(double));
	double *query_mem = (double *)malloc(QUERYELEMS * (PROBDIM + 1) * sizeof(double));	
	
	xdata = (double **)malloc(TRAINELEMS * sizeof(double *));
	
	load_binary_data(trainfile, mem, TRAINELEMS*(PROBDIM+1));

	for (int i = 0; i < TRAINELEMS; i++)
		xdata[i] = mem + i * (PROBDIM + 1); //&mem[i*PROBDIM];

	for (int i = 0; i < TRAINELEMS; i++)
	{
        #if defined(SURROGATES)
		ydata[i] = mem[i * (PROBDIM + 1) + PROBDIM];
        #else
		ydata[i] = 0;
        #endif
	}

	load_binary_data(queryfile, query_mem, QUERYELEMS * (PROBDIM + 1));

#if DEBUG
	FILE *fpout = fopen("output.knn.txt","w");
#endif
	double *y = malloc(QUERYELEMS * sizeof(double));

	for (int i = 0; i < QUERYELEMS; i++)
	{
	#if defined(SURROGATES)
                y[i] = query_mem[i * (PROBDIM + 1) + PROBDIM];
        #else
                y[i] = 0.0;
        #endif
	}

	double t0, t1, t_start, t_end, t_first = 0.0, t_sum = 0.0;
        double sse = 0.0;
        double err, err_sum = 0.0;

	size_t nthreads;

	t_start = gettime();
	#pragma omp parallel reduction(+ : sse, err_sum, t_sum) private(t0, t1, err) 
	{
		double yp;
		size_t tid = omp_get_thread_num();

		#pragma omp single
		nthreads = omp_get_num_threads();

		size_t start = tid * (QUERYELEMS / nthreads);
		size_t end = (tid + 1) * (QUERYELEMS / nthreads);
		if (tid == nthreads - 1)
			end = QUERYELEMS;

		for (int i = start; i < end; i++) 	/* requests */
		{
			t0 = gettime();
                	yp = find_knn_value(&query_mem[i * (PROBDIM + 1)], PROBDIM, NNBS);
                	t1 = gettime();

			t_sum += (t1 - t0);
			
			if (i == 0)
				t_first = (t1 - t0);

			sse += (y[i] - yp) * (y[i] - yp);

        	#if DEBUG
	                for (int k = 0; k < PROBDIM; k++)
        	                fprintf(fpout, "%.5f ", query_mem[i * (PROBDIM + 1) + k]);
        	#endif

                	err = 100.0 * fabs((yp - y[i]) / y[i]);

        	#if DEBUG
                	fprintf(fpout,"%.5f %.5f %.2f\n", y[i], yp, err);
        	#endif
                
			err_sum += err;
		}
	}
	t_end = gettime();

	double mse = sse / QUERYELEMS;
	double ymean = compute_mean(y, QUERYELEMS);
	double var = compute_var(y, QUERYELEMS, ymean);
	double r2 = 1 - (mse / var);

	printf("Results for %d query points\n", QUERYELEMS);
	printf("APE = %.2f %%\n", err_sum / QUERYELEMS);
	printf("MSE = %.6f\n", mse);
	printf("R2 = 1 - (MSE/Var) = %.6lf\n", r2);

	printf("Total time = %lf ms\n", t_end - t_start);
	printf("Time for 1st query = %lf ms\n", t_first);
	printf("Time for 2..N queries = %lf ms\n", t_end - t_start - t_first);
	printf("Average time/query = %lf ms\n", (t_sum - t_first) / (QUERYELEMS - 1));

	free(mem);
	free(xdata);
	free(query_mem);
	free(y);

	return 0;
}
