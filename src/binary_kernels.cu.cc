#if GOOGLE_CUDA

#define EIGEN_USE_GPU
#define EIGEN_USE_THREADS

#define BLOCK_SIZE 16

#include <stdio.h>
#include "xnor_gemm_kernel.h"
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"

using namespace tensorflow;

#define EIGEN_USE_GPU

template <typename T>
// CUDA tutorial: http://www.nvidia.com/docs/IO/116711/sc11-cuda-c-basics.pdf
// http://docs.nvidia.com/cuda/cuda-c-programming-guide/index.html#shared-memory
// A is shape (m,n), B is shape (n,k) and C is shape (m,k)
__global__ void gemm(float* A, float* B, float* C, int m, int n, int k) {

    // Block row and column
    int blockRow = blockIdx.y;
    int blockCol = blockIdx.x;
    
    // Thread row and column within Csub
    int row = threadIdx.y;
    int col = threadIdx.x;

    // Each thread block computes one sub-matrix Csub of C
    float* Csub = &C[BLOCK_SIZE * k * blockRow + BLOCK_SIZE * blockCol];

    // Shared memory used to store Asub and Bsub respectively
    __shared__ float As[BLOCK_SIZE][BLOCK_SIZE];
    __shared__ float Bs[BLOCK_SIZE][BLOCK_SIZE];
    
    // Each thread computes one element of Csub
    // by accumulating results into Cvalue
    // block_size = 16 -> 256 threads, one per Csub element
    float Cvalue = 0.0;
    
    // Loop over all the sub-matrices of A and B that are
    // required to compute Csub
    // Multiply each pair of sub-matrices together
    // and accumulate the results
    for (int i = 0; i < (n / BLOCK_SIZE); ++i) {
    
        // Get sub-matrix Asub of A
        float* Asub = &A[BLOCK_SIZE * blockRow * n + BLOCK_SIZE * i];
        
        // Get sub-matrix Bsub of B
        float* Bsub = &B[BLOCK_SIZE * k * i + BLOCK_SIZE * blockCol];
        
        // Load Asub and Bsub from device memory to shared memory
        // Each thread loads one element of each sub-matrix
        As[row][col] = Asub[row*n+col];
        Bs[row][col] = Bsub[row*k+col];
    
        // Synchronize to make sure the sub-matrices are loaded
        // before starting the computation
        __syncthreads();
        
        // Multiply Asub and Bsub together
        for (int j = 0; j < BLOCK_SIZE; ++j) Cvalue += As[row][j] * Bs[j][col]; 
        
        // Synchronize to make sure that the preceding
        // computation is done before loading two new
        // sub-matrices of A and B in the next iteration
        __syncthreads();
    }
    
    // Write Csub to device memory
    // Each thread writes one element
    if(col + blockCol* BLOCK_SIZE< k && row + blockRow* BLOCK_SIZE< m) Csub[row*k+col] = Cvalue;
}

// 32 single float array ->  32 bits unsigned int
__device__ unsigned int concatenate(float* array)
{
    unsigned int rvalue=0;
    unsigned int sign;
    
    for (int i = 0; i < 32; i++)
    {
        sign = (array[i]>=0);
        rvalue = rvalue | (sign<<i);
    }
    
    return rvalue;
}

__global__ void concatenate_rows_kernel(float *a, unsigned int *b, int size)
{ 
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if(i<size) b[i] = concatenate(&a[i*32]);
}

__global__ void concatenate_cols_kernel(float *a, unsigned int *b, int m, int n)
{   

    int j = blockIdx.x * blockDim.x + threadIdx.x;
    
    if(j<n){
        float * array = new float[32];
        for(int i=0; i<m; i+=32){
            for(int k=0; k<32;k++) array[k] = a[j + n*(i+k)];
            b[j+n*i/32]=concatenate(array); 
        } 
        delete[] array;
    }
}

// 32 bits unsigned int -> 32 single float array
// TODO: the array allocation should not be done here
__device__ float* deconcatenate(unsigned int x)
{
    float * array = new float[32];
    
    for (int i = 0; i < 32; i++)    
    {   
        array[i] = (x & ( 1 << i )) >> i;
    }
    
    return array;
}

__global__ void deconcatenate_rows_kernel(unsigned int *a, float *b, int size)
{ 
    float * array;
    
    for(int i=0; i<size; i+=32)
    {
        array = deconcatenate(a[i/32]);
        for (int k=0;k<32;k++) b[i+k] = array[k];
        delete[] array;
    }
}

// A is shape (m,n), B is shape (n,k) and C is shape (m,k)
__global__ void xnor_gemm(unsigned int* A, unsigned int* B, float* C, int m, int n, int k) {
    
    // Block row and column
    int blockRow = blockIdx.y;
    int blockCol = blockIdx.x;
    
    // Thread row and column within Csub
    int row = threadIdx.y;
    int col = threadIdx.x;

    // Each thread block computes one sub-matrix Csub of C
    float* Csub = &C[BLOCK_SIZE * k * blockRow + BLOCK_SIZE * blockCol];

    // Shared memory used to store Asub and Bsub respectively
    __shared__ unsigned int As[BLOCK_SIZE][BLOCK_SIZE];
    __shared__ unsigned int Bs[BLOCK_SIZE][BLOCK_SIZE];
    
    // Each thread computes one element of Csub
    // by accumulating results into Cvalue
    // block_size = 16 -> 256 threads, one per Csub element
    unsigned int Cvalue = 0;
    
    // Loop over all the sub-matrices of A and B that are
    // required to compute Csub
    // Multiply each pair of sub-matrices together
    // and accumulate the results
    for (int i = 0; i < (n / BLOCK_SIZE); ++i) {
    
        // Get sub-matrix Asub of A
        unsigned int* Asub = &A[BLOCK_SIZE * blockRow * n + BLOCK_SIZE * i];
        
        // Get sub-matrix Bsub of B
        unsigned int* Bsub = &B[BLOCK_SIZE * k * i + BLOCK_SIZE * blockCol];
        
        // Load Asub and Bsub from device memory to shared memory
        // Each thread loads one element of each sub-matrix
        As[row][col] = Asub[row*n+col];
        Bs[row][col] = Bsub[row*k+col];
    
        // Synchronize to make sure the sub-matrices are loaded
        // before starting the computation
        __syncthreads();
        
        // Multiply Asub and Bsub together
        // THIS IS THE MOST INTERESTING PART
        for (int j = 0; j < BLOCK_SIZE; ++j) Cvalue += __popc(As[row][j]^Bs[j][col]);
        
        // Synchronize to make sure that the preceding
        // computation is done before loading two new
        // sub-matrices of A and B in the next iteration
        __syncthreads();
    }
    
    // Write Csub to device memory
    // Each thread writes one element
    if(col + blockCol* BLOCK_SIZE< k && row + blockRow* BLOCK_SIZE< m) Csub[row*k+col] = -(2*(float)Cvalue-32*n);
}

void XnorGemmKernelLauncher(unsigned int* A, unsigned int* B, float* C, int m, int n, int k, int grid, int block) {
    //xnor_gemm<<<gridDim, blockDim>>>(A, B, C, N, N / 32, N);
    //int block_size = 16;
    //int grid = (k / block_size + 1, m / block_size + 1);
    xnor_gemm<<<grid, block>>>(A, B, C, m, n, k);
}

void ConcatenateRowsKernelLauncher(float *a, unsigned int *b, int size, int grid, int block) {
    //int block = 64, grid = m * m / (block * 32)  + 1;
    //concatenate_rows_kernel<<<grid, block>>>(fA, Aconc, N * N / 32);
    concatenate_rows_kernel<<<grid, block>>>(a, b, size * size / 32);
}

void ConcatenateColsKernelLauncher(float *a, unsigned int *b, int m, int n, int grid, int block) {
    //grid = N / block + 1;
    //concatenate_cols_kernel<<<grid, block>>>(fB, Bconc, N, N);
    concatenate_cols_kernel<<<grid, block>>>(a, b, m, n);
}    

// Define the GPU implementation that launches the CUDA kernel.
template <typename T>
struct ExampleFunctor<GPUDevice, T> {
  void operator()(const GPUDevice& d, int size, const T* in, T* out) {
    // Launch the cuda kernel.
    //
    // See core/util/cuda_kernel_helper.h for example of computing
    // block count and thread_per_block count.
    int block_count = 1024;
    int thread_per_block = 20;
    ExampleCudaKernel<T>
        <<<block_count, thread_per_block, 0, d.stream()>>>(size, in, out);
  }
};

// Instantiate functors for the types of OpKernels registered.
typedef Eigen::GpuDevice GPUDevice;
template struct ExampleFunctor<GPUDevice, float>;
template struct ExampleFunctor<GPUDevice, int32>;

#endif  // GOOGLE_CUDA