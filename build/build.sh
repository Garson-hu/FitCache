#!/bin/bash

# module reset
#module load PrgEnv-gnu 
# module load mercury cmake libfabric
module unload darshan-runtime 

export FitCache_SERVER_COUNT=1
export FitCache_LOG_LEVEL=800
export RDMAV_FORK_SAFE=1
export VERBS_LOG_LEVEL=4
export BBPATH=/mnt/bb/$USER

export http_proxy=http://proxy.ccs.ornl.gov:3128/
export https_proxy=https://proxy.ccs.ornl.gov:3128/

export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/lustre/orion/gen008/proj-shared/log4c-1.2.4/install/lib/pkgconfig
export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/lustre/orion/gen008/proj-shared/log4c-1.2.4/install/lib
export PATH=/lustre/orion/gen008/proj-shared/mercury-2.0.1/build/bin:$PATH
export LD_LIBRARY_PATH=/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/lib:$LD_LIBRARY_PATH
export C_INCLUDE_PATH=/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/include:$C_INCLUDE_PATH
export CPLUS_INCLUDE_PATH=/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/include:$CPLUS_INCLUDE_PATH
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/lustre/orion/gen008/proj-shared/rlibrary/mercury2.0.1/lib/pkgconfig


cmake \
    -DCMAKE_C_COMPILER=/opt/cray/pe/gcc/12.2.0/bin/gcc \
    -DCMAKE_CXX_COMPILER=/opt/cray/pe/gcc/12.2.0/bin/g++ \
    -DCMAKE_C_FLAGS="-O3" \
    -DCMAKE_CXX_FLAGS="-O3" \
    .. -DDEBUG_HU=1

make -j4

