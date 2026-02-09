#pragma once
#ifndef __FitCache_DATA_MOVER_INTERNAL_H__
#define __FitCache_DATA_MOVER_INTERNAL_H__

#include <queue>
#include <map>

#include <unordered_map>
#include <shared_mutex>
#include <mutex>
/*Data Mover */

#ifdef FitCache_SERVER
extern pthread_cond_t data_cond;
extern pthread_mutex_t data_mutex;
extern std::queue<std::string> data_queue;

extern std::unordered_map<int, std::string> fd_to_path;
extern std::unordered_map<std::string, std::string> path_cache_map;

extern std::shared_mutex cache_mtx;

#endif


void *fitcache_data_mover_fn(void *args);

#endif //__FitCache_DATA_MOVER_INTERNAL_H__