/* Data mover responsible for maintaining the NVMe state
 * and prefetching the data
 */
#include <filesystem>
#include <string>
#include <queue>
#include <iostream>

#include <pthread.h>
#include <string.h>
#include <chrono>
#include "fitcache_logging.h"
#include "fitcache_data_mover_internal.h"
#include "fitcache_cache_policy.h"
using namespace std;
namespace fs = std::filesystem;

// uint64_t g_dram_capacity_bytes;
// uint64_t g_nvme_capacity_bytes;
// std::string g_dram_path;
// std::string g_nvme_path;
uint64_t g_dram_used_bytes = 0;
uint64_t g_nvme_used_bytes = 0;

pthread_cond_t data_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;

unordered_map<int,string> fd_to_path;             // & Server File Descriptor -> Original path
unordered_map<string, string> path_cache_map;     // & Original path -> Redirection path
shared_mutex cache_mtx;                       // & Mutex for the path cache

queue<string> data_queue;               // & List of files to be moved

void *fitcache_data_mover_fn(void *args)
{
    if(getenv("FitCache_DRAM_PATH")  == NULL)
    {
        L4C_FATAL("Please set environment variables FitCache_DRAM_PATH\n");
        return NULL;
    }

    string g_dram_path = string(getenv("FitCache_DRAM_PATH"));

    if(getenv("FitCache_NVME_PATH")  == NULL)
    {
        L4C_FATAL("Please set environment variables FitCache_NVME_PATH\n");
        return NULL;
    }

    string g_nvme_path = string(getenv("FitCache_NVME_PATH"));

    if(getenv("FitCache_DRAM_CAPACITY")  == NULL)
    {
        L4C_FATAL("Please set environment variables FitCache_DRAM_CAPACITY\n");
        return NULL;
    }

    char* g_dram_capacity_tmp  = getenv("FitCache_DRAM_CAPACITY");
    uint64_t g_dram_capacity_bytes = std::stoull(g_dram_capacity_tmp);

    if(getenv("FitCache_NVME_CAPACITY")  == NULL)
    {
        L4C_FATAL("Please set environment variables FitCache_NVME_CAPACITY\n");
        return NULL;
    }

    char* g_nvme_capacity_tmp  = getenv("FitCache_NVME_CAPACITY");
    uint64_t g_nvme_capacity_bytes = std::stoull(g_nvme_capacity_tmp);
    
    L4C_INFO("FitCache_DRAM_PATH is set to %s", g_dram_path.c_str());
    L4C_INFO("FitCache_NVME_PATH is set to %s", g_nvme_path.c_str());
    L4C_INFO("FitCache_DRAM_CAPACITY is set to %llu", g_dram_capacity_bytes);
    L4C_INFO("FitCache_NVME_CAPACITY is set to %llu", g_nvme_capacity_bytes);

    queue<string> local_list;

    // string fsdax_base = string(fsdax_path) + "/XXXXXX";
    // string ssd_base   = string(ssd_path)   + "/XXXXXX";

    string dram_base = string(g_dram_path);
    string nvme_base   = string(g_nvme_path);

    while (1) {
        pthread_mutex_lock(&data_mutex);
        pthread_cond_wait(&data_cond, &data_mutex);
        
        int queue_size = data_queue.size();
        L4C_INFO("DEBUG: Data queue size: %d", queue_size);
        /* We can do stuff here when signaled */
        while (!data_queue.empty()){
            local_list.push(data_queue.front());
            data_queue.pop();
        }

        pthread_mutex_unlock(&data_mutex);

        while (!local_list.empty())
        {
            L4C_INFO("Data mover: Moving file %s", local_list.front().c_str());
            string original_path = local_list.front();

            // (&used_dram_bytes, &used_nvme_bytes);

            // Determine target tier by checking remaining capacity with file size
            bool to_dram = true;
            uint64_t file_size = 0;
            try {
                file_size = fs::file_size(original_path);
            } catch (const fs::filesystem_error &e) {
                L4C_ERR("Data mover: failed to stat %s: %s", original_path.c_str(), e.what());
                local_list.pop();
                continue;
            }

            if (g_dram_used_bytes + file_size <= g_dram_capacity_bytes) {
                to_dram = true;
            } else if (g_nvme_used_bytes + file_size <= g_nvme_capacity_bytes) {
                to_dram = false;
            } else {
                L4C_ERR("No space in DRAM or NVME to move file %s\n", original_path.c_str());
                local_list.pop();
                continue;
            }

            // Simple hash bucket implementation
            string basepath = to_dram ? dram_base : nvme_base;
            L4C_INFO("Basepath %s", basepath.c_str());
            size_t h = std::hash<std::string>{}(original_path);
            char subdir[8];
            sprintf(subdir, "%02zx/%02zx", (h >> 8) & 0xFF, h & 0xFF);
            string dirpath = basepath + "/" + subdir;
            fs::create_directories(dirpath);
            L4C_INFO("Creating directory %s", dirpath.c_str());
            string filename = dirpath + "/" + fs::path(original_path).filename().string();

            try{
                fs::copy(original_path, filename);
                path_cache_map[original_path] = filename;
                // Update the used bytes
                if(to_dram)
                {
                    g_dram_used_bytes += file_size;
                }
                else
                {
                    g_nvme_used_bytes += file_size;
                }
                if(DEBUG_HU)
                    L4C_INFO("Data mover: Copied %s -> %s using simple hash buckets", original_path.c_str(), filename.c_str());

            } catch (const fs::filesystem_error& e)
            {
                fprintf(stderr, "Error : %s copying from %s to %s\n", e.what(), original_path.c_str(), filename.c_str());
                L4C_INFO("Failed to copy %s to %s\n", original_path.c_str(), filename.c_str());
            }        
            
            local_list.pop();
        }
    }
    return NULL;
}
