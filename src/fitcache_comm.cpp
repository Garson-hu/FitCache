/*
    fitcache_comm.cpp is mainly responsible for server-side communication initialization 
    and RPC processing. 
    It uses the Mercury library to realize high-performance RPC communication.
*/


extern "C" {
#include "fitcache_logging.h"
#include <fcntl.h>
#include <cassert>
//#include <pmi.h>
#include <unistd.h>
}

#include <sys/file.h>
#include <sys/stat.h>
#include <string>
#include <iostream>
#include <map>
#include <atomic>
#include "fitcache_comm.h"


hg_class_t *hg_class = NULL;
hg_context_t *hg_context = NULL;
std::atomic<int> fitcache_progress_thread_shutdown_flags{0};
int fitcache_server_rank = -1;
int server_rank = -1;

//Initialize communication for both the client and server
//processes
//This is based on the rpc_engine template provided by the mercury lib
void fitcache_init_comm(hg_bool_t listen)
{
    const char *info_string = "ofi+tcp;ofi_rxm://";  
    
    char *rank_str = getenv("SLURM_PROCID");
    if (rank_str == NULL) {
        L4C_FATAL("No SLURM_PROCID is set. Please ensure proper environment setup.");
        exit(EXIT_FAILURE);
    }
	server_rank = atoi(rank_str);
    L4C_INFO("SLURM node id:%s, SLURM_JOB_NODELIST=%s", getenv("SLURM_NODEID") ? getenv("SLURM_NODEID") : "NULL"\
                                                        ,getenv("SLURM_JOB_NODELIST") ? getenv("SLURM_JOB_NODELIST") : "NULL");  
    L4C_INFO("PMIX_RANK: %s Server Rank: %d \n", rank_str, server_rank);

	pthread_t fitcache_progress_tid;

    HG_Set_log_level("DEBUG");

    /* Initialize Mercury with the desired network abstraction class */
    hg_class = HG_Init(info_string, listen);
	if (hg_class == NULL){
		L4C_FATAL("Failed to initialize HG_CLASS Listen Mode : %d : PMI_RANK %d \n", listen, server_rank);
	}

    /* Create HG context */
    hg_context = HG_Context_create(hg_class);
	if (hg_context == NULL){
		L4C_FATAL("Failed to initialize HG_CONTEXT\n");
	}
	//Only for server processes
	if (listen)
	{
		if (rank_str != NULL){
			fitcache_server_rank = atoi(rank_str);
		}else
		{
			L4C_FATAL("Failed to extract rank\n");
		}
	}

	//  L4C_INFO("Mecury initialized");
    //  free(rank_str);
	//  free(pid_server);

	// ! I need to understand this better I don't want to create unecessary work for the client
    // ! For now, just create the progress thread
	if (pthread_create(&fitcache_progress_tid, NULL, fitcache_progress_fn, NULL) != 0){
		L4C_FATAL("Failed to initialized mecury progress thread\n");
	}

}

/**
 * @brief Shuts down the FitCache communication.
 *
 * This function sets the shutdown flag for the progress thread and, if the 
 * Mercury context is initialized, proceeds with shutting down the context 
 * and finalizing the Mercury class.
 */
void fitcache_shutdown_comm()
{
    fitcache_progress_thread_shutdown_flags = true;

    if (hg_context == NULL)
	return;

}


void *fitcache_progress_fn(void *args)
{
	hg_return_t ret;
	unsigned int actual_count = 0;
    // fitcache_progress_thread_shutdown_flags in initialized as 0, so always true if not invoke fitcache_shutdown_comm()
	while (!fitcache_progress_thread_shutdown_flags){
		do{
			ret = HG_Trigger(hg_context, 0, 1, &actual_count);
		} while (
			(ret == HG_SUCCESS) && actual_count && !fitcache_progress_thread_shutdown_flags);
		if (!fitcache_progress_thread_shutdown_flags)
			HG_Progress(hg_context, 100);
	}
	
	return NULL;
}

// !File lock version
void fitcache_comm_list_addr() {

    char self_addr_string[PATH_MAX];
    char filename[PATH_MAX];

    hg_addr_t self_addr;
    hg_size_t self_addr_string_size = PATH_MAX;

    char *jobid =  getenv("SLURM_JOBID");
    L4C_INFO("JOB_ID: %s\n", jobid); 
    sprintf(filename, "./.ports.cfg.%s", jobid);

    /* Get self addr to tell client about */
    HG_Addr_self(hg_class, &self_addr);
    hg_return_t tostr_ret = HG_Addr_to_string(hg_class, self_addr_string, &self_addr_string_size, self_addr);
    if (tostr_ret != HG_SUCCESS) {
        L4C_ERR("HG_Addr_to_string failed: %d", (int)tostr_ret);
        // Still proceed but write an empty address to avoid garbage
        self_addr_string[0] = '\0';
    }
    HG_Addr_free(hg_class, self_addr);

    FILE *na_config = fopen(filename, "a+");
    if (!na_config) {
        L4C_ERR("Could not open config file: %s", filename);
        exit(0);
    }

    // Obtain a lock on the file
    int fd = fileno(na_config);
    if (flock(fd, LOCK_EX) != 0) {  // Exclusive lock
        L4C_ERR("Failed to lock the file: %s", strerror(errno));
        fclose(na_config);
        exit(EXIT_FAILURE);
    }

    // Write server ID and address to the file
    L4C_INFO("Writing to config: Server ID=%d, Address=%s", fitcache_server_rank, self_addr_string);
    fprintf(na_config, "%d %s\n", fitcache_server_rank, self_addr_string);
    fflush(na_config);
    // Release the lock
    if (flock(fd, LOCK_UN) != 0) {
        L4C_ERR("Failed to unlock the file: %s", strerror(errno));
    }
    L4C_INFO("Node writing address Rank %d, Address %s", fitcache_server_rank, self_addr_string);
    fclose(na_config);
}

/* callback triggered upon completion of bulk transfer */
hg_return_t
fitcache_rpc_handler_bulk_cb(const struct hg_cb_info *info)
{
    struct fitcache_rpc_state *fitcache_rpc_state_p = (struct fitcache_rpc_state*)info->arg;
    int ret;
    fitcache_rpc_out_t out;
    out.ret = fitcache_rpc_state_p->size;
    // & infor->ret is the type of hg_return_t
    assert(info->ret == 0);

    ret = HG_Respond(fitcache_rpc_state_p->handle, NULL, NULL, &out);
    assert(ret == HG_SUCCESS);        

    HG_Bulk_free(fitcache_rpc_state_p->bulk_handle);
    // L4C_INFO("Info Server: Freeing Bulk Handle\n");
    HG_Destroy(fitcache_rpc_state_p->handle);
    free(fitcache_rpc_state_p->buffer);
    free(fitcache_rpc_state_p);
    return (hg_return_t)0;
}

/* Create context even for client */
void
fitcache_comm_create_handle(hg_addr_t addr, hg_id_t id, hg_handle_t *handle)
{    
    hg_return_t ret = HG_Create(hg_context, addr, id, handle);

    assert(ret==HG_SUCCESS);    
}

/*Free the addr */
void 
fitcache_comm_free_addr(hg_addr_t addr)
{
    hg_return_t ret = HG_Addr_free(hg_class,addr);
    assert(ret==HG_SUCCESS);
}

hg_class_t *fitcache_comm_get_class()
{
    return hg_class;
}

hg_context_t *fitcache_comm_get_context()
{
    return hg_context;
}
