#ifndef FitCache_MULTI_SOURCE_READ_H
#define FitCache_MULTI_SOURCE_READ_H

#ifdef __cplusplus
    #include <cstddef>
    #include <mutex>
    extern "C" {
#endif

#include <pthread.h>
#include <stddef.h>   
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>

extern std::vector<int> g_pm_ranks;   // DRAM ranks
extern std::vector<int> g_ssd_ranks;  // NVME ranks

ssize_t ms_read(int fd, void* buf, size_t count, int64_t offset);

// A structure to hold the asynchronous state for the multi-source read
struct ms_read_state {
    pthread_mutex_t      lock;
    pthread_cond_t       cond;
    bool                 completed;    // Have we returned data to the user?
    bool                 pm_done;      // Has DRAM request finished?
    bool                 ssd_done;     // Has NVME request finished?
    ssize_t              pm_result;    // Bytes read from DRAM
    ssize_t              ssd_result;   // Bytes read from NVME
};

#ifdef __cplusplus
}  // close extern "C"
#endif

#endif  // FitCache_MULTI_SOURCE_READ_H