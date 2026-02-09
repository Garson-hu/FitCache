#!/bin/bash
#SBATCH -A gen008
##SBATCH -q debug
##SBATCH -p extended
#SBATCH --nodes=1
#SBATCH -t 00:30:00
#SBATCH -J FitCache
#SBATCH -C nvme
#SBATCH --mail-type=BEGIN
#SBATCH --mail-user=ghu4@ncsu.edu
#SBATCH -o logs/pdsw/FitCache-%j.out

echo "Allocated Nodes: $SLURM_NODELIST"
# rm fitcache_server_log.*
# rm fitcache_intercept_log.*
module load rocm/6.0.0
module load python/3.10-miniforge3
module load gcc/12.2.0

export MIOPEN_DISABLE_CACHE=1
export MIOPEN_FIND_MODE=3
export MIOPEN_USER_DB_PATH=/tmp/miopen_cache_$SLURM_PROCID
mkdir -p $MIOPEN_USER_DB_PATH
# export MIOPEN_USER_DB_PATH=/lustre/orion/gen008/proj-shared/ghu4/miopen_cache


source /lustre/orion/gen008/proj-shared/ghu4/envs/fitcache_tf/bin/activate


export FitCache_LOG_LEVEL=500
export RDMAV_FORK_SAFE=1
export VERBS_LOG_LEVEL=4
export TF_CPP_MIN_LOG_LEVEL=3

export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/lustre/orion/gen008/proj-shared/log4c-1.2.4/install/lib/pkgconfig
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/lustre/orion/gen008/proj-shared/log4c-1.2.4/install/lib
export PATH=/lustre/orion/gen008/proj-shared/mercury-2.0.1/build/bin:$PATH
export LD_LIBRARY_PATH=/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/lib:$LD_LIBRARY_PATH
export C_INCLUDE_PATH=/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/include:$C_INCLUDE_PATH
export CPLUS_INCLUDE_PATH=/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/include:$CPLUS_INCLUDE_PATH
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/lib/pkgconfig

# ! Should change this to the correct path
# Small scale
export FitCache_DATA_DIR= /PATH_TO_YOUR_DATA_DIR # The Data that need to be cached

# total number of servers, If you have 4 nodes and then I want to run 2 servers on each node, then you should set this to 8
export FitCache_SERVER_COUNT=2

set -x
export BBPATH=/tmp

# Start 2 servers per node: 128 nodes × 2 = 256 servers total
srun -N 1 -n $FitCache_SERVER_COUNT -c 2 --ntasks-per-node=2 --cpus-per-task=1 --cpu-bind=cores /lustre/orion/gen008/proj-shared/ghu4/GHU_FitCache/build/src/fitcache_server $FitCache_SERVER_COUNT &

sleep 10

FitCache_SERVER_PID_FILE="/tmp/fitcache_server.pid"
echo "Waiting for FitCache server to write its PID to $FitCache_SERVER_PID_FILE..."
FitCache_SERVER_ACTUAL_PID=""
for i in $(seq 1 20); do # Wait up to 20 seconds
    if [ -f "$FitCache_SERVER_PID_FILE" ]; then
        FitCache_SERVER_ACTUAL_PID=$(cat "$FitCache_SERVER_PID_FILE")
        # Basic check if PID is a number and process exists
        if [[ "$FitCache_SERVER_ACTUAL_PID" =~ ^[0-9]+$ ]] && ps -p "$FitCache_SERVER_ACTUAL_PID" > /dev/null; then
            echo "FitCache server running with PID: $FitCache_SERVER_ACTUAL_PID"
            break
        fi
    fi
    sleep 1
done

if [ -z "$FitCache_SERVER_ACTUAL_PID" ]; then
    echo "Error: Could not get FitCache server PID from $FitCache_SERVER_PID_FILE after 20 seconds."
    # exit 1 # Or handle error
fi

# srun -N 2 -c3 --gpus-per-node=8 --ntasks-per-gpu=1 PATH_TO_YOUR_COMMAND_FILE/command_CF_FitCache.sh
srun -N 1 -c 4 --gpus-per-node=8 --ntasks-per-gpu=1 --cpu-bind=cores PATH_TO_YOUR_COMMAND_FILE/command_CF_FitCache.sh

echo "Shutting down FitCache server..."
if [ -n "$FitCache_SERVER_ACTUAL_PID" ] && ps -p "$FitCache_SERVER_ACTUAL_PID" > /dev/null; then
    echo "Sending SIGTERM to FitCache server (PID: $FitCache_SERVER_ACTUAL_PID)."
    kill -SIGTERM "$FitCache_SERVER_ACTUAL_PID"
    # Wait a bit for graceful shutdown
    sleep 5 # Adjust as needed
    if ps -p "$FitCache_SERVER_ACTUAL_PID" > /dev/null; then
        echo "Server didn't shut down, sending SIGKILL."
        kill -SIGKILL "$FitCache_SERVER_ACTUAL_PID"
    else
        echo "Server shut down."
    fi
    rm -f "$FitCache_SERVER_PID_FILE" # Clean up PID file
else
    echo "FitCache server (PID: $FitCache_SERVER_ACTUAL_PID) not found or already exited."
fi

echo "Slurm script finished."