#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <cassert>
#include <mutex>
#include <vector>
#include <condition_variable>

#include "fitcache_internal.h"        // For fitcache_file_tracked, fitcache_get_path, etc.
#include "fitcache_comm.h"
#include "fitcache_cache_policy.h"
#include "fitcache_multi_source_read.h"
#include "fitcache_logging.h"         // For L4C_INFO, L4C_ERR

extern std::map<int, int > fd_redir_map;
// extern map<int,string> fd_to_path;             // & Server File Descriptor -> Original path
extern std::map<int,std::string> fd_map;	


static hg_return_t ms_read_cb(const struct hg_cb_info *info);

extern uint32_t g_fitcache_server_count;

ssize_t ms_read(int fd, void* buf, size_t count, int64_t offset)
{
    
    // Check if file is tracked, otherwise fallback to normal read
    if (!fitcache_file_tracked(fd)) 
    {
        MAP_OR_FAIL(read);
        // L4C_INFO("File not tracked, falling back to normal read");
        if(offset == -1) 
        {
            return __real_read(fd, buf, count);
        }
        else 
        {
            MAP_OR_FAIL(pread);
            return __real_pread(fd, buf, count, offset);
        }
    }

    L4C_INFO("File tracked, using multi-source read, the file descriptor is %d", fd);

    // Create ms_read_state to store asynchronous results
    ms_read_state *ms = (ms_read_state*) calloc(1, sizeof(ms_read_state));
    pthread_mutex_init(&ms->lock, NULL);
    pthread_cond_init(&ms->cond, NULL);
    ms->completed = false;
    ms->pm_done = false;
    ms->ssd_done = false;
    ms->pm_result = -1;
    ms->ssd_result= -1;

    fitcache_rpc_state* dram_state = (fitcache_rpc_state*) calloc(1, sizeof(fitcache_rpc_state));
    dram_state->ms = ms;
    dram_state->buffer = buf;
    dram_state->size = count;
    dram_state->requested_tier = CACHE_TIER_DRAM;

    fitcache_rpc_state* nvme_state = (fitcache_rpc_state*) calloc(1, sizeof(fitcache_rpc_state));
    nvme_state->ms = ms;
    nvme_state->buffer = buf;
    nvme_state->size = count;
    nvme_state->requested_tier = CACHE_TIER_NVME;

    int remote_fd = fd_redir_map[fd];
    L4C_INFO("Remote fd: %d", remote_fd);
    int host = std::hash<std::string>{}(fd_map[fd]) % g_fitcache_server_count;

    fitcache_client_comm_gen_read_rpc_with_ms(host, fd, buf, count, offset,
        ms_read_cb, dram_state);
    
    L4C_INFO("Generated read rpc with ms");
    ssize_t final_result = -1;
    bool done = false;

    pthread_mutex_lock(&ms->lock);

    while(!ms->completed) 
    {
        pthread_cond_wait(&ms->cond, &ms->lock);
    }

    if(ms->pm_done && ms->pm_result != -1) 
    {
        final_result = ms->pm_result;
        done = true;
    }
    else if(ms->ssd_done && ms->ssd_result != -1) 
    {
        final_result = ms->ssd_result;
        done = true;
    }
    pthread_mutex_unlock(&ms->lock);
    if(DEBUG_HU)
    {
        L4C_INFO("Final result: %ld", final_result);
        L4C_INFO("ms->pm_done: %d, ms->ssd_done: %d", ms->pm_done, ms->ssd_done);
        L4C_INFO("ms->pm_result: %ld, ms->ssd_result: %ld", ms->pm_result, ms->ssd_result);
    }

    // 7. Clean up
    free(dram_state);
    free(nvme_state);
    pthread_mutex_destroy(&ms->lock);
    pthread_cond_destroy(&ms->cond);
    free(ms);
    // 8. Return whichever result completed first
    return final_result;
}

static hg_return_t ms_read_cb(const struct hg_cb_info *info)
{
    fitcache_rpc_state* state = (fitcache_rpc_state*) info->arg;
    ms_read_state* ms = state->ms;


    if(info->ret != HG_SUCCESS) 
    {
        L4C_INFO("RPC failed");
        pthread_mutex_lock(&ms->lock);
        if(state->requested_tier == CACHE_TIER_DRAM) 
        {
            ms->pm_done = true;
            ms->pm_result = -1;
        }
        else if(state->requested_tier == CACHE_TIER_NVME) 
        {
            ms->ssd_done = true;
            ms->ssd_result = -1;
        }

        if (!ms->completed) 
        {
            // check if the other request is done or not
            if ((ms->pm_done && ms->ssd_done) &&
                (ms->pm_result < 0 && ms->ssd_result < 0)) 
                {
                // both fail => complete
                ms->completed = true;
                pthread_cond_signal(&ms->cond);
            }
        }
        pthread_mutex_unlock(&ms->lock);

        if (state->bulk_handle != HG_BULK_NULL) {
            HG_Bulk_free(state->bulk_handle);
        }
        HG_Destroy(info->info.forward.handle);

        return HG_SUCCESS;
    }

    fitcache_rpc_out_t out;
    hg_return_t ret = HG_Get_output(info->info.forward.handle, &out);
    if (ret != HG_SUCCESS) 
    {
        // handle error
        pthread_mutex_lock(&ms->lock);
        if (state->requested_tier == CACHE_TIER_DRAM) 
        {
            ms->pm_done    = true;
            ms->pm_result  = -1;
        } else {
            ms->ssd_done   = true;
            ms->ssd_result = -1;
        }
        // check if both done => complete
        if (!ms->completed) 
        {
            if ((ms->pm_done && ms->ssd_done) &&
                (ms->pm_result < 0 && ms->ssd_result < 0)) 
                {
                ms->completed = true;
                pthread_cond_signal(&ms->cond);
            }
        }
        pthread_mutex_unlock(&ms->lock);

        // TODO: add destroy in here.
        if (state->bulk_handle != HG_BULK_NULL) {
            HG_Bulk_free(state->bulk_handle);
        }
        HG_Destroy(info->info.forward.handle);

        return HG_SUCCESS;
    }

    ssize_t bytes_read = out.ret;
    fitcache_rpc_state *rpc = state;

    ret = HG_Bulk_free(rpc->bulk_handle);
    ret = HG_Free_output(info->info.forward.handle, &out);
    ret = HG_Destroy(info->info.forward.handle);

    // success read, update ms
    pthread_mutex_lock(&ms->lock);
    if (rpc->requested_tier == CACHE_TIER_DRAM) {
        ms->pm_done   = true;
        ms->pm_result = bytes_read;
    } else if (rpc->requested_tier == CACHE_TIER_NVME)
    {
        ms->ssd_done   = true;
        ms->ssd_result = bytes_read;
    }
    // if success => set ms->completed
    if (bytes_read >= 0 && !ms->completed) {
        ms->completed = true;
        pthread_cond_signal(&ms->cond);
    } else 
    {
        // if the other side also done or both fail => complete
        if ((ms->pm_done && ms->ssd_done) &&
            (ms->pm_result < 0 && ms->ssd_result < 0)) 
        {
            ms->completed = true;
            pthread_cond_signal(&ms->cond);
        }
    }
    pthread_mutex_unlock(&ms->lock);

    return HG_SUCCESS;
}