//Starting to use CPP functionality
/*
	Usage: Implement the logic of the open, read, seek and close operation from remote
*/

#include <map>
#include <string>
#include <filesystem>
#include <iostream>
#include <assert.h>
#include <fstream> 
#include <unordered_set>
#include <vector>
#include <cstdio>

#include "fitcache_timer.h"
#include "fitcache_internal.h"
#include "fitcache_logging.h"
#include "fitcache_comm.h"


#define FitCache_CLIENT 1

__thread bool tl_disable_redirect = false; 			// Thread local variable, used to close IO redirection
bool g_disable_redirect = true;						// Global variable, control all the IO redirection should be closed or not
bool g_fitcache_initialized = false;					// signal of fitcache has been initialized or not
bool g_fitcache_comm_initialized = false;				// signal of fitcache communication parts is initialized or not
bool g_mercury_init=false;							// signal of mercury is initialized or not

uint32_t g_fitcache_server_count = 0;
char *fitcache_data_dir = NULL;

pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

std::map<int,std::string> fd_map;					// Store the FD of file to the file path
std::map<int, int > fd_redir_map;					// Store the map of local FD to the remote FD

/* Devise a way to safely call this and initialize early */
static void __attribute__((constructor)) fitcache_client_init()
{	
    pthread_mutex_lock(&init_mutex);
    if (g_fitcache_initialized){ 						// if already initialized, return
        pthread_mutex_unlock(&init_mutex);
        return;
    }

    fitcache_init_logging();

    if (getenv("FitCache_SERVER_COUNT") != NULL)
    {
        g_fitcache_server_count = atoi(getenv("FitCache_SERVER_COUNT"));
        L4C_INFO("FitCache_SERVER_COUNT is set to %d", g_fitcache_server_count);
    }
    else
    {        
        L4C_FATAL("Please set enviroment variable FitCache_SERVER_COUNT\n");
        exit(-1);
    }
 	char * fitcache_data_dir_c = getenv("FitCache_DATA_DIR");
    // L4C_INFO("FitCache_DATA_DIR is set to %s", fitcache_data_dir_c);

    if (fitcache_data_dir_c != NULL)
    {
		fitcache_data_dir = (char *)malloc(strlen(fitcache_data_dir_c) + 1);
		snprintf(fitcache_data_dir, strlen(fitcache_data_dir_c) + 1, "%s", fitcache_data_dir_c);
    }

    g_fitcache_initialized = true;

    pthread_mutex_unlock(&init_mutex);
    
	g_disable_redirect = false;
}

static void __attribute((destructor)) fitcache_client_shutdown()
{
    fitcache_shutdown_comm();
}

bool fitcache_track_file(const char *path, int flags, int fd)
{     
    FitCache_TIMING("HvacClient_track_file_total");
    
	if (strstr(path, ".ports.cfg.") != NULL)
	{
		return false;
	}
	//Always back out of RDONLY
	bool tracked = false;

	if ((flags & O_ACCMODE) == O_WRONLY) {
		return false;
	}

	if ((flags & O_APPEND)) {
		return false;
	}    

	try {
		std::string ppath = std::filesystem::canonical(path).parent_path();

		// Check if current file exists in FitCache_DATA_DIR
		if (fitcache_data_dir != NULL)
		{
			std::string test = std::filesystem::canonical(fitcache_data_dir);
			// ? path should be the directory of data
			if (ppath.find(test) != std::string::npos)
			{
				L4C_INFO("Tracking used HV_DD file path %s",path);
				L4C_INFO("Tracking used HV_DD file ppath %s",ppath.c_str());
				L4C_INFO("Tracking used HV_DD file canonical of path %s",std::filesystem::canonical(path).c_str());
				fd_map[fd] = std::filesystem::canonical(path);
				tracked = true;
			}		
		}
		// & If not set the fitcache_data_dir (such like test file)
		else if (ppath == std::filesystem::current_path()) 
		{     
			L4C_INFO("Tracking used CWD file path %s",path);
			L4C_INFO("Tracking used CWD file ppath %s",ppath.c_str());
			L4C_INFO("Tracking used CWD file canonical of path %s",std::filesystem::canonical(path).c_str());
			fd_map[fd] = std::filesystem::canonical(path);
			tracked = true;
		}
	} catch (...)
	{
		//Need to do something here
	}

	// Send RPC to tell server to open file 
	if (tracked){
		if (!g_mercury_init){
			fitcache_init_comm(false);	
			/* I think I only need to do this once */
			fitcache_client_comm_register_rpc();
			g_mercury_init = true;
		}
		// Decide which server should we sent data
		int host = std::hash<std::string>{}(fd_map[fd]) % g_fitcache_server_count;
		L4C_INFO("Remote open - Host %d", host);
		fitcache_client_comm_gen_open_rpc(host, fd_map[fd], fd);

		// * Wait for file-specific operation to complete
		fitcache_client_block_for_file(fd_map[fd]);
	}

	return tracked;
}

/* Need to clean this up - in theory the RPC should time out if the request hasn't been serviced we'll go to the file-system?
 * Maybe not - we'll roll to another server.
 * For now we return true to keep the good path happy
 */
ssize_t fitcache_remote_read(int fd, void *buf, size_t count)
{
    FitCache_TIMING("HvacClient_remote_read_total");
    
	/* FitCache Code */
	/* Check the local fd - if it's tracked we pass it to the RPC function
	 * The local FD is converted to the remote FD with the buf and count
	 * We must know the remote FD to avoid collision on the remote side
	 */
	ssize_t bytes_read = -1;
	if (fitcache_file_tracked(fd)){
		int host = std::hash<std::string>{}(fd_map[fd]) % g_fitcache_server_count;			// The host is the same host when open the file in fd_map[fd]
		L4C_INFO("Remote read - Host %d", host);		
		fitcache_client_comm_gen_read_rpc(host, fd, buf, count, -1);
		bytes_read = fitcache_read_block_for_file(fd_map[fd]);   		
		return bytes_read;
	}
	/* Non-FitCache Reads come from base */
	return bytes_read;
}

/* Need to clean this up - in theory the RPC should time out if the request hasn't been serviced we'll go to the file-system?
 * Maybe not - we'll roll to another server.
 * For now we return true to keep the good path happy
 */
ssize_t fitcache_remote_pread(int fd, void *buf, size_t count, off_t offset)
{
    FitCache_TIMING("HvacClient_remote_pread_total");
    
	/* FitCache Code */
	/* Check the local fd - if it's tracked we pass it to the RPC function
	 * The local FD is converted to the remote FD with the buf and count
	 * We must know the remote FD to avoid collision on the remote side
	 */
	ssize_t bytes_read = -1;
	if (fitcache_file_tracked(fd) && fd_redir_map[fd] != 0){
		int host = std::hash<std::string>{}(fd_map[fd]) % g_fitcache_server_count;	
		L4C_INFO("Remote pread - Host %d", host);		
		fitcache_client_comm_gen_read_rpc(host, fd, buf, count, offset);
		bytes_read = fitcache_read_block_for_file(fd_map[fd]);   	
	}
	/* Non-FitCache Reads come from base */
	return bytes_read;
}

ssize_t fitcache_remote_lseek(int fd, int offset, int whence)
{
    FitCache_TIMING("HvacClient_remote_lseek_total");
    
		/* FitCache Code */
	/* Check the local fd - if it's tracked we pass it to the RPC function
	 * The local FD is converted to the remote FD with the buf and count
	 * We must know the remote FD to avoid collision on the remote side
	 */
	ssize_t bytes_read = -1;
	if (fitcache_file_tracked(fd)){
		int host = std::hash<std::string>{}(fd_map[fd]) % g_fitcache_server_count;	
		// L4C_INFO("Remote seek - Host %d", host);		
		fitcache_client_comm_gen_seek_rpc(host, fd, offset, whence);
		bytes_read = fitcache_read_block_for_file(fd_map[fd]);  // Reuse read_block for seek result		
		return bytes_read;
	}
	/* Non-FitCache Reads come from base */
	return bytes_read;
}

void fitcache_remote_close(int fd){
	if (fitcache_file_tracked(fd)){
		int host = std::hash<std::string>{}(fd_map[fd]) % g_fitcache_server_count;
		fitcache_client_comm_gen_close_rpc(host, fd);             	
	}
}

bool fitcache_file_tracked(int fd)
{
	return (fd_map.find(fd) != fd_map.end());
}


const char * fitcache_get_path(int fd)
{	
	if (fd_map.find(fd) != fd_map.end())
	{
		return fd_map[fd].c_str();
	}
	return NULL;
}

bool fitcache_remove_fd(int fd)
{
	fitcache_remote_close(fd);	
	return fd_map.erase(fd);
}