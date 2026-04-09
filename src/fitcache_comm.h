#ifndef __FitCache_RPC_ENGINE_INTERNAL_H__
#define __FitCache_RPC_ENGINE_INTERNAL_H__

extern "C" {
#include <mercury.h>
#include <mercury_bulk.h>
#include <mercury_macros.h>
#include <mercury_proc_string.h>
}

#include <string>
#include <atomic>
#include "fitcache_cache_policy.h"
using namespace std;

struct ms_read_state;

extern hg_class_t *hg_class;
extern hg_context_t *hg_context;
extern std::atomic<int> fitcache_progress_thread_shutdown_flags;
extern int fitcache_server_rank;
extern int server_rank;
extern "C" hg_return_t fitcache_rpc_handler_bulk_cb(const struct hg_cb_info *info);

//RPC Open Handler
MERCURY_GEN_PROC(fitcache_open_out_t, ((int32_t)(ret_status)))
/*
typedef struct {
    int32_t ret_status;
} fitcache_open_out_t;
*/

MERCURY_GEN_PROC(fitcache_open_in_t, ((hg_string_t)(path)))
/*
typedef struct {
    hg_string_t path;
} fitcache_open_in_t;
*/


//BULK Read Handler
MERCURY_GEN_PROC(fitcache_rpc_out_t, ((int64_t)(ret)))
MERCURY_GEN_PROC(fitcache_rpc_in_t, ((int64_t)(input_val))((hg_bulk_t)(bulk_handle))((int32_t)(accessfd))((int64_t)(offset))((int32_t)(requested_tier)))

//RPC Seek Handler
MERCURY_GEN_PROC(fitcache_seek_out_t, ((int64_t)(ret)))
/*
    typedef struct {
        int64_t ret;
    } fitcache_seek_out_t;
*/
MERCURY_GEN_PROC(fitcache_seek_in_t, ((int32_t)(fd))((int64_t)(offset))((int32_t)(whence)))


//Close Handler input arg
MERCURY_GEN_PROC(fitcache_close_in_t, ((int32_t)(fd)))

// RPC for triggering server to print stats
MERCURY_GEN_PROC(fitcache_rpc_trigger_srv_print_stats_in_t, ((int32_t)(dummy_arg)))
MERCURY_GEN_PROC(fitcache_rpc_trigger_srv_print_stats_out_t, ((int32_t)(status)))


//General
void fitcache_init_comm(hg_bool_t listen);
void *fitcache_progress_fn(void *args);
void fitcache_comm_list_addr();
void fitcache_comm_create_handle(hg_addr_t addr, hg_id_t id, hg_handle_t *handle);
void fitcache_shutdown_comm();
void fitcache_comm_free_addr(hg_addr_t addr);

//Retrieve the static variables
hg_class_t *fitcache_comm_get_class();
hg_context_t *fitcache_comm_get_context();


//Client
void fitcache_client_comm_gen_seek_rpc(uint32_t svr_hash, int fd, int64_t offset, int whence);
void fitcache_client_comm_gen_read_rpc(uint32_t svr_hash, int localfd, void* buffer, ssize_t count, off_t offset);

// Multi source read function
void fitcache_client_comm_gen_read_rpc_with_ms(uint32_t svr_hash, int localfd, void* buffer, ssize_t count, int64_t offset,
                                            hg_cb_t callback, struct fitcache_rpc_state* rpc_state);
void fitcache_client_comm_gen_open_rpc(uint32_t svr_hash, string path, int fd);
void fitcache_client_comm_gen_close_rpc(uint32_t svr_hash, int fd);
hg_addr_t fitcache_client_comm_lookup_addr(int rank);
void fitcache_client_comm_register_rpc();

// NEW: Per-file blocking functions
void fitcache_client_block_for_file(const std::string& file_path);
ssize_t fitcache_read_block_for_file(const std::string& file_path);

// DEPRECATED: Old global blocking functions (kept for backward compatibility)
void fitcache_client_block();
ssize_t fitcache_read_block();
ssize_t fitcache_seek_block();



//Mercury common RPC registration
hg_id_t fitcache_rpc_register_client(void);
hg_id_t fitcache_open_rpc_register_client(void);
hg_id_t fitcache_close_rpc_register_client(void);
hg_id_t fitcache_seek_rpc_register_client(void);

hg_id_t fitcache_rpc_register_server(void);
hg_id_t fitcache_open_rpc_register_server(void);
hg_id_t fitcache_close_rpc_register_server(void);
hg_id_t fitcache_seek_rpc_register_server(void);

// Universal RPC registration functions (can be used by both client and server)
hg_id_t fitcache_rpc_register(void);
hg_id_t fitcache_open_rpc_register(void);
hg_id_t fitcache_close_rpc_register(void);
hg_id_t fitcache_seek_rpc_register(void);
hg_id_t fitcache_trigger_srv_print_stats_rpc_register(void);

// Forward declarations for per-file sync context and state structures
struct fitcache_file_sync_context;
struct fitcache_open_state;
struct fitcache_seek_state;

/* struct used to carry state of overall operation across callbacks */
struct fitcache_rpc_state {
    uint32_t            value;
    hg_size_t           size;
    void                *buffer;
    hg_bulk_t           bulk_handle;
    hg_handle_t         handle;
    fitcache_rpc_in_t       in;

    // A pointer back to the ms_read_state, so the callback can update high-level info
    ms_read_state*      ms;   
    cache_tier_t        requested_tier;  
    
    // NEW: Per-file synchronization support
    fitcache_file_sync_context *sync_ctx;     // Per-file sync context
    std::string filename;                 // Associated filename
};


extern hg_return_t fitcache_rpc_handler(hg_handle_t handle);
extern hg_return_t fitcache_open_rpc_handler(hg_handle_t handle);
extern hg_return_t fitcache_trigger_srv_print_stats_rpc_handler(hg_handle_t handle);

#endif

