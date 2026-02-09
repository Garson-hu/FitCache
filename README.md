# FitCache

**FitCache** is a transparent, drop-in multi-tier caching framework designed to accelerate distributed Deep Learning (DL) workloads. By coordinating memory (DRAM/PMem) and NVMe storage as hierarchical caches atop Parallel File Systems (PFS), FitCache eliminates I/O bottlenecks and optimizes data loading performance. This work has been accepted to *Proceedings of the 40th IEEE International Parallel & Distributed Processing Symposium ([**IPDPS 2026**](https://www.ipdps.org/)).*


## Key Features

* **Transparent Drop-in**: Seamlessly integrates with existing DL training pipelines (e.g., PyTorch, TensorFlow) via `LD_PRELOAD` without any code modifications.
* **Multi-Tier Coordination**: Dynamically manages data across DRAM and NVMe tiers. It adapts to hardware diversity—if NVMe is unavailable, memory transparently acts as the primary caching tier.
* **Fastest-Responder Reads**: Intercepts I/O requests and issues concurrent fetches across all storage tiers, returning data from the fastest available source to minimize latency.
* **Decentralized Metadata**: Eliminates the need for a centralized metadata server, enabling high scalability and resilience in large-scale HPC clusters.
* **Performance Optimized**: Specifically engineered for HPC environments to ensure low-overhead communication between nodes.

## System Architecture

FitCache operates by intercepting standard POSIX I/O calls. When a training process requests a data sample, FitCache checks the local DRAM tier, then the NVMe tier, and finally the remote PFS. If the data is not in the fastest tiers, it is fetched and asynchronously promoted to the upper tiers for future access.

## Compile and Run
> **Note**: The following instructions are primarily optimized for the **Frontier** supercomputer at Oak Ridge National Laboratory (ORNL). However, the deployment process on other HPC clusters, such as **ARC**, follows a very similar workflow.

### Prerequisites
To build and run FitCache, ensure the following dependencies are installed:

#### Software
- [Mercury](https://mercury-hpc.github.io/) (for RPC communication)
- [Log4C](https://log4c.sourceforge.net/) (for debugging and logging)
- CMake (version >= 3.16)
- GCC (version >= 9.1.0)
- libfabric (if using distributed mode)

#### Hardware (Depends on your system)
- DRAM
- Persistent Memory (PMem)
- Node-local Storage

#### Performance Characteristics by Tier

|  Storage Tier  | Access Latency | Bandwidth | Capacity | Use Case |
|----------------|---------------|-----------|----------|----------|
| Memory| ~100 ns | >200 GB/s | Limited (~200GB) | Hot training data|
| NVMe| ~1-5 μs | >5 GB/s | Limited (~2TB) | Hot training data, checkpoints |
| PFS | ~50-200 μs | ~5 GB/s | Large (~PB) | Cold data, persistent storage |


### Compilation
#### on Frontier
1. Load required modules:
```
module load log4c
module load mercury
module load gcc/12.2.0  # optional; depends on the system
```
2. Clone FitCache source code:
```
git clone https://github.com/Garson-hu/FitCache.git
```
3. Go to build directory
```
cd FitCache
mkdir -p build && cd build
```
4. Run the build script
``` 
./clean_cmake.sh
./build.sh 
```

### Project Structure

```
├── build
│   ├── build.sh
│   └── clean_cmake.sh
├── CMakeLists.txt
├── README.md
├── scripts
│   ├── command_FitCache.sh
│   └── run_FitCache.sh
├── src
│   ├── CMakeLists.txt
│   ├── fitcache_cache_policy.cpp
│   ├── fitcache_cache_policy.h
│   ├── fitcache_client.cpp
│   ├── fitcache_comm_client.cpp
│   ├── fitcache_comm.cpp
│   ├── fitcache_comm.h
│   ├── fitcache_comm_server.cpp
│   ├── fitcache_data_mover.cpp
│   ├── fitcache_data_mover_internal.h
│   ├── fitcache_internal.h
│   ├── fitcache_logging.c
│   ├── fitcache_logging.h
│   ├── fitcache_multi_source_read.cpp
│   ├── fitcache_multi_source_read.h
│   ├── fitcache_real_functions.cpp
│   ├── fitcache_server.cpp
│   ├── fitcache_timer.h
│   └── wrappers.c
└── tests
    ├── basic_test.c
    ├── CMakeLists.txt
    ├── my_ldpreload.cpp
    └── test_open_close.c
```

### Run
1. Allocate Compute Resources
Before running FitCache, you must allocate compute nodes. If you are using a system managed by Slurm (such as Frontier or ARC), please refer to the [Frontier User Guide](https://docs.olcf.ornl.gov/systems/frontier_user_guide.html#running-jobs) on Running Jobs for detailed instructions on job submission and resource allocation.

A typical command to get an interactive session on Frontier is:
```
salloc -t 00:30:00 -p batch -N 1 -C nvme -A <your_project_id>
```

2. Import all the required environment variables:
```
export BBPATH=/YOUR_NODE_LOCAL_STORAGE_PATH/
export FitCache_LOG_LEVEL=800   # Bigger means more information, set to 500 will print nothing
export RDMAV_FORK_SAFE=1
export VERBS_LOG_LEVEL=4
export FitCache_SERVER_COUNT=YOUR_SERVER_COUNT # Single node: 1 or 2, Distributed: number of nodes
export FitCache_DATA_DIR=/YOUR_TRAINING_SET_PATH/

```

3. Launch the server and client (For simple test)
```
mpirun -N 1 ./fitcache_server $FitCache_SERVER_COUNT &
mpirun -N 1 ./scripts/command_FitCache.sh
```

### Running on Cluster (Slurm Example)

For large-scale deployments on clusters like **Frontier** or **ARC**, you can use Slurm batch scripts to automate the lifecycle of the FitCache server and your training job. 

We provide two template scripts to handle the environment setup and process synchronization.

#### 1. The Launcher Script (`run_FitCache.sh`)
This script allocates nodes, sets up the necessary environment (modules, library paths, and Python virtual environment), and orchestrates the server/client startup.

**Key operations in the script:**
- **Server Startup**: Uses `srun` to launch the `fitcache_server` in the background. It is configured to run multiple servers per node (e.g., 2 servers per node) to maximize throughput.
- **Synchronization**: It waits for the server to generate a PID file to ensure the caching service is ready before the training starts.
- **Client Execution**: Invokes your training command script (see below).
- **Shutdown**: Sends a `SIGTERM` to the server after the training is finished to ensure all cached data and logs are properly handled.

#### 2. The Command Script (`command_FitCache.sh`)
This is a lightweight wrapper that uses `LD_PRELOAD` to inject the FitCache client library into your training process.

```bash
#!/bin/bash
# Inject FitCache client into the application
LD_PRELOAD=/path/to/libfitcache_client.so python3 train.py -d
```

## Future work
- Work on CXL Memory (FamFS) to support Distributed LLM Training


## License

This work was funded in part by subcontract B664613 from LLNL, as well as NSF grants CISE-2217020 and CISE-2316201. This research also used resources of the Oak Ridge Leadership Computing Facility, located at the National Center for Computational Sciences at the Oak Ridge National Laboratory, which is supported by the Office of Science of the DOE under Contract DE-AC05-00OR22725. 

## Contact

Please contact Guangxing Hu (ghu4@ncsu.edu) for any errors or inquiries.
