#!/bin/bash

set -x 
LD_PRELOAD=PATH_TO_YOUR_FITCACHE_CLIENT/libfitcache_client.so python3 PATH_TO_YOUR_CODE/train.py -d

# echo DONE `hostname`
