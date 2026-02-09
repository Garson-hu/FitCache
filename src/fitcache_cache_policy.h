/**
 * Author: Guangxing Hu
 * Affliation: North Carolina State University
 * Email: ghu4@ncsu.edu
 * 
 * This header file declares data structures and functions
 * for managing a multi-tier caching policy:
 *   - DRAM tier (e.g., /tmp)
 *   - NVME tier (e.g., /mnt/bb/...)
 *   - Orion (Frontier) tier or Beegfs (ARC) tier
 *
 * A global or static data structure tracks metadata for each file:
 *   - Current tier (DRAM/NVME/PFS)
 *   - Access count
 *   - File size
 *   - Whether the file is closed or open, etc.
 * 
 * When space in DRAM is insufficient, a victim file can be evicted (moved from DRAM to NVME).
 */

#ifndef FitCache_CACHE_POLICY_H
#define FitCache_CACHE_POLICY_H

#include <map>
#include <string>
#include <cstdint>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Indicates the storage tier where a file resides.
 */
typedef enum {
    CACHE_TIER_DRAM = 0,             // DRAM-based tmpfs (e.g., /tmp)
    CACHE_TIER_NVME,
    CACHE_TIER_PFS,                  // Shared filesystem (e.g., Beegfs/PFS)
    CACHE_TIER_UNKNOWN               // Unknown or not found
} cache_tier_t;

/**
 * Metadata structure for each file being tracked by the caching policy.
 */
typedef struct file_meta {
    std::string   path;             // Original file path
    cache_tier_t  current_tier;     // Which tier the file is currently on
    uint64_t      size;             // File size in bytes
    uint64_t      access_count;     // Number of read accesses
    bool          is_open;          // Whether file is currently open
    // struct file_meta* next;         // Next file in the list
} file_meta_t;

/**
 * Initializes the cache policy module, sets up data structures, and configures
 * capacity for DRAM or NVME if necessary.
 * @return 0 on success, non-zero otherwise
 */
int cache_policy_init();

/**
 * Finalizes the cache policy module and frees resources.
 */
void cache_policy_finalize();

/**
 * Registers a file in the caching metadata, typically called during open or creation.
 *
 * @param path        The original file path
 * @param size_bytes  File size in bytes
 */
void cache_policy_add_file(const std::string &path, uint64_t size_bytes);

/**
 * Updates the access count for a file, typically on read or any other access.
 *
 * @param path  The original file path
 */
void cache_policy_update_access(const std::string &path);

/**
 * Updates the tier of a file (e.g., from DRAM to NVME), used after moving the file.
 *
 * @param path         The original file path
 * @param new_tier     The new tier where the file resides
 */
void cache_policy_update_tier(const std::string &path, cache_tier_t new_tier);

/**
 * Marks a file as open or closed in the metadata.
 *
 * @param path    The original file path
 * @param is_open true if file is now open, false if closed
 */
void cache_policy_set_open_state(const std::string &path, bool is_open);

/**
 * Retrieves the current tier of a given file.
 *
 * @param path The original file path
 * @return     The tier (DRAM/NVME/PFS), or CACHE_TIER_UNKNOWN if not found
 */
cache_tier_t cache_policy_get_tier(const std::string &path);

/**
 * Checks whether DRAM capacity is exceeded/or near exceed; if so, selects a victim file
 * from DRAM based on some eviction policy (for now, lowest access count) // TODO: Maybe Highest access count?
 * and returns its path. 
 *
 * This function only chooses a victim; it does not actually move the file.
 *
 * @return A victim file path to evict, or an empty string if no eviction needed or no candidates.
 */
std::string cache_policy_select_victim_for_eviction();

/**
 * Removes a file from the metadata tracking. For example, if the file is no longer needed,
 * or you want to clear the entry after eviction or unlinking the file.
 *
 * @param path The file path
 */
void cache_policy_remove_file(const std::string &path);

/**
 * Checks DRAM usage and evicts (moves) files to NVME if needed.
 * TODO: This function might enqueue a data mover task or perform the move immediately
 * depending on the design.
 *
 * @return 0 if no eviction happened or eviction succeeded, non-zero on failure
 */
int cache_policy_evict_if_needed();

/**
 * Returns the current usage (bytes) of DRAM and NVME tracked by the cache policy.
 *
 * @param used_pm_bytes   [out] current used bytes in DRAM
 * @param used_ssd_bytes  [out] current used bytes in NVME
 */
void cache_policy_get_usage_bytes(uint64_t* used_pm_bytes, uint64_t* used_ssd_bytes);

#ifdef __cplusplus
}
#endif

#endif /* FitCache_CACHE_POLICY_H */
