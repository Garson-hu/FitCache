
#include <string>
#include <iostream>
#include <map>
#include <unordered_map>
#include <mutex>

#include "fitcache_timer.h"
#include "fitcache_comm.h"
#include "fitcache_data_mover_internal.h"

extern "C" {
#include "fitcache_logging.h"
#include <fcntl.h>
#include <cassert>
#include <unistd.h>
}

// ========== Per-File Synchronization Implementation ==========

/**
 * Per-file synchronization context
 * Each file gets its own mutex/condition variable pair
 */
struct fitcache_file_sync_context {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ref_count;
    bool operation_done;
    int result;
    ssize_t read_result;     // For read operations
    
    fitcache_file_sync_context() : ref_count(1), operation_done(false), result(0), read_result(-1) {
        pthread_mutex_init(&mutex, NULL);
        pthread_cond_init(&cond, NULL);
    }
    
    ~fitcache_file_sync_context() {
        pthread_mutex_destroy(&mutex);
        pthread_cond_destroy(&cond);
    }
};

/**
 * Additional state structures for per-file sync context
 */
struct fitcache_open_state {
    uint32_t local_fd;
    fitcache_file_sync_context *sync_ctx;     // Per-file sync context
    std::string filename;                 // Associated filename
};

struct fitcache_seek_state {
    fitcache_file_sync_context *sync_ctx;     // Per-file sync context
    std::string filename;                 // Associated filename
};

// Global mapping from filenames to sync contexts
static std::unordered_map<std::string, fitcache_file_sync_context*> file_sync_map;
static std::mutex file_sync_map_mutex;  // Protects the global map

// File descriptor state management with sharding
#define FD_STATE_SHARDS 256
#define GET_FD_SHARD(fd) ((fd) % FD_STATE_SHARDS)

enum fitcache_fd_state {
    FitCache_FD_OPENING = 0,
    FitCache_FD_READY = 1,
    FitCache_FD_ERROR = 2
};

struct fd_state_entry {
    int fd;
    enum fitcache_fd_state state;
    std::string filename;
};

static std::unordered_map<int, fd_state_entry> fd_state_shards[FD_STATE_SHARDS];
static pthread_rwlock_t fd_state_locks[FD_STATE_SHARDS];

// Initialize FD state sharding
static void __attribute__((constructor)) init_fd_state_sharding() {
    for (int i = 0; i < FD_STATE_SHARDS; i++) {
        pthread_rwlock_init(&fd_state_locks[i], NULL);
    }
}

// Cleanup FD state sharding
static void __attribute__((destructor)) cleanup_fd_state_sharding() {
    for (int i = 0; i < FD_STATE_SHARDS; i++) {
        pthread_rwlock_destroy(&fd_state_locks[i]);
    }
}

/**
 * Get or create per-file sync context with thread safety and reference counting
 * Uses double-checked locking pattern for optimal performance
 */
static fitcache_file_sync_context* fitcache_get_file_sync_context(const std::string& filename) {
    // Fast path: check if context exists without global lock
    {
        std::lock_guard<std::mutex> lock(file_sync_map_mutex);
        auto it = file_sync_map.find(filename);
        if (it != file_sync_map.end()) {
            it->second->ref_count++;
            return it->second;
        }
    }
    
    // Slow path: create new context
    fitcache_file_sync_context* new_ctx = new fitcache_file_sync_context();
    
    {
        std::lock_guard<std::mutex> lock(file_sync_map_mutex);
        // Double-check: another thread might have created it
        auto it = file_sync_map.find(filename);
        if (it != file_sync_map.end()) {
            delete new_ctx;  // We don't need the new one
            it->second->ref_count++;
            return it->second;
        }
        
        // Actually insert the new context
        file_sync_map[filename] = new_ctx;
        return new_ctx;
    }
}

/**
 * Release per-file sync context with automatic cleanup
 */
static void fitcache_release_file_sync_context(const std::string& filename) {
    std::lock_guard<std::mutex> lock(file_sync_map_mutex);
    auto it = file_sync_map.find(filename);
    if (it != file_sync_map.end()) {
        it->second->ref_count--;
        if (it->second->ref_count <= 0) {
            delete it->second;
            file_sync_map.erase(it);
        }
    }
}

/**
 * Wait for operation completion on specific file
 */
static void fitcache_wait_for_file_operation(fitcache_file_sync_context* ctx) {
    pthread_mutex_lock(&ctx->mutex);
    while (!ctx->operation_done) {
        pthread_cond_wait(&ctx->cond, &ctx->mutex);
    }
    ctx->operation_done = false;  // Reset for next operation
    pthread_mutex_unlock(&ctx->mutex);
}

/**
 * Signal operation completion on specific file
 */
static void fitcache_signal_file_operation_done(fitcache_file_sync_context* ctx, int result = 0, ssize_t read_result = -1) {
    pthread_mutex_lock(&ctx->mutex);
    ctx->operation_done = true;
    ctx->result = result;
    if (read_result != -1) {
        ctx->read_result = read_result;
    }
    pthread_cond_signal(&ctx->cond);
    pthread_mutex_unlock(&ctx->mutex);
}

/**
 * File descriptor state management functions
 */
static void fitcache_set_fd_state(int fd, enum fitcache_fd_state state, const std::string& filename = "") {
    int shard = GET_FD_SHARD(fd);
    pthread_rwlock_wrlock(&fd_state_locks[shard]);
    fd_state_shards[shard][fd] = {fd, state, filename};
    pthread_rwlock_unlock(&fd_state_locks[shard]);
}

static enum fitcache_fd_state fitcache_get_fd_state(int fd, std::string* filename = nullptr) {
    int shard = GET_FD_SHARD(fd);
    pthread_rwlock_rdlock(&fd_state_locks[shard]);
    auto it = fd_state_shards[shard].find(fd);
    enum fitcache_fd_state state = (it != fd_state_shards[shard].end()) ? it->second.state : FitCache_FD_ERROR;
    if (filename && it != fd_state_shards[shard].end()) {
        *filename = it->second.filename;
    }
    pthread_rwlock_unlock(&fd_state_locks[shard]);
    return state;
}

static void fitcache_wait_fd_ready(int fd) {
    const int max_attempts = 1000;
    const int sleep_us = 1000;  // 1ms
    
    for (int i = 0; i < max_attempts; i++) {
        enum fitcache_fd_state state = fitcache_get_fd_state(fd);
        if (state == FitCache_FD_READY) {
            return;
        } else if (state == FitCache_FD_ERROR) {
            L4C_ERR("FD %d is in error state", fd);
            return;
        }
        usleep(sleep_us);
    }
    L4C_ERR("Timeout waiting for FD %d to become ready", fd);
}

// ========== Legacy Global Variables (Deprecated) ==========
// These are kept for backward compatibility but should not be used

/* RPC Globals */
static hg_id_t fitcache_client_rpc_id;
static hg_id_t fitcache_client_open_id;
static hg_id_t fitcache_client_close_id;
static hg_id_t fitcache_client_seek_id;

/* Mercury Data Caching */
static std::mutex address_cache_mutex;  // Protects address_cache
std::map<int, std::string> address_cache;  // Key: Rank, Value: Server Address
extern std::map<int, int > fd_redir_map;
extern std::map<int, std::string > fd_map;

extern "C" bool fitcache_file_tracked(int fd);
extern "C" bool fitcache_track_file(const char* path, int flags, int fd);

// ========== Updated Callback Functions ==========

static hg_return_t
fitcache_seek_cb(const struct hg_cb_info *info)
{
    FitCache_TIMING("HvacCommClient_(fitcache_seek_cb)_total");
    
    fitcache_seek_out_t out;
    ssize_t bytes_read = -1;
    fitcache_seek_state *seek_state = (fitcache_seek_state *)info->arg;

    HG_Get_output(info->info.forward.handle, &out);    
    bytes_read = out.ret;
    HG_Free_output(info->info.forward.handle, &out);
    HG_Destroy(info->info.forward.handle);

    // Signal completion to the specific file's sync context
    fitcache_signal_file_operation_done(seek_state->sync_ctx, bytes_read);
    
    // Release the sync context and cleanup
    fitcache_release_file_sync_context(seek_state->filename);
    delete seek_state;
    
    return HG_SUCCESS;    
}

static hg_return_t
fitcache_open_cb(const struct hg_cb_info *info)
{
    FitCache_TIMING("HvacCommClient_(fitcache_open_cb)_total");
    
    fitcache_open_out_t out;
    struct fitcache_open_state *open_state = (struct fitcache_open_state *)info->arg;    
    assert(info->ret == HG_SUCCESS);

    HG_Get_output(info->info.forward.handle, &out); 

    // Map the local fd to the remote fd
    fd_redir_map[open_state->local_fd] = out.ret_status;
    // Update FD state
    if (out.ret_status > 0) {
        fitcache_set_fd_state(open_state->local_fd, FitCache_FD_READY, open_state->filename);
    } else {
        fitcache_set_fd_state(open_state->local_fd, FitCache_FD_ERROR, open_state->filename);
    }
    
    L4C_INFO("Open RPC Returned FD: %d, Local FD %d\n",out.ret_status, open_state->local_fd);

    HG_Free_output(info->info.forward.handle, &out);
    HG_Destroy(info->info.forward.handle);

    // Signal completion to the specific file's sync context
    fitcache_signal_file_operation_done(open_state->sync_ctx, out.ret_status);
    
    // Release the sync context and cleanup
    fitcache_release_file_sync_context(open_state->filename);
    delete open_state;
    
    return HG_SUCCESS;
}

static hg_return_t
fitcache_read_cb(const struct hg_cb_info *info)
{
    FitCache_TIMING("HvacCommClient_(fitcache_read_cb)_total");
    
    hg_return_t ret;
    fitcache_rpc_out_t out;
    ssize_t bytes_read = -1;
    struct fitcache_rpc_state *fitcache_rpc_state_p = (struct fitcache_rpc_state *)info->arg;
    assert(info->ret == HG_SUCCESS);

    /* decode response */
    HG_Get_output(info->info.forward.handle, &out);
    bytes_read = out.ret;
    
    /* clean up resources consumed by this rpc */
    ret = HG_Bulk_free(fitcache_rpc_state_p->bulk_handle);
    assert(ret == HG_SUCCESS);

    ret = HG_Free_output(info->info.forward.handle, &out);
    assert(ret == HG_SUCCESS);
    
    ret = HG_Destroy(info->info.forward.handle);
    assert(ret == HG_SUCCESS);

    // Signal completion to the specific file's sync context
    fitcache_signal_file_operation_done(fitcache_rpc_state_p->sync_ctx, 0, bytes_read);
    
    // Release the sync context and cleanup
    fitcache_release_file_sync_context(fitcache_rpc_state_p->filename);
    free(fitcache_rpc_state_p);
    
    return HG_SUCCESS;
}

// ========== Updated RPC Registration ==========

void fitcache_client_comm_register_rpc()
{   
    fitcache_client_open_id = fitcache_open_rpc_register_client();
    fitcache_client_rpc_id = fitcache_rpc_register_client();    
    fitcache_client_close_id = fitcache_close_rpc_register_client();
    fitcache_client_seek_id = fitcache_seek_rpc_register_client();
}

// ========== Updated Blocking Functions ==========

/**
 * NEW: Per-file blocking function for any operation
 */
void fitcache_client_block_for_file(const std::string& file_path) {
    FitCache_TIMING("HvacCommClient_block_for_file_total");
    
    fitcache_file_sync_context* ctx = fitcache_get_file_sync_context(file_path);
    fitcache_wait_for_file_operation(ctx);
    fitcache_release_file_sync_context(file_path);
}

/**
 * NEW: Per-file blocking function for read operations
 */
ssize_t fitcache_read_block_for_file(const std::string& file_path) {
    FitCache_TIMING("HvacCommClient_read_block_for_file_total");
    
    fitcache_file_sync_context* ctx = fitcache_get_file_sync_context(file_path);
    fitcache_wait_for_file_operation(ctx);
    ssize_t result = ctx->read_result;
    fitcache_release_file_sync_context(file_path);
    return result;
}

/**
 * DEPRECATED: Old global blocking functions (kept for backward compatibility)
 */
void fitcache_client_block() {
    // This should not be used anymore, but kept for compatibility
    L4C_WARN("fitcache_client_block() is deprecated, use fitcache_client_block_for_file() instead");
}

ssize_t fitcache_read_block() {
    // This should not be used anymore, but kept for compatibility
    L4C_WARN("fitcache_read_block() is deprecated, use fitcache_read_block_for_file() instead");
    return -1;
}

ssize_t fitcache_seek_block() {
    // This should not be used anymore, but kept for compatibility
    L4C_WARN("fitcache_seek_block() is deprecated, use per-file blocking instead");
    return -1;
}


void fitcache_client_comm_gen_close_rpc(uint32_t svr_hash, int fd)
{   
    FitCache_TIMING("HvacCommClient_gen_close_rpc_Total");
    
    hg_addr_t svr_addr; 
    fitcache_close_in_t in;
    hg_handle_t handle; 
    int ret;
    /* Get address */
    svr_addr = fitcache_client_comm_lookup_addr(svr_hash);        

    /* create create handle to represent this rpc operation */
    fitcache_comm_create_handle(svr_addr, fitcache_client_close_id, &handle);

    in.fd = fd_redir_map[fd];

    ret = HG_Forward(handle, NULL, NULL, &in);
    assert(ret == 0);

    fd_redir_map.erase(fd);

    HG_Destroy(handle);
    fitcache_comm_free_addr(svr_addr);

    return;

}

/*
*    This function is used to open a file on the remote server
*    @param svr_hash: The hash of the server to connect to
*    @param path: The original path of the file to open
*    @param fd: The local file descriptor
*/
void fitcache_client_comm_gen_open_rpc(uint32_t svr_hash, string path, int fd)
{
    FitCache_TIMING("HvacCommClient_gen_open_rpc_Total");
    
    hg_addr_t svr_addr;
    fitcache_open_in_t in;
    hg_handle_t handle;
    struct fitcache_open_state *fitcache_open_state_p;  
    int ret;
    // done = HG_FALSE; // This line is removed as per the new_code

    /* Get address according to server hash  */
    /* svr_hash is calculated as: ((fd_map[fd]) % g_fitcache_server_count) */
    svr_addr = fitcache_client_comm_lookup_addr(svr_hash);  
    if (!svr_addr) {
        L4C_ERR("Open RPC: could not resolve server rank %d (missing .ports.cfg?)", (int)svr_hash);
        return;
    }
    L4C_INFO("PREAD: FitCache: Open RPC Server Hash: %d", svr_hash);
    /* Allocate args for callback pass through */
    fitcache_open_state_p = new fitcache_open_state();
    fitcache_open_state_p->local_fd = fd;

    // Get or create per-file sync context
    fitcache_open_state_p->sync_ctx = fitcache_get_file_sync_context(path);
    fitcache_open_state_p->filename = path;
    
    // Set FD state to opening
    fitcache_set_fd_state(fd, FitCache_FD_OPENING, path);

    /* create  handle to represent this rpc operation
        & warp the function from HG_Create()    
    */    
    fitcache_comm_create_handle(svr_addr, fitcache_client_open_id, &handle);  
    in.path = (hg_string_t)malloc(strlen(path.c_str()) + 1 );
    sprintf(in.path,"%s",path.c_str());
    
    L4C_INFO("FitCache: Open RPC Path: %s", in.path);
    L4C_INFO("FitCache: Open RPC FD: %d", fd);

    ret = HG_Forward(handle, fitcache_open_cb, fitcache_open_state_p, &in);
    free(in.path);
    if (ret != HG_SUCCESS) {
        L4C_ERR("HG_Forward() failed with ret=%d", ret);
    } else {
        L4C_INFO("HG_Forward success for open_rpc");
    }
    /*
        & Warp the function from HG_Addr_free()
    */
    fitcache_comm_free_addr(svr_addr);

    return;

}

void fitcache_client_comm_gen_read_rpc(uint32_t svr_hash, int localfd, void *buffer, ssize_t count, off_t offset)
{
    FitCache_TIMING("HvacCommClient_gen_read_rpc_Total");
    
    hg_addr_t svr_addr;
    fitcache_rpc_in_t in;
    const struct hg_info *hgi;
    int ret;
    struct fitcache_rpc_state *fitcache_rpc_state_p;
    // done = HG_FALSE; // This line is removed as per the new_code
    // read_ret = -1; // This line is removed as per the new_code

    /* Get address */
    svr_addr = fitcache_client_comm_lookup_addr(svr_hash);
    if (!svr_addr) {
        L4C_ERR("Read RPC: could not resolve server rank %d (missing .ports.cfg?)", (int)svr_hash);
        return;
    }

    /* set up state structure */
    fitcache_rpc_state_p = (struct fitcache_rpc_state *)malloc(sizeof(*fitcache_rpc_state_p));
    fitcache_rpc_state_p->size = count;


    /* This includes allocating a src buffer for bulk transfer */
    fitcache_rpc_state_p->buffer = buffer;
    assert(fitcache_rpc_state_p->buffer);

    // Get filename from FD map for per-file sync context
    std::string filename;
    fitcache_get_fd_state(localfd, &filename);
    if (filename.empty()) {
        // Fallback: try to get from fd_map
        extern std::map<int, std::string> fd_map;
        auto it = fd_map.find(localfd);
        if (it != fd_map.end()) {
            filename = it->second;
        } else {
            filename = "unknown_fd_" + std::to_string(localfd);
        }
    }
    
    // Get or create per-file sync context
    fitcache_rpc_state_p->sync_ctx = fitcache_get_file_sync_context(filename);
    fitcache_rpc_state_p->filename = filename;

    /* create handle to represent this rpc operation */
    fitcache_comm_create_handle(svr_addr, fitcache_client_rpc_id, &(fitcache_rpc_state_p->handle));

    /* register buffer for rdma/bulk access by server */
    hgi = HG_Get_info(fitcache_rpc_state_p->handle);
    assert(hgi);
    ret = HG_Bulk_create(hgi->hg_class, 1, (void**) &(buffer),
       &(fitcache_rpc_state_p->size), HG_BULK_WRITE_ONLY, &(in.bulk_handle));

    fitcache_rpc_state_p->bulk_handle = in.bulk_handle;
    assert(ret == HG_SUCCESS);

    /* Send rpc. Note that we are also transmitting the bulk handle in the
     * input struct.  It was set above.
     */
    in.input_val = count;

    //Convert FD to remote FD
    in.accessfd = fd_redir_map[localfd];
	in.offset = offset;
    // Pseudo dual-tier hint: default to DRAM when single-source path
    in.requested_tier = (int32_t)CACHE_TIER_DRAM;
    
    
    ret = HG_Forward(fitcache_rpc_state_p->handle, fitcache_read_cb, fitcache_rpc_state_p, &in);
    assert(ret == 0);

    fitcache_comm_free_addr(svr_addr);

    return;
}

void fitcache_client_comm_gen_read_rpc_with_ms(uint32_t svr_hash, int localfd, void* buffer, ssize_t count, int64_t offset,
                                            hg_cb_t callback, struct fitcache_rpc_state* rpc_state)
{

    if(DEBUG_HU)
        L4C_INFO("PREAD -1 DEBUG: DEBUG_HU: FitCache: Offset %lld", offset);
    hg_addr_t svr_addr = fitcache_client_comm_lookup_addr(svr_hash);
    if (!svr_addr) {
        L4C_ERR("Read RPC (ms): could not resolve server rank %d (missing .ports.cfg?)", (int)svr_hash);
        return;
    }
    fitcache_rpc_in_t in;
    const struct hg_info *hgi;
    int ret;

    fitcache_comm_create_handle(svr_addr, fitcache_client_rpc_id, &(rpc_state->handle));
    
    /* register buffer for rdma/bulk access by server */
    rpc_state->size = count;
    rpc_state->buffer = buffer;
    assert(rpc_state->buffer);

    hgi = HG_Get_info(rpc_state->handle);
    assert(hgi);
    ret = HG_Bulk_create(hgi->hg_class, 1, (void**) &(buffer),
       &(rpc_state->size), HG_BULK_WRITE_ONLY, &(in.bulk_handle));
    assert(ret == HG_SUCCESS);

    rpc_state->bulk_handle = in.bulk_handle;

    in.input_val = count;
    in.accessfd = fd_redir_map[localfd];
    if(DEBUG_HU)
        L4C_INFO("PREAD -1 DEBUG: DEBUG_HU: FitCache: Remote FD %d, Local fd: %d", in.accessfd, localfd);
    in.offset = offset;
    // Pseudo tier hint: follow rpc_state->requested_tier (DRAM or NVME)
    in.requested_tier = (int32_t)rpc_state->requested_tier;
    ret = HG_Forward(rpc_state->handle, callback, rpc_state, &in);
    assert(ret == 0);

    fitcache_comm_free_addr(svr_addr);

    return;


}

void fitcache_client_comm_gen_seek_rpc(uint32_t svr_hash, int fd, int64_t offset, int whence)
{
    FitCache_TIMING("HvacCommClient_gen_seek_rpc_Total");
    
    hg_addr_t svr_addr;
    fitcache_seek_in_t in;
    hg_handle_t handle;
    // read_ret = -1; // This line is removed as per the new_code
    int ret;
    // done = HG_FALSE; // This line is removed as per the new_code

    /* Get address */
    svr_addr = fitcache_client_comm_lookup_addr(svr_hash);    
    if (!svr_addr) {
        L4C_ERR("Seek RPC: could not resolve server rank %d (missing .ports.cfg?)", (int)svr_hash);
        return;
    }

    /* Allocate args for callback pass through */
    fitcache_seek_state *seek_state = new fitcache_seek_state();
    
    // Get filename from FD map for per-file sync context
    std::string filename;
    fitcache_get_fd_state(fd, &filename);
    if (filename.empty()) {
        // Fallback: try to get from fd_map
        extern std::map<int, std::string> fd_map;
        auto it = fd_map.find(fd);
        if (it != fd_map.end()) {
            filename = it->second;
        } else {
            filename = "unknown_fd_" + std::to_string(fd);
        }
    }
    
    // Get or create per-file sync context
    seek_state->sync_ctx = fitcache_get_file_sync_context(filename);
    seek_state->filename = filename;
    
    /* create create handle to represent this rpc operation */    
    fitcache_comm_create_handle(svr_addr, fitcache_client_seek_id, &handle);  

    in.fd = fd_redir_map[fd];
    in.offset = offset;
    in.whence = whence;
    

    ret = HG_Forward(handle, fitcache_seek_cb, seek_state, &in);
    assert(ret == 0);

    
    fitcache_comm_free_addr(svr_addr);

    return;

}


//We've converted the filename to a rank
//Using standard c++ hashing modulo servers
//Find the address
hg_addr_t fitcache_client_comm_lookup_addr(int rank)
{
    FitCache_TIMING("HvacCommClient_Lookup_Addr_TOTAL");
    
	// L4C_INFO("Guangxing RANK %d", rank);
	{
		std::lock_guard<std::mutex> lock(address_cache_mutex);
		if (address_cache.find(rank) != address_cache.end())
		{
			hg_addr_t target_server;
			HG_Addr_lookup2(fitcache_comm_get_class(), address_cache[rank].c_str(), &target_server);
			return target_server;
		}
	}

	/* The hardway */
	char filename[PATH_MAX];
	char svr_str[PATH_MAX];
	int svr_rank = -1;
	char *jobid = getenv("SLURM_JOBID");
	hg_addr_t target_server = nullptr;
	bool svr_found = false;
	FILE *na_config = NULL;
	sprintf(filename, "./.ports.cfg.%s", jobid);
	na_config = fopen(filename,"r+");

	while (fscanf(na_config, "%d %s\n",&svr_rank, svr_str) == 2)
	{
		if (svr_rank == rank){
			L4C_INFO("Connecting to %s %d\n", svr_str, svr_rank);
			svr_found = true;
            break;
		}
	}
	fclose(na_config);

	if (svr_found){
		std::lock_guard<std::mutex> lock(address_cache_mutex);
		address_cache[rank] = svr_str;
		HG_Addr_lookup2(fitcache_comm_get_class(),svr_str,&target_server);
	}

	return target_server;
}


/* register this particular rpc type with Mercury */
hg_id_t
fitcache_rpc_register_client(void)
{
    hg_id_t tmp;
    // & replace HG_Register()
    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_base_rpc", fitcache_rpc_in_t, fitcache_rpc_out_t, NULL);

    return tmp;
}

hg_id_t
fitcache_open_rpc_register_client(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_open_rpc", fitcache_open_in_t, fitcache_open_out_t, NULL);

    return tmp;
}

hg_id_t
fitcache_close_rpc_register_client(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_close_rpc", fitcache_close_in_t, void, NULL);
    

    int ret =  HG_Registered_disable_response(hg_class, tmp,HG_TRUE);                        
    assert(ret == HG_SUCCESS);

    return tmp;
}

/* register this particular rpc type with Mercury */
hg_id_t
fitcache_seek_rpc_register_client(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_seek_rpc", fitcache_seek_in_t, fitcache_seek_out_t, NULL);

    return tmp;
}

/* register trigger server print stats RPC with Mercury */
hg_id_t
fitcache_trigger_srv_print_stats_rpc_register(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_trigger_srv_print_stats_rpc", 
        fitcache_rpc_trigger_srv_print_stats_in_t, 
        fitcache_rpc_trigger_srv_print_stats_out_t, 
        NULL);

    return tmp;
}

// Universal RPC registration functions
hg_id_t fitcache_rpc_register(void) {
    return fitcache_rpc_register_client();
}

hg_id_t fitcache_open_rpc_register(void) {
    return fitcache_open_rpc_register_client();
}

hg_id_t fitcache_close_rpc_register(void) {
    return fitcache_close_rpc_register_client();
}

hg_id_t fitcache_seek_rpc_register(void) {
    return fitcache_seek_rpc_register_client();
}