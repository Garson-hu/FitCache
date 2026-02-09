#include "fitcache_comm.h"
#include "fitcache_data_mover_internal.h"
#include "fitcache_cache_policy.h"
#include "fitcache_logging.h"
#include "fitcache_timer.h"

#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cassert>
#include <map>
#include <string>
#include <cstdio>
#include <ctime>

// External server ID variable declared in fitcache_server.cpp

hg_return_t
fitcache_open_rpc_handler(hg_handle_t handle)
{
    fitcache_open_in_t in;
    fitcache_open_out_t out;
    int ret = HG_Get_input(handle, &in);
    assert(ret == 0);

    std::string redir_path = in.path;
    L4C_INFO("Redirected Path before cache %s", redir_path.c_str());

    // redir_path = path_cache_map[redir_path];
    if (path_cache_map.find(redir_path) != path_cache_map.end())
    {
        L4C_INFO("Server Rank %d : Successful Redirection %s to %s", server_rank, redir_path.c_str(), path_cache_map[redir_path].c_str());
        redir_path = path_cache_map[redir_path];
    }

    L4C_INFO("Redirected Path after cache %s", redir_path.c_str());
    
    // out.ret_status is the server file descriptor  
    out.ret_status = open(redir_path.c_str(), O_RDONLY);
    L4C_INFO("Server Rank %d : Opened %s with fd %d", server_rank, redir_path.c_str(), out.ret_status);
    if (out.ret_status == -1) {
        L4C_ERR("Failed to open file: %s", redir_path.c_str());
        out.ret_status = -errno;  // Return the negative error code
    } else {
        std::unique_lock<std::shared_mutex> wlock(cache_mtx);
        fd_to_path[out.ret_status] = in.path;
    }

    HG_Respond(handle, NULL, NULL, &out);

    return (hg_return_t)ret;
}


// * handle read request
hg_return_t
fitcache_rpc_handler(hg_handle_t handle)
{
    int ret;
    struct fitcache_rpc_state *fitcache_rpc_state_p;
    const struct hg_info *hgi;
    ssize_t readbytes;
    fitcache_rpc_state_p = (struct fitcache_rpc_state*)malloc(sizeof(*fitcache_rpc_state_p));

    /* decode input */
    HG_Get_input(handle, &fitcache_rpc_state_p->in);   
    
    /* This includes allocating a target buffer for bulk transfer */
    fitcache_rpc_state_p->buffer = calloc(1, fitcache_rpc_state_p->in.input_val);
    assert(fitcache_rpc_state_p->buffer);

    fitcache_rpc_state_p->size = fitcache_rpc_state_p->in.input_val;
    fitcache_rpc_state_p->handle = handle;

    /* register local target buffer for bulk access */
    hgi = HG_Get_info(handle);
    assert(hgi);
    ret = HG_Bulk_create(hgi->hg_class, 1, &fitcache_rpc_state_p->buffer,
        &fitcache_rpc_state_p->size, HG_BULK_READ_ONLY,
        &fitcache_rpc_state_p->bulk_handle);
    assert(ret == 0);

    if (fitcache_rpc_state_p->in.offset == -1){
        // NOTE: requested_tier is a hint for future per-tier file selection.
        // Current prototype uses the already-opened fd mapping; tier selection
        // is performed by path redirection at open/close time.
        readbytes = read(fitcache_rpc_state_p->in.accessfd, fitcache_rpc_state_p->buffer, fitcache_rpc_state_p->size);
        if(DEBUG_HU)
        {
            std::string path_copy;
            {
                std::shared_lock<std::shared_mutex> rlock(cache_mtx);
                auto it = fd_to_path.find(fitcache_rpc_state_p->in.accessfd);
                if (it != fd_to_path.end())
                    path_copy = it->second;    
            }
            L4C_DEBUG("Server Rank %d : Read %ld bytes from file %s", server_rank,readbytes, path_copy.c_str());
        }
        
    }else
    {
        // See note above regarding requested_tier pseudo logic.
        readbytes = pread(fitcache_rpc_state_p->in.accessfd, fitcache_rpc_state_p->buffer, fitcache_rpc_state_p->size, fitcache_rpc_state_p->in.offset);
        if(DEBUG_HU)
        {
            L4C_DEBUG("Server Rank %d : PRead %ld bytes from file %s at offset %lld", server_rank, readbytes, fd_to_path[fitcache_rpc_state_p->in.accessfd].c_str(),(long long) fitcache_rpc_state_p->in.offset );
        }
    }

    //Reduce size of transfer to what was actually read 
    //We may need to revisit this.
    fitcache_rpc_state_p->size = readbytes;

    /* initiate bulk transfer from client to server */
    ret = HG_Bulk_transfer(hgi->context, fitcache_rpc_handler_bulk_cb, fitcache_rpc_state_p,
        HG_BULK_PUSH, hgi->addr, fitcache_rpc_state_p->in.bulk_handle, 0,
        fitcache_rpc_state_p->bulk_handle, 0, fitcache_rpc_state_p->size, HG_OP_ID_IGNORE);
    
    assert(ret == 0);
    (void) ret;

    return (hg_return_t)ret;
}

static hg_return_t
fitcache_close_rpc_handler(hg_handle_t handle)
{
    fitcache_close_in_t in;
    int ret = HG_Get_input(handle, &in);
    assert(ret == HG_SUCCESS);
    L4C_INFO("DEBUG HU: Closing File %d\n",in.fd);
    ret = close(in.fd);
    if(ret != 0) {
        L4C_ERR("Failed to close file %d, errno: %d (%s)", in.fd, errno, strerror(errno));
    }

    if (path_cache_map.find(fd_to_path[in.fd]) == path_cache_map.end())
    {
        L4C_INFO("Caching %s",fd_to_path[in.fd].c_str());
        pthread_mutex_lock(&data_mutex);
        data_queue.push(fd_to_path[in.fd]);
        pthread_cond_signal(&data_cond);
        pthread_mutex_unlock(&data_mutex);
    }   

    fd_to_path.erase(in.fd);
 
    return (hg_return_t)ret;
}

static hg_return_t
fitcache_seek_rpc_handler(hg_handle_t handle)
{
    fitcache_seek_in_t in;
    fitcache_seek_out_t out;    
    int ret = HG_Get_input(handle, &in);
    assert(ret == 0);

    out.ret = lseek64(in.fd, in.offset, in.whence);

    HG_Respond(handle,NULL,NULL,&out);

    return (hg_return_t)ret;
}

/* register this particular rpc type with Mercury */
hg_id_t
fitcache_rpc_register_server(void)
{
    hg_id_t tmp;
    // & replace HG_Register()
    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_base_rpc", fitcache_rpc_in_t, fitcache_rpc_out_t, fitcache_rpc_handler);

    return tmp;
}

hg_id_t
fitcache_open_rpc_register_server(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_open_rpc", fitcache_open_in_t, fitcache_open_out_t, fitcache_open_rpc_handler);

    return tmp;
}

hg_id_t
fitcache_close_rpc_register_server(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_close_rpc", fitcache_close_in_t, void, fitcache_close_rpc_handler);
    

    int ret =  HG_Registered_disable_response(hg_class, tmp,HG_TRUE);                        
    assert(ret == HG_SUCCESS);

    return tmp;
}

/* register this particular rpc type with Mercury */
hg_id_t
fitcache_seek_rpc_register_server(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_seek_rpc", fitcache_seek_in_t, fitcache_seek_out_t, fitcache_seek_rpc_handler);

    return tmp;
}

// RPC handler for printing server statistics
hg_return_t
fitcache_trigger_srv_print_stats_rpc_handler(hg_handle_t handle)
{
    FitCache_TIMING("HvacServer_print_stats_rpc_handler_Total");
    
    fitcache_rpc_trigger_srv_print_stats_in_t in;
    fitcache_rpc_trigger_srv_print_stats_out_t out;
    
    int ret = HG_Get_input(handle, &in);
    assert(ret == HG_SUCCESS);

    L4C_INFO("Received RPC request to print timer statistics...");

    // Print statistics to console
    fitcache::print_all_stats();
    
    // Export to file with timestamp
    char filename[256];
    time_t now = time(0);
    struct tm* timeinfo = localtime(&now);
    snprintf(filename, sizeof(filename), "fitcache_server_stats_%04d%02d%02d_%02d%02d%02d.csv",
            timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
            timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
    
    fitcache::export_all_stats_to_file(filename);
    L4C_INFO("Timer statistics exported to %s", filename);

    // Send success response
    out.status = 0;  // 0 indicates success

    HG_Respond(handle, NULL, NULL, &out);

    return HG_SUCCESS;
}

// Register RPC for triggering server to print stats
hg_id_t
fitcache_trigger_srv_print_stats_rpc_register(void)
{
    hg_id_t tmp;

    tmp = MERCURY_REGISTER(
        hg_class, "fitcache_trigger_srv_print_stats_rpc", 
        fitcache_rpc_trigger_srv_print_stats_in_t, 
        fitcache_rpc_trigger_srv_print_stats_out_t, 
        fitcache_trigger_srv_print_stats_rpc_handler);

    return tmp;
}

// Universal RPC registration functions
hg_id_t fitcache_rpc_register(void) {
    return fitcache_rpc_register_server();
}

hg_id_t fitcache_open_rpc_register(void) {
    return fitcache_open_rpc_register_server();
}

hg_id_t fitcache_close_rpc_register(void) {
    return fitcache_close_rpc_register_server();
}

hg_id_t fitcache_seek_rpc_register(void) {
    return fitcache_seek_rpc_register_server();
}