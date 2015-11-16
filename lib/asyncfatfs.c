/**
 * This is a FAT16/FAT32 filesystem for SD cards which uses asynchronous operations: The caller need never wait
 * for the SD card to be ready.
 *
 * On top of the regular FAT32 concepts, we add the idea of a super cluster. Given one FAT sector, a super cluster is
 * the series of clusters which corresponds to all of the cluster entries in that FAT sector. If files are allocated
 * on super-cluster boundaries, they will have FAT sectors which are dedicated to them and independent of all other
 * files.
 *
 * We can pre-allocate a "freefile" which is a file on disk made up of contiguous superclusters. Then when we want
 * to allocate a file on disk, we can carve it out of the freefile, and know that the clusters will be contiguous
 * without needing to read the FAT at all (the freefile's FAT is completely determined from its start cluster and file
 * size which we get from the directory entry). This allows for extremely fast append-only logging.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef AFATFS_DEBUG
#include <signal.h>
#include <stdio.h>
#endif

#include "asyncfatfs.h"

#include "fat_standard.h"
#include "sdcard.h"

#define AFATFS_NUM_CACHE_SECTORS 8

// FAT filesystems are allowed to differ from these parameters, but we choose not to support those weird filesystems:
#define AFATFS_SECTOR_SIZE  512
#define AFATFS_NUM_FATS     2

#define AFATFS_MAX_OPEN_FILES 3

#define AFATFS_FILES_PER_DIRECTORY_SECTOR (AFATFS_SECTOR_SIZE / sizeof(fatDirectoryEntry_t))

#define AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR  (AFATFS_SECTOR_SIZE / sizeof(uint32_t))
#define AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR (AFATFS_SECTOR_SIZE / sizeof(uint16_t))

#define AFATFS_SUBSTATE_INITIALIZATION_READ_MBR                  0
#define AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID            1
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATING         2
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH       3
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT       4
#define AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY   5

// We will read from the file
#define AFATFS_FILE_MODE_READ             1
// We will write to the file
#define AFATFS_FILE_MODE_WRITE            2
// We will append to the file, may not be combined with the write flag
#define AFATFS_FILE_MODE_APPEND           4
// File will occupy a series of superclusters (only valid for creating new files):
#define AFATFS_FILE_MODE_CONTIGUOUS       8
// File should be created if it doesn't exist:
#define AFATFS_FILE_MODE_CREATE           16
#define AFATFS_FILE_MODE_RETAIN_DIRECTORY 32

#define AFATFS_CACHE_READ         1
#define AFATFS_CACHE_WRITE        2
#define AFATFS_CACHE_LOCK         4
#define AFATFS_CACHE_UNLOCK       8
#define AFATFS_CACHE_DISCARDABLE  16
#define AFATFS_CACHE_RETAIN       32

// Turn the largest free block on the disk into one contiguous file for efficient fragment-free allocation
#define AFATFS_USE_FREEFILE

// When allocating a freefile, leave this many clusters un-allocated for regular files to use
#define AFATFS_FREEFILE_LEAVE_CLUSTERS 100

// Filename in 8.3 format:
#define AFATFS_FREESPACE_FILENAME "FREESPAC.E"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

typedef enum {
    AFATFS_CACHE_STATE_EMPTY,
    AFATFS_CACHE_STATE_READING,
    AFATFS_CACHE_STATE_IN_SYNC,
    AFATFS_CACHE_STATE_DIRTY
} afatfsCacheBlockState_e;

typedef enum {
    AFATFS_FILE_TYPE_NONE,
    AFATFS_FILE_TYPE_NORMAL,
    AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY,
    AFATFS_FILE_TYPE_DIRECTORY
} afatfsFileType_e;

typedef enum {
    CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR,
    CLUSTER_SEARCH_FREE_SECTOR,
    CLUSTER_SEARCH_OCCUPIED_SECTOR,
} afatfsClusterSearchCondition_e;

enum {
    AFATFS_CREATEFILE_PHASE_INITIAL = 0,
    AFATFS_CREATEFILE_PHASE_FIND_FILE,
    AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE,
    AFATFS_CREATEFILE_PHASE_SUCCESS,
    AFATFS_CREATEFILE_PHASE_FAILURE,
};

typedef enum {
    AFATFS_FIND_CLUSTER_IN_PROGRESS,
    AFATFS_FIND_CLUSTER_FOUND,
    AFATFS_FIND_CLUSTER_FATAL,
    AFATFS_FIND_CLUSTER_NOT_FOUND,
} afatfsFindClusterStatus_e;

struct afatfsOperation_t;

typedef union afatfsFATSector_t {
    uint8_t *bytes;
    uint16_t *fat16;
    uint32_t *fat32;
} afatfsFATSector_t;

typedef struct afatfsCacheBlockDescriptor_t {
    uint32_t sectorIndex;
    afatfsCacheBlockState_e state;
    uint32_t lastUse;

    /*
     * The state of this block must not transition (do not flush to disk, do not discard). This is useful for a sector
     * which is currently being written to by the application (so flushing it would be a waste of time).
     */
    unsigned locked:1;

    /*
     * A counter for how many parties want this sector to be retained in memory (not discarded). If this value is
     * non-zero, the sector may be flushed to disk if dirty but must remain in the cache. This is useful if we require
     * a directory sector to be cached in order to meet our response time requirements.
     */
    unsigned retainCount:6;

    /*
     * If this block is in the In Sync state, it should be discarded from the cache in preference to other blocks.
     * This is useful for data that we don't expect to read again, e.g. data written to an append-only file. This hint
     * is overridden by the locked and retainCount flags.
     */
    unsigned discardable:1;
} afatfsCacheBlockDescriptor_t;

typedef void (*afatfsCallback_t)();

typedef enum {
    AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE,
    AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE
} afatfsFreeSpaceSearchPhase_e;

typedef struct afatfsFreeSpaceSearch_t {
    uint32_t candidateStart;
    uint32_t candidateEnd;
    uint32_t bestGapStart;
    uint32_t bestGapLength;
    afatfsFreeSpaceSearchPhase_e phase;
} afatfsFreeSpaceSearch_t;

typedef struct afatfsFreeSpaceFAT_t {
    uint32_t startCluster;
    uint32_t endCluster;
} afatfsFreeSpaceFAT_t;

typedef struct afatfsCreateFile_t {
    afatfsFileCallback_t callback;

    uint8_t phase;
} afatfsCreateFile_t;

typedef struct afatfsSeek_t {
    afatfsFileCallback_t callback;

    uint32_t seekOffset;
} afatfsSeek_t;

typedef enum {
    AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT = 0,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY,
    AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
} afatfsAppendSuperclusterPhase_e;

typedef struct afatfsAppendSupercluster_t {
    afatfsAppendSuperclusterPhase_e phase;
    uint32_t previousCluster;
    uint32_t fatRewriteStartCluster;
    uint32_t fatRewriteEndCluster;
} afatfsAppendSupercluster_t;

typedef enum {
    AFATFS_APPEND_FREE_CLUSTER_PHASE_INIT = 0,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE,
    AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE,
} afatfsAppendFreeClusterPhase_e;

typedef struct afatfsAppendFreeCluster_t {
    afatfsAppendFreeClusterPhase_e phase;
    uint32_t previousCluster;
    uint32_t searchCluster;
} afatfsAppendFreeCluster_t;

typedef enum {
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL = 0,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_ADD_FREE_CLUSTER = 0,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS,
    AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE
} afatfsExtendSubdirectoryPhase_e;

typedef struct afatfsExtendSubdirectory_t {
    // We need to call this as a sub-operation so we have it as our first member to be compatible with its memory layout:
    afatfsAppendFreeCluster_t appendFreeCluster;

    afatfsExtendSubdirectoryPhase_e phase;
    uint32_t parentDirectoryCluster;
    afatfsFileCallback_t callback;
} afatfsExtendSubdirectory_t;

typedef enum {
    AFATFS_TRUNCATE_FILE_INITIAL = 0,
    AFATFS_TRUNCATE_FILE_UPDATE_DIRECTORY = 0,
    AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL,
    AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS,
    AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE,
    AFATFS_TRUNCATE_FILE_SUCCESS,
} afatfsTruncateFilePhase_e;

typedef struct afatfsTruncateFile_t {
    uint32_t startCluster; // First cluster to erase
    uint32_t endCluster; // Optional, for contiguous files set to 1 past the end cluster of the file, otherwise set to 0
    afatfsFileCallback_t callback;
    afatfsTruncateFilePhase_e phase;
} afatfsTruncateFile_t;

typedef enum {
    AFATFS_DELETE_FILE_ERASE_DIRECTORY_ENTRY,
    AFATFS_DELETE_FILE_DEALLOCATE_CLUSTERS,
} afatfsDeleteFilePhase_e;

typedef struct afatfsDeleteFile_t {
    afatfsTruncateFile_t truncateFile;
    afatfsDeleteFilePhase_e phase;
} afatfsUnlinkFile_t;

typedef enum {
    AFATFS_FILE_OPERATION_NONE,
    AFATFS_FILE_OPERATION_CREATE_FILE,
    AFATFS_FILE_OPERATION_SEEK, // Seek the file's cursorCluster forwards by seekOffset bytes
    AFATFS_FILE_OPERATION_CLOSE,
    AFATFS_FILE_OPERATION_TRUNCATE,
    AFATFS_FILE_OPERATION_UNLINK,
#ifdef AFATFS_USE_FREEFILE
    AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER,
#endif
    AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER,
    AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY,
} afatfsFileOperation_e;

typedef struct afatfsOperation_t {
    afatfsFileOperation_e operation;
    union {
        afatfsCreateFile_t createFile;
        afatfsSeek_t seek;
        afatfsAppendSupercluster_t appendSupercluster;
        afatfsAppendFreeCluster_t appendFreeCluster;
        afatfsExtendSubdirectory_t extendSubdirectory;
        afatfsUnlinkFile_t unlinkFile;
        afatfsTruncateFile_t truncateFile;
    } state;
} afatfsFileOperation_t;

typedef struct afatfsFile_t {
    afatfsFileType_e type;

    // The byte offset of the cursor within the file
    uint32_t cursorOffset;

    /*
     * The cluster that the file pointer is currently within. When seeking to the end of the file, this will be
     * set to zero.
     */
    uint32_t cursorCluster;

    /*
     * The cluster before the one the file pointer is inside. This is set to zero when at the start of the file.
     */
    uint32_t cursorPreviousCluster;

    uint8_t mode; // A combination of AFATFS_FILE_MODE_* flags

    // We hold on to one sector entry in the cache and remember its index here
    int8_t lockedCacheIndex;

    // The position of our directory entry on the disk (so we can update it without consulting a parent directory file)
    afatfsDirEntryPointer_t directoryEntryPos;
    // A copy of the file's FAT directory entry (especially fileSize and first cluster details)
    fatDirectoryEntry_t directoryEntry;

    // State for a queued operation on the file
    struct afatfsOperation_t operation;
} afatfsFile_t;

typedef struct afatfs_t {
    fatFilesystemType_e filesystemType;

    afatfsFilesystemState_e filesystemState;
    uint32_t substate;

    // State used during FS initialisation where only one member of the union is used at a time
#ifdef AFATFS_USE_FREEFILE
    union {
        afatfsFreeSpaceSearch_t freeSpaceSearch;
        afatfsFreeSpaceFAT_t freeSpaceFAT;
    } initState;
#endif

    uint8_t cache[AFATFS_SECTOR_SIZE * AFATFS_NUM_CACHE_SECTORS];
    afatfsCacheBlockDescriptor_t cacheDescriptor[AFATFS_NUM_CACHE_SECTORS];
    uint32_t cacheTimer;
    int cacheDirtyEntries; // The number of cache entries in the AFATFS_CACHE_STATE_DIRTY state

    afatfsFile_t openFiles[AFATFS_MAX_OPEN_FILES];

#ifdef AFATFS_USE_FREEFILE
    afatfsFile_t freeFile;
#endif

    bool filesystemFull;

    // The current working directory:
    afatfsFile_t currentDirectory;

    uint32_t partitionStartSector; // The physical sector that the first partition on the device begins at

    uint32_t fatStartSector; // The first sector of the first FAT
    uint32_t fatSectors;     // The size in sectors of a single FAT

    /*
     * Number of clusters available for storing user data. Note that clusters are numbered starting from 2, so the
     * index of the last cluster on the volume is numClusters + 1 !!!
     */
    uint32_t numClusters;
    uint32_t clusterStartSector;
    uint32_t sectorsPerCluster;

    /*
     * Number of the cluster we last allocated (i.e. free->occupied). Searches for a free cluster will begin after this
     * cluster.
     */
    uint32_t lastClusterAllocated;

    /* Mask to be ANDed with a byte offset within a file to give the offset within the cluster */
    uint32_t byteInClusterMask;

    uint32_t rootDirectoryCluster; // Present on FAT32 and set to zero for FAT16
    uint32_t rootDirectorySectors; // Zero on FAT32, for FAT16 the number of sectors that the root directory occupies
} afatfs_t;

typedef enum {
    AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN,
    AFATFS_FAT_PATTERN_TERMINATED_CHAIN,
    AFATFS_FAT_PATTERN_FREE
} afatfsFATPattern_e;

static afatfs_t afatfs;

static void afatfs_fileOperationContinue(afatfsFile_t *file);
static uint8_t* afatfs_fileLockCursorSectorForWrite(afatfsFilePtr_t file);
static uint8_t* afatfs_fileGetCursorSectorForRead(afatfsFilePtr_t file);

static uint32_t roundUpTo(uint32_t value, uint32_t rounding)
{
    uint32_t remainder = value % rounding;

    if (remainder > 0) {
        value += rounding - remainder;
    }

    return value;
}

static bool isPowerOfTwo(unsigned int x)
{
    return ((x != 0) && ((x & (~x + 1)) == x));
}

static bool afatfs_assert(bool condition)
{
    if (!condition) {
        afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
#ifdef AFATFS_DEBUG
        raise(SIGTRAP);
#endif
    }

    return condition;
}

static bool afatfs_fileIsBusy(afatfsFilePtr_t file)
{
    return file->operation.operation != AFATFS_FILE_OPERATION_NONE;
}

/**
 * The number of FAT table entries that fit within one AFATFS sector size.
 */
static uint32_t afatfs_fatEntriesPerSector()
{
    return afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32 ? AFATFS_FAT32_FAT_ENTRIES_PER_SECTOR : AFATFS_FAT16_FAT_ENTRIES_PER_SECTOR;
}

/**
 * Size of a FAT cluster in bytes
 */
 static uint32_t afatfs_clusterSize()
{
    return afatfs.sectorsPerCluster * AFATFS_SECTOR_SIZE;
}

/**
 * Given a byte offset within a file, return the byte offset of that position within the cluster it belongs to.
 */
static uint32_t afatfs_byteIndexInCluster(uint32_t byteOffset)
{
    return afatfs.byteInClusterMask & byteOffset;
}

/**
 * Given a byte offset within a file, return the index of the sector within the cluster it belongs to.
 */
static uint32_t afatfs_sectorIndexInCluster(uint32_t byteOffset)
{
    return afatfs_byteIndexInCluster(byteOffset) / AFATFS_SECTOR_SIZE;
}

static uint8_t *afatfs_cacheSectorGetMemory(int cacheSectorIndex)
{
    return afatfs.cache + cacheSectorIndex * AFATFS_SECTOR_SIZE;
}

static int afatfs_getCacheDescriptorIndexForBuffer(uint8_t *memory)
{
    int index = (memory - afatfs.cache) / AFATFS_SECTOR_SIZE;

    if (afatfs_assert(index >= 0 && index < AFATFS_NUM_CACHE_SECTORS)) {
        return index;
    } else {
        return -1;
    }
}

static afatfsCacheBlockDescriptor_t* afatfs_getCacheDescriptorForBuffer(uint8_t *memory)
{
    int index = afatfs_getCacheDescriptorIndexForBuffer(memory);

    if (index > -1) {
        return afatfs.cacheDescriptor + index;
    } else {
        return NULL;
    }
}

/**
 * Mark the cached sector that the given memory pointer lies inside as dirty.
 */
static void afatfs_cacheSectorMarkDirty(uint8_t *memory)
{
    afatfsCacheBlockDescriptor_t *descriptor = afatfs_getCacheDescriptorForBuffer(memory);

    if (descriptor && descriptor->state != AFATFS_CACHE_STATE_DIRTY) {
        descriptor->state = AFATFS_CACHE_STATE_DIRTY;
        afatfs.cacheDirtyEntries++;
    }
}

static void afatfs_cacheSectorInit(afatfsCacheBlockDescriptor_t *descriptor, uint32_t sectorIndex, bool locked)
{
    descriptor->lastUse = ++afatfs.cacheTimer;
    descriptor->sectorIndex = sectorIndex;
    descriptor->locked = locked;
    descriptor->discardable = 0;
    descriptor->state = AFATFS_CACHE_STATE_EMPTY;
}

/**
 * Attempt to flush the dirty cache entry with the given index to the SDcard. Returns true if the SDcard accepted
 * the write, false if the card was busy.
 */
static bool afatfs_cacheFlushSector(int cacheIndex)
{
    if (afatfs.cacheDescriptor[cacheIndex].state != AFATFS_CACHE_STATE_DIRTY)
        return true; // Already flushed

    if (sdcard_writeBlock(afatfs.cacheDescriptor[cacheIndex].sectorIndex, afatfs_cacheSectorGetMemory(cacheIndex))) {
        afatfs.cacheDescriptor[cacheIndex].state = AFATFS_CACHE_STATE_IN_SYNC;
        afatfs.cacheDirtyEntries--;
        return true;
    }
    return false;
}

static void afatfs_sdcardReadComplete(sdcardBlockOperation_e operation, uint32_t sectorIndex, uint8_t *buffer, uint32_t callbackData)
{
    (void) operation;
    (void) callbackData;

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].state != AFATFS_CACHE_STATE_EMPTY
            && afatfs.cacheDescriptor[i].sectorIndex == sectorIndex
        ) {
            afatfs_assert(afatfs_cacheSectorGetMemory(i) == buffer && afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_READING);

            afatfs.cacheDescriptor[i].state = AFATFS_CACHE_STATE_IN_SYNC;
            break;
        }
    }
}

/**
 * Find a sector in the cache which corresponds to the given physical sector index, or NULL if the sector isn't
 * cached. Note that the cached sector could be in any state including completely empty.
 */
static afatfsCacheBlockDescriptor_t* afatfs_findCacheSector(uint32_t sectorIndex)
{
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            return &afatfs.cacheDescriptor[i];
        }
    }

    return NULL;
}

/**
 * Find or allocate a cache sector for the given sector index on disk. Returns a block which matches one of these
 * conditions (in descending order of preference):
 *
 * - The requested sector that already exists in the cache
 * - The index of an empty sector
 * - The index of a synced discardable sector
 * - The index of the oldest synced sector
 *
 * Otherwise it returns -1 to signal failure (cache is full!)
 */
static int afatfs_allocateCacheSector(uint32_t sectorIndex)
{
    int allocateIndex;
    int emptyIndex = -1, discardableIndex = -1;

    uint32_t oldestSyncedSectorLastUse = 0xFFFFFFFF;
    int oldestSyncedSectorIndex = -1;

    if (
        !afatfs_assert(
            afatfs.numClusters == 0 // We're unable to check sector bounds during startup since we haven't read volume label yet
            || sectorIndex < afatfs.clusterStartSector + afatfs.numClusters * afatfs.sectorsPerCluster
        )
    ) {
        return -1;
    }

    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (afatfs.cacheDescriptor[i].sectorIndex == sectorIndex) {
            /*
             * If the sector is actually empty then do a complete re-init of it just like the standard
             * empty case. (Sectors marked as empty should be treated as if they don't have a block index assigned)
             */
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_EMPTY) {
                emptyIndex = i;
                break;
            }

            // Bump the last access time
            afatfs.cacheDescriptor[i].lastUse = ++afatfs.cacheTimer;
            return i;
        }

        switch (afatfs.cacheDescriptor[i].state) {
            case AFATFS_CACHE_STATE_EMPTY:
                emptyIndex = i;
            break;
            case AFATFS_CACHE_STATE_IN_SYNC:
                if (!afatfs.cacheDescriptor[i].locked && afatfs.cacheDescriptor[i].retainCount == 0) {
                    if (afatfs.cacheDescriptor[i].discardable) {
                        discardableIndex = i;
                    } else if (afatfs.cacheDescriptor[i].lastUse < oldestSyncedSectorLastUse) {
                        // This block could be evicted from the cache to make room for us since it's idle and not dirty
                        oldestSyncedSectorLastUse = afatfs.cacheDescriptor[i].lastUse;
                        oldestSyncedSectorIndex = i;
                    }
                }
            break;
            default:
                ;
        }
    }

    if (emptyIndex > -1) {
        allocateIndex = emptyIndex;
    } else if (discardableIndex > -1) {
        allocateIndex = discardableIndex;
    } else if (oldestSyncedSectorIndex > -1) {
        allocateIndex = oldestSyncedSectorIndex;
    } else {
        allocateIndex = -1;
    }

    if (allocateIndex > -1) {
        afatfs_cacheSectorInit(&afatfs.cacheDescriptor[allocateIndex], sectorIndex, false);
    }

    return allocateIndex;
}

/**
 * Attempt to flush dirty cache pages out to the card, returning true if all flushable data has been flushed.
 */
bool afatfs_flush()
{
    if (afatfs.cacheDirtyEntries > 0) {
        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_DIRTY && !afatfs.cacheDescriptor[i].locked) {
                afatfs_cacheFlushSector(i);

                return false;
            }
        }
    }

    return true;
}

bool afatfs_isFull()
{
    return afatfs.filesystemFull;
}

/**
 * Get the physical sector number that corresponds to the FAT sector of the given fatSectorIndex within the given
 * FAT (fatIndex may be 0 or 1). (0, 0) gives the first sector of the first FAT.
 */
static uint32_t afatfs_fatSectorToPhysical(int fatIndex, uint32_t fatSectorIndex)
{
    return afatfs.fatStartSector + (fatIndex ? afatfs.fatSectors : 0) + fatSectorIndex;
}

static uint32_t afatfs_fileClusterToPhysical(uint32_t clusterNumber, uint32_t sectorIndex)
{
    return afatfs.clusterStartSector + (clusterNumber - 2) * afatfs.sectorsPerCluster + sectorIndex;
}

static uint32_t afatfs_fileGetCursorPhysicalSector(afatfsFilePtr_t file)
{
    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        return afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors + file->cursorOffset / AFATFS_SECTOR_SIZE;
    } else {
        uint32_t cursorSectorInCluster = afatfs_sectorIndexInCluster(file->cursorOffset);
        return afatfs_fileClusterToPhysical(file->cursorCluster, cursorSectorInCluster);
    }
}

static void afatfs_fileGetCursorClusterAndSector(afatfsFilePtr_t file, uint32_t *cluster, uint16_t *sector)
{
    *cluster = file->cursorCluster;

    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        *sector = file->cursorOffset / AFATFS_SECTOR_SIZE;
    } else {
        *sector = afatfs_sectorIndexInCluster(file->cursorOffset);
    }
}

/**
 * Get a cache entry for the given sector that is suitable for write only (no read!)
 *
 * lock        - True if the sector should not be flushed to disk yet, false to clear the lock.
 * discardable - Set to true as a hint that this sector needn't be retained in cache after writing.
 */
 static afatfsOperationStatus_e afatfs_cacheSector(uint32_t physicalSectorIndex, uint8_t **buffer, uint8_t sectorFlags)
{
    // We never write to the MBR
    if (!afatfs_assert(!((sectorFlags & AFATFS_CACHE_WRITE) != 0 && physicalSectorIndex == 0))) {
        return AFATFS_OPERATION_FAILURE;
    }

    int cacheSectorIndex = afatfs_allocateCacheSector(physicalSectorIndex);

    if (cacheSectorIndex == -1) {
        // We don't have enough free cache to service this request right now, try again later
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    switch (afatfs.cacheDescriptor[cacheSectorIndex].state) {
        case AFATFS_CACHE_STATE_READING:
            return AFATFS_OPERATION_IN_PROGRESS;
        break;

        case AFATFS_CACHE_STATE_EMPTY:
            if ((sectorFlags & AFATFS_CACHE_READ) != 0) {
                if (sdcard_readBlock(physicalSectorIndex, afatfs_cacheSectorGetMemory(cacheSectorIndex), afatfs_sdcardReadComplete, 0)) {
                    afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_READING;
                }
                return AFATFS_OPERATION_IN_PROGRESS;
            }

            // We only get to decide if it is discardable if we're the ones who fill it
            afatfs.cacheDescriptor[cacheSectorIndex].discardable = (sectorFlags & AFATFS_CACHE_DISCARDABLE) != 0 ? 1 : 0;

            // Fall through

        case AFATFS_CACHE_STATE_IN_SYNC:
            if ((sectorFlags & AFATFS_CACHE_WRITE) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].state = AFATFS_CACHE_STATE_DIRTY;
                afatfs.cacheDirtyEntries++;
            }
            // Fall through

        case AFATFS_CACHE_STATE_DIRTY:
            if ((sectorFlags & AFATFS_CACHE_LOCK) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].locked = 1;
            }
            if ((sectorFlags & AFATFS_CACHE_UNLOCK) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].locked = 0;
            }
            if ((sectorFlags & AFATFS_CACHE_RETAIN) != 0) {
                afatfs.cacheDescriptor[cacheSectorIndex].retainCount++;
            }

            *buffer = afatfs_cacheSectorGetMemory(cacheSectorIndex);

            return AFATFS_OPERATION_SUCCESS;
        break;

        default:
            // Cache block in unknown state, should never happen
            afatfs_assert(false);
            return AFATFS_OPERATION_FAILURE;
    }
}

/**
 * Parse the details out of the given MBR sector (512 bytes long). Return true if a compatible filesystem was found.
 */
static bool afatfs_parseMBR(const uint8_t *sector)
{
    // Check MBR signature
    if (sector[AFATFS_SECTOR_SIZE - 2] != 0x55 || sector[AFATFS_SECTOR_SIZE - 1] != 0xAA)
        return false;

    mbrPartitionEntry_t *partition = (mbrPartitionEntry_t *) (sector + 446);

    for (int i = 0; i < 4; i++) {
        if (
            partition[i].lbaBegin > 0
            && (
                partition[i].type == MBR_PARTITION_TYPE_FAT32
                || partition[i].type == MBR_PARTITION_TYPE_FAT32_LBA
                || partition[i].type == MBR_PARTITION_TYPE_FAT16
                || partition[i].type == MBR_PARTITION_TYPE_FAT16_LBA
            )
        ) {
            afatfs.partitionStartSector = partition[i].lbaBegin;

            return true;
        }
    }

    return false;
}

static bool afatfs_parseVolumeID(const uint8_t *sector)
{
    fatVolumeID_t *volume = (fatVolumeID_t *) sector;

    if (volume->bytesPerSector != AFATFS_SECTOR_SIZE || volume->numFATs != AFATFS_NUM_FATS
            || sector[510] != FAT_VOLUME_ID_SIGNATURE_1 || sector[511] != FAT_VOLUME_ID_SIGNATURE_2) {
        return false;
    }

    afatfs.fatStartSector = afatfs.partitionStartSector + volume->reservedSectorCount;

    afatfs.sectorsPerCluster = volume->sectorsPerCluster;
    if (afatfs.sectorsPerCluster < 1 || afatfs.sectorsPerCluster > 128 || !isPowerOfTwo(afatfs.sectorsPerCluster)) {
        return false;
    }

    afatfs.byteInClusterMask = AFATFS_SECTOR_SIZE * afatfs.sectorsPerCluster - 1;

    afatfs.fatSectors = volume->FATSize16 != 0 ? volume->FATSize16 : volume->fatDescriptor.fat32.FATSize32;

    // Always zero on FAT32 since rootEntryCount is always zero (this is non-zero on FAT16)
    afatfs.rootDirectorySectors = ((volume->rootEntryCount * FAT_DIRECTORY_ENTRY_SIZE) + (volume->bytesPerSector - 1)) / volume->bytesPerSector;
    uint32_t totalSectors = volume->totalSectors16 != 0 ? volume->totalSectors16 : volume->totalSectors32;
    uint32_t dataSectors = totalSectors - (volume->reservedSectorCount + (AFATFS_NUM_FATS * afatfs.fatSectors) + afatfs.rootDirectorySectors);

    afatfs.numClusters = dataSectors / volume->sectorsPerCluster;

    if (afatfs.numClusters <= FAT12_MAX_CLUSTERS) {
        afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT12;
        afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;

        return false; // FAT12 is not a supported filesystem
    } else if (afatfs.numClusters <= FAT16_MAX_CLUSTERS) {
        afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT16;
    } else {
        afatfs.filesystemType = FAT_FILESYSTEM_TYPE_FAT32;
    }

    uint32_t endOfFATs = afatfs.fatStartSector + AFATFS_NUM_FATS * afatfs.fatSectors;

    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32) {
        afatfs.rootDirectoryCluster = volume->fatDescriptor.fat32.rootCluster;
    } else {
        // FAT16 doesn't store the root directory in clusters
        afatfs.rootDirectoryCluster = 0;
    }

    afatfs.clusterStartSector = endOfFATs + afatfs.rootDirectorySectors;

    afatfs_chdir(NULL);

    return true;
}

/**
 * Get the position of the dirEntryPos in the FAT for the cluster with the given number.
 */
static void afatfs_getFATPositionForCluster(uint32_t cluster, uint32_t *fatSectorIndex, uint32_t *fatSectorEntryIndex)
{
    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
        // There are AFATFS_SECTOR_SIZE / sizeof(uint16_t) entries per FAT16 sector
        *fatSectorIndex = (cluster & 0x0FFFFFFF) >> 8;
        *fatSectorEntryIndex = cluster & 0xFF;
    } else {
        // There are AFATFS_SECTOR_SIZE / sizeof(uint32_t) entries per FAT32 sector
        *fatSectorIndex = (cluster & 0x0FFFFFFF) >> 7;
        *fatSectorEntryIndex = cluster & 0x7F;
    }
}

static bool afatfs_FATIsEndOfChainMarker(uint32_t clusterNumber)
{
    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT32) {
        return fat32_isEndOfChainMarker(clusterNumber);
    } else {
        return fat16_isEndOfChainMarker(clusterNumber);
    }
}

/**
 * Look up the FAT to find out which cluster follows the one with the given number and store it into *nextCluster.
 *
 * Use fat_isFreeSpace() and fat_isEndOfChainMarker() on nextCluster to distinguish those special values from regular
 * cluster numbers.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - FS is busy right now, call again later
 *     AFATFS_OPERATION_SUCCESS     - *nextCluster is set to the next cluster number
 */
static afatfsOperationStatus_e afatfs_FATGetNextCluster(int fatIndex, uint32_t cluster, uint32_t *nextCluster)
{
    uint32_t fatSectorIndex, fatSectorEntryIndex;
    afatfsFATSector_t sector;

    afatfs_getFATPositionForCluster(cluster, &fatSectorIndex, &fatSectorEntryIndex);

    afatfsOperationStatus_e result = afatfs_cacheSector(afatfs_fatSectorToPhysical(fatIndex, fatSectorIndex), &sector.bytes, AFATFS_CACHE_READ);

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            *nextCluster = sector.fat16[fatSectorEntryIndex];
        } else {
            *nextCluster = fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex]);
        }
    }

    return result;
}

/**
 * Set the cluster number that follows the given cluster. Pass 0xFFFFFFFF for nextCluster to terminate the FAT chain.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS - On success
 *     AFATFS_OPERATION_IN_PROGRESS - Card is busy, call again later
 *     AFATFS_OPERATION_FAILURE - When the filesystem encounters a fatal error
 */
static afatfsOperationStatus_e afatfs_FATSetNextCluster(uint32_t startCluster, uint32_t nextCluster)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, fatSectorEntryIndex, fatPhysicalSector;
    afatfsOperationStatus_e result;

    afatfs_getFATPositionForCluster(startCluster, &fatSectorIndex, &fatSectorEntryIndex);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    result = afatfs_cacheSector(fatPhysicalSector, &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE);

    if (result == AFATFS_OPERATION_SUCCESS) {
        if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
            sector.fat16[fatSectorEntryIndex] = nextCluster;
        } else {
            sector.fat32[fatSectorEntryIndex] = nextCluster;
        }
    }

    return result;
}

static void afatfs_fileUnlockCacheSector(afatfsFilePtr_t file)
{
    if (file->lockedCacheIndex != -1) {
        afatfs.cacheDescriptor[file->lockedCacheIndex].locked = 0;
        file->lockedCacheIndex = -1;
    }
}

/**
 * Starting from and including the given cluster number, find the number of the first cluster which matches the given
 * condition.
 *
 * searchLimit - One past the final cluster that should be examined. To search the entire volume, pass:
 *                   afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER
 *
 * Condition:
 *     CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR - Find a cluster marked as free in the FAT which lies at the
 *         beginning of its FAT sector. The passed initial search 'cluster' must correspond to the first entry of a FAT sector.
 *     CLUSTER_SEARCH_FREE_SECTOR     - Find a cluster marked as free in the FAT
 *     CLUSTER_SEARCH_OCCUPIED_SECTOR - Find a cluster marked as occupied in the FAT.
 *
 * Returns:
 *     AFATFS_FIND_CLUSTER_FOUND       - When a cluster matching the criteria was found and stored in *cluster
 *     AFATFS_FIND_CLUSTER_IN_PROGRESS - When the search is not over, call this routine again later with the updated *cluster value to resume
 *     AFATFS_FIND_CLUSTER_FATAL       - An unexpected read error occurred, the volume should be abandoned
 *     AFATFS_FIND_CLUSTER_NOT_FOUND   - When the entire device was searched without finding a suitable cluster (the
 *                                    *cluster points to just beyond the final cluster).
 */
static afatfsFindClusterStatus_e afatfs_findClusterWithCondition(afatfsClusterSearchCondition_e condition, uint32_t *cluster, uint32_t searchLimit)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, fatSectorEntryIndex;

    uint32_t fatEntriesPerSector = afatfs_fatEntriesPerSector();
    bool lookingForFree = condition == CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR || condition == CLUSTER_SEARCH_FREE_SECTOR;

    int jump;

    // Get the FAT entry which corresponds to this cluster so we can begin our search there
    afatfs_getFATPositionForCluster(*cluster, &fatSectorIndex, &fatSectorEntryIndex);

    switch (condition) {
        case CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR:
            jump = fatEntriesPerSector;

            // We're supposed to call this routine with the cluster properly aligned
            if (!afatfs_assert(fatSectorEntryIndex == 0)) {
                return AFATFS_FIND_CLUSTER_FATAL;
            }
        break;
        case CLUSTER_SEARCH_OCCUPIED_SECTOR:
        case CLUSTER_SEARCH_FREE_SECTOR:
            jump = 1;
        break;
    }

    while (*cluster < searchLimit) {

#ifdef AFATFS_USE_FREEFILE
        // If we're looking inside the freefile, we won't find any free clusters! Skip it!
        if (afatfs.freeFile.directoryEntry.fileSize > 0 &&
                *cluster == (uint32_t) ((afatfs.freeFile.directoryEntry.firstClusterHigh << 16) + afatfs.freeFile.directoryEntry.firstClusterLow)) {
            *cluster += (afatfs.freeFile.directoryEntry.fileSize + afatfs_clusterSize() - 1) / afatfs_clusterSize();

            // Maintain alignment
            *cluster = roundUpTo(*cluster, jump);
            continue; // Go back to check that the new cluster number is within the volume
        }
#endif

        afatfsOperationStatus_e status = afatfs_cacheSector(afatfs_fatSectorToPhysical(0, fatSectorIndex), &sector.bytes, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE);

        switch (status) {
            case AFATFS_OPERATION_SUCCESS:
                do {
                    uint32_t clusterNumber;

                    switch (afatfs.filesystemType) {
                        case FAT_FILESYSTEM_TYPE_FAT16:
                            clusterNumber = sector.fat16[fatSectorEntryIndex];
                        break;
                        case FAT_FILESYSTEM_TYPE_FAT32:
                            clusterNumber = fat32_decodeClusterNumber(sector.fat32[fatSectorEntryIndex]);
                        break;
                        default:
                            return AFATFS_FIND_CLUSTER_FATAL;
                    }

                    if (fat_isFreeSpace(clusterNumber) == lookingForFree) {
                        /*
                         * The final FAT sector's clusters may not all be valid ones, so we need to check the cluster
                         * number again here
                         */
                        if (*cluster < searchLimit) {
                            return AFATFS_FIND_CLUSTER_FOUND;
                        } else {
                            *cluster = searchLimit;
                            return AFATFS_FIND_CLUSTER_NOT_FOUND;
                        }
                    }

                    (*cluster) += jump;
                    fatSectorEntryIndex += jump;
                } while (fatSectorEntryIndex < fatEntriesPerSector);

                // Move on to the next FAT sector
                fatSectorIndex++;
                fatSectorEntryIndex = 0;
            break;
            case AFATFS_OPERATION_FAILURE:
                return AFATFS_FIND_CLUSTER_FATAL;
            break;
            case AFATFS_OPERATION_IN_PROGRESS:
                return AFATFS_FIND_CLUSTER_IN_PROGRESS;
            break;
        }
    }

    // We looked at every available cluster and didn't find one matching the condition
    *cluster = searchLimit;
    return AFATFS_FIND_CLUSTER_NOT_FOUND;
}

static afatfsOperationStatus_e afatfs_fileGetNextCluster(afatfsFilePtr_t file, uint32_t currentCluster, uint32_t *nextCluster)
{
#ifndef AFATFS_USE_FREEFILE
    (void) file;
#else
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        uint32_t freeFileStart = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;

        // Would the next cluster lie outside the allocated file? (i.e. inside the freefile)
        if (currentCluster + 1 == freeFileStart) {
            *nextCluster = 0;
        } else {
            *nextCluster = currentCluster + 1;
        }

        return AFATFS_OPERATION_SUCCESS;
    } else
#endif
    {
        return afatfs_FATGetNextCluster(0, currentCluster, nextCluster);
    }
}

#ifdef AFATFS_USE_FREEFILE

/**
 * Update the FAT to fill the contiguous series of clusters with indexes [*startCluster...endCluster) with the
 * specified pattern.
 *
 * AFATFS_FAT_PATTERN_TERMINATED_CHAIN - Chain the clusters together in linear sequence and terminate the final cluster
 * AFATFS_FAT_PATTERN_CHAIN - Chain the clusters together without terminating the final entry
 * AFATFS_FAT_PATTERN_FREE  - Mark the clusters as free space
 *
 * Returns -
 *     AFATFS_OPERATION_SUCCESS     - When the entire chain has been written
 *     AFATFS_OPERATION_IN_PROGRESS - Call again later with the updated *startCluster value in order to resume writing.
 */
static afatfsOperationStatus_e afatfs_FATFillWithPattern(afatfsFATPattern_e pattern, uint32_t *startCluster, uint32_t endCluster)
{
    afatfsFATSector_t sector;
    uint32_t fatSectorIndex, firstEntryIndex, fatPhysicalSector;
    uint8_t fatEntrySize;
    uint32_t nextCluster;
    afatfsOperationStatus_e result;

    // Find the position of the initial cluster to begin our fill
    afatfs_getFATPositionForCluster(*startCluster, &fatSectorIndex, &firstEntryIndex);

    fatPhysicalSector = afatfs_fatSectorToPhysical(0, fatSectorIndex);

    while (*startCluster < endCluster) {
        // One past the last entry we will fill inside this sector:
        uint32_t lastEntryIndex = MIN(firstEntryIndex + (endCluster - *startCluster), afatfs_fatEntriesPerSector());

        uint8_t cacheFlags = AFATFS_CACHE_WRITE | AFATFS_CACHE_DISCARDABLE;

        if (firstEntryIndex > 0 || lastEntryIndex < afatfs_fatEntriesPerSector()) {
            // We're not overwriting the entire FAT sector so we must read the existing contents
            cacheFlags |= AFATFS_CACHE_READ;
        }

        result = afatfs_cacheSector(fatPhysicalSector, &sector.bytes, cacheFlags);

        if (result != AFATFS_OPERATION_SUCCESS)
            return result;

        switch (pattern) {
            case AFATFS_FAT_PATTERN_TERMINATED_CHAIN:
            case AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN:
                nextCluster = *startCluster + 1;
                // Write all the "next cluster" pointers
                if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                    for (uint32_t i = firstEntryIndex; i < lastEntryIndex; i++, nextCluster++) {
                        sector.fat16[i] = nextCluster;
                    }
                } else {
                    for (uint32_t i = firstEntryIndex; i < lastEntryIndex; i++, nextCluster++) {
                        sector.fat32[i] = nextCluster;
                    }
                }

                *startCluster += lastEntryIndex - firstEntryIndex;

                if (pattern == AFATFS_FAT_PATTERN_TERMINATED_CHAIN && *startCluster == endCluster) {
                // We completed the chain! Overwrite the last entry we wrote with the terminator for the end of the chain
                    if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16) {
                        sector.fat16[lastEntryIndex - 1] = 0xFFFF;
                    } else {
                        sector.fat32[lastEntryIndex - 1] = 0xFFFFFFFF;
                    }
                    break;
                }
            break;
            case AFATFS_FAT_PATTERN_FREE:
                fatEntrySize = afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16 ? sizeof(uint16_t) : sizeof(uint32_t);

                memset(sector.bytes, 0, (lastEntryIndex - firstEntryIndex) * fatEntrySize);

                *startCluster += lastEntryIndex - firstEntryIndex;
            break;
        }

        fatPhysicalSector++;
        firstEntryIndex = 0;
    }

    return AFATFS_OPERATION_SUCCESS;
}

#endif

/**
 * Write the directory entry for the file into its `directoryEntryPos` position in its containing directory.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS - The directory entry has been stored into the directory sector in cache.
 *     AFATFS_OPERATION_IN_PROGRESS - Cache is too busy, retry later
 *     AFATFS_OPERATION_FAILURE - If the filesystem enters the fatal state
 */
static afatfsOperationStatus_e afatfs_saveDirectoryEntry(afatfsFilePtr_t file)
{
    uint8_t *sector;
    afatfsOperationStatus_e result;

    result = afatfs_cacheSector(file->directoryEntryPos.sectorNumberPhysical, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_WRITE);

    if (result == AFATFS_OPERATION_SUCCESS) {
        // (sub)directories don't store a filesize in their directory entry:
        if (file->type == AFATFS_FILE_TYPE_DIRECTORY) {
            file->directoryEntry.fileSize = 0;
        }

        if (afatfs_assert(file->directoryEntryPos.entryIndex >= 0)) {
            memcpy(sector + file->directoryEntryPos.entryIndex * FAT_DIRECTORY_ENTRY_SIZE, &file->directoryEntry, FAT_DIRECTORY_ENTRY_SIZE);
        } else {
            return AFATFS_OPERATION_FAILURE;
        }
    }

    return result;
}

/**
 * Attempt to add a free cluster to the end of the given file. If the file was previously empty, the directory entry
 * is updated to point to the new cluster.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The cluster has been appended
 *     AFATFS_OPERATION_IN_PROGRESS - Cache was busy, so call again later to continue
 *     AFATFS_OPERATION_FAILURE     - Cluster could not be appended because the filesystem ran out of space
 *                                    (afatfs.filesystemFull is set to true)
 *
 * Note that the file's current operation is not changed by this routine, so if SUCCESS/FAILURE is returned you might
 * want to set the operation back to AFATFS_FILE_OPERATION_NONE if the sole thing you were working on was an append
 * (this is because you might be working on a different operation of which the append is just a sub-operation).
 */
static afatfsOperationStatus_e afatfs_appendRegularFreeClusterContinue(afatfsFile_t *file)
{
    afatfsAppendFreeCluster_t *opState = &file->operation.state.appendFreeCluster;
    afatfsOperationStatus_e status;

    doMore:

    switch (opState->phase) {
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_INIT:
            opState->searchCluster = afatfs.lastClusterAllocated;

            opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE;
            goto doMore;
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FIND_FREESPACE:
            switch (afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE_SECTOR, &opState->searchCluster, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
                case AFATFS_FIND_CLUSTER_FOUND:
                    afatfs.lastClusterAllocated = opState->searchCluster;

                    // Make the cluster available for us to write in
                    file->cursorCluster = opState->searchCluster;

                    if (opState->previousCluster == 0) {
                        // This is the new first cluster in the file so we also need to update the directory entry
                        file->directoryEntry.firstClusterHigh = opState->searchCluster >> 16;
                        file->directoryEntry.firstClusterLow = opState->searchCluster & 0xFFFF;

                    }

                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1;
                    goto doMore;
                break;
                case AFATFS_FIND_CLUSTER_FATAL:
                case AFATFS_FIND_CLUSTER_NOT_FOUND:
                    // We couldn't find an empty cluster to append to the file
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE;
                    goto doMore;
                break;
                case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                break;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT1:
            // Terminate the new cluster
            status = afatfs_FATSetNextCluster(opState->searchCluster, 0xFFFFFFFF);

            if (status == AFATFS_OPERATION_SUCCESS) {
                // Is the new cluster the first cluster in the file?
                if (opState->previousCluster == 0) {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
                } else {
                    opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2;
                }
                goto doMore;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
            if (afatfs_saveDirectoryEntry(file) == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_UPDATE_FAT2:
            // Add the new cluster to the pre-existing chain
            status = afatfs_FATSetNextCluster(opState->previousCluster, opState->searchCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_COMPLETE:
            return AFATFS_OPERATION_SUCCESS;
        break;
        case AFATFS_APPEND_FREE_CLUSTER_PHASE_FAILURE:
            afatfs.filesystemFull = true;
            return AFATFS_OPERATION_FAILURE;
        break;
    }

    return AFATFS_OPERATION_IN_PROGRESS;
}

static void afatfs_appendRegularFreeClusterInitOperationState(afatfsAppendFreeCluster_t *state, uint32_t previousCluster)
{
    state->phase = AFATFS_APPEND_FREE_CLUSTER_PHASE_INIT;
    state->previousCluster = previousCluster;
}

/**
 * Queue up an operation to append a free cluster to the file and update the file's cursorCluster to point to it.
 *
 * You must seek to the end of the file first, so file.cursorCluster will be 0 for the first call, and
 * `file.cursorPreviousCluster` will be the cluster to append after.
 *
 * Note that the cursorCluster will be updated before this operation is completely finished (i.e. before the FAT is
 * updated) but you can go ahead and write to it before the operation succeeds.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The append completed successfully
 *     AFATFS_OPERATION_IN_PROGRESS - The operation was queued on the file and will complete later
 *     AFATFS_OPERATION_FAILURE     - Operation could not be queued or append failed, check afatfs.fileSystemFull
 */
static afatfsOperationStatus_e afatfs_appendRegularFreeCluster(afatfsFilePtr_t file)
{
    if (file->operation.operation == AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER)
        return AFATFS_OPERATION_IN_PROGRESS;

    if (afatfs.filesystemFull || afatfs_fileIsBusy(file)) {
        return AFATFS_OPERATION_FAILURE;
    }

    file->operation.operation = AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER;

    afatfs_appendRegularFreeClusterInitOperationState(&file->operation.state.appendFreeCluster, file->cursorPreviousCluster);

    afatfsOperationStatus_e status = afatfs_appendRegularFreeClusterContinue(file);

    if (status != AFATFS_OPERATION_IN_PROGRESS) {
        // Operation is over (for better or worse)
        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    }

    return status;
}

#ifdef AFATFS_USE_FREEFILE

/**
 * Size of a AFATFS supercluster in bytes
 */
static uint32_t afatfs_superClusterSize()
{
    return afatfs_fatEntriesPerSector() * afatfs_clusterSize();
}

/**
 * Continue to attempt to add a supercluster to the end of the given file.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - On completion
 *     AFATFS_OPERATION_IN_PROGRESS - Operation still in progress
 */
static afatfsOperationStatus_e afatfs_appendSuperclusterContinue(afatfsFile_t *file)
{
    afatfsAppendSupercluster_t *opState = &file->operation.state.appendSupercluster;

    afatfsOperationStatus_e status;
    uint32_t freeFileStartCluster;

    doMore:

    switch (opState->phase) {
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT:
            // Our file steals the first cluster of the freefile
            freeFileStartCluster = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;

            // The new supercluster needs to have its clusters chained contiguously and marked with a terminator at the end
            opState->fatRewriteStartCluster = freeFileStartCluster;
            opState->fatRewriteEndCluster = freeFileStartCluster + afatfs_fatEntriesPerSector();

            if (opState->previousCluster == 0) {
                // This is the new first cluster in the file so we need to update the directory entry
                file->directoryEntry.firstClusterHigh = afatfs.freeFile.directoryEntry.firstClusterHigh;
                file->directoryEntry.firstClusterLow = afatfs.freeFile.directoryEntry.firstClusterLow;
            } else {
                /*
                 * We also need to update the FAT of the supercluster that used to end the file so that it no longer
                 * terminates there
                 */
                opState->fatRewriteStartCluster -= afatfs_fatEntriesPerSector();
            }

            // Remove the first supercluster from the freefile
            afatfs.freeFile.directoryEntry.fileSize -= afatfs_superClusterSize();

            uint32_t newFreeFileStartCluster;

            /* Even if the freefile becomes empty, we still don't set its first cluster to zero. This is so that
             * afatfs_fileGetNextCluster() can tell where a contiguous file ends (at the start of the freefile).
             *
             * Note that normally the freefile can't become empty because it is allocated as a non-integer number
             * of superclusters to avoid precisely this situation.
             */
            newFreeFileStartCluster = freeFileStartCluster + afatfs_fatEntriesPerSector();

            afatfs.freeFile.directoryEntry.firstClusterLow = newFreeFileStartCluster;
            afatfs.freeFile.directoryEntry.firstClusterHigh = newFreeFileStartCluster >> 16;

            opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT;
            goto doMore;
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FAT:
            status = afatfs_FATFillWithPattern(AFATFS_FAT_PATTERN_TERMINATED_CHAIN, &opState->fatRewriteStartCluster, opState->fatRewriteEndCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY;
                goto doMore;
            }
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FREEFILE_DIRECTORY:
            status = afatfs_saveDirectoryEntry(&afatfs.freeFile);

            if (status == AFATFS_OPERATION_SUCCESS) {
                if (opState->previousCluster == 0) {
                    // Need to write the new first-cluster to the file's directory entry
                    opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY;
                    goto doMore;
                } else {
                    return AFATFS_OPERATION_SUCCESS;
                }
            }
        break;
        case AFATFS_APPEND_SUPERCLUSTER_PHASE_UPDATE_FILE_DIRECTORY:
            status = afatfs_saveDirectoryEntry(file);
        break;
    }

    return status;
}

/**
 * Attempt to queue up an operation to append the first supercluster of the freefile to the given `file` which presently
 * ends at `previousCluster`. The new cluster number will be set into the file's cursorCluster.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The append completed successfully and the file's cursorCluster has been updated
 *     AFATFS_OPERATION_IN_PROGRESS - The operation was queued on the file and will complete later, or there is already an
 *                                    append in progress.
 *     AFATFS_OPERATION_FAILURE     - Operation could not be queued (file was busy) or append failed (filesystem is full).
 *                                    Check afatfs.fileSystemFull
 */
static afatfsOperationStatus_e afatfs_appendSupercluster(afatfsFilePtr_t file, uint32_t previousCluster)
{
    uint32_t superClusterSize = afatfs_superClusterSize();
    afatfsOperationStatus_e status;

    if (file->operation.operation == AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER)
        return AFATFS_OPERATION_IN_PROGRESS;

    if (afatfs.freeFile.directoryEntry.fileSize < superClusterSize) {
        afatfs.filesystemFull = true;
    }

    if (afatfs.filesystemFull || afatfs_fileIsBusy(file)) {
        return AFATFS_OPERATION_FAILURE;
    }

    afatfsAppendSupercluster_t *opState = &file->operation.state.appendSupercluster;

    file->operation.operation = AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER;
    opState->phase = AFATFS_APPEND_SUPERCLUSTER_PHASE_INIT;
    opState->previousCluster = previousCluster;

    /*
     * We can go ahead and write to that space before the FAT and directory are updated by the
     * queued operation:
     */
    file->cursorCluster = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;

    status = afatfs_appendSuperclusterContinue(file);

    if (status != AFATFS_OPERATION_IN_PROGRESS) {
        // The operation completed already
        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    }

    return status;
}

#endif

static afatfsOperationStatus_e afatfs_appendFreeCluster(afatfsFilePtr_t file)
{
    afatfsOperationStatus_e status;

#ifdef AFATFS_USE_FREEFILE
    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        // Steal the first cluster from the beginning of the freefile if we can
        status = afatfs_appendSupercluster(file, file->cursorPreviousCluster);
    } else
#endif
    {
        status = afatfs_appendRegularFreeCluster(file);
    }

    return status;
}

static bool afatfs_isEndOfAllocatedFile(afatfsFilePtr_t file)
{
    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        return file->cursorOffset >= AFATFS_SECTOR_SIZE * afatfs.rootDirectorySectors;
    } else {
        return file->cursorCluster == 0 || afatfs_FATIsEndOfChainMarker(file->cursorCluster);
    }
}

static uint8_t* afatfs_fileGetCursorSectorForRead(afatfsFilePtr_t file)
{
    uint8_t *result;

    uint32_t physicalSector = afatfs_fileGetCursorPhysicalSector(file);

    if (file->lockedCacheIndex != -1) {
        if (!afatfs_assert(physicalSector == afatfs.cacheDescriptor[file->lockedCacheIndex].sectorIndex)) {
            return NULL;
        }

        result = afatfs_cacheSectorGetMemory(file->lockedCacheIndex);
    } else {
        if (afatfs_isEndOfAllocatedFile(file)) {
            return NULL;
        }

        afatfs_assert(physicalSector > 0); // We never read the root sector using files

        afatfsOperationStatus_e status = afatfs_cacheSector(
            physicalSector,
            &result,
            AFATFS_CACHE_READ
        );

        if (status != AFATFS_OPERATION_SUCCESS) {
            // Sector not ready for read
            return NULL;
        }

        file->lockedCacheIndex = afatfs_getCacheDescriptorIndexForBuffer(result);
    }

    return result;
}

/**
 * Attempt to seek the file pointer by the offset.
 *
 * Returns true if the seek was completed, or false if you should try again later by calling this routine again (the
 * cursor is not moved and no seek operation is queued on the file for you).
 *
 * You can only seek fowards by the size of a cluster or less, or backwards to stay within the same cluster. Otherwise
 * false will always be returned (calling this routine again will never make progress on the seek).
 *
 * This amount of seek is special because we will have to wait on at most one read operation, so it's easy to make
 * the seek atomic.
 */
static bool afatfs_fseekAtomic(afatfsFilePtr_t file, int32_t offset)
{
    // Seeks within a sector
    uint32_t newSectorOffset = offset + file->cursorOffset % AFATFS_SECTOR_SIZE;

    // i.e. offset is non-negative and smaller than AFATFS_SECTOR_SIZE
    if (newSectorOffset < AFATFS_SECTOR_SIZE) {
        file->cursorOffset += offset;
        return true;
    }

    // We're seeking outside the sector so unlock it if we were holding it
    afatfs_fileUnlockCacheSector(file);

    // FAT16 root directories are made up of contiguous sectors rather than clusters
    if (file->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY) {
        file->cursorOffset += offset;

        return true;
    }

    uint32_t clusterSizeBytes = afatfs_clusterSize();
    uint32_t offsetInCluster = afatfs_byteIndexInCluster(file->cursorOffset);
    uint32_t newOffsetInCluster = offsetInCluster + offset;

    afatfsOperationStatus_e status;

    if (offset > (int32_t) clusterSizeBytes || offset < -(int32_t) offsetInCluster) {
        return false;
    }

    // Are we seeking outside the cluster? If so we'll need to find out the next cluster number
    if (newOffsetInCluster >= clusterSizeBytes) {
        uint32_t nextCluster;

        status = afatfs_fileGetNextCluster(file, file->cursorCluster, &nextCluster);

        if (status == AFATFS_OPERATION_SUCCESS) {
            // Seek to the beginning of the next cluster
            uint32_t bytesToSeek = clusterSizeBytes - offsetInCluster;

            file->cursorPreviousCluster = file->cursorCluster;
            file->cursorCluster = nextCluster;
            file->cursorOffset += bytesToSeek;

            offset -= bytesToSeek;
        } else {
            // Try again later
            return false;
        }
    }

    // If we didn't already hit the end of the file, add any remaining offset needed inside the cluster
    if (!afatfs_isEndOfAllocatedFile(file)) {
        file->cursorOffset += offset;
    }

    return true;
}

static bool afatfs_fseekInternalContinue(afatfsFile_t *file)
{
    afatfsSeek_t *opState = &file->operation.state.seek;
    uint32_t clusterSizeBytes = afatfs_clusterSize();
    uint32_t offsetInCluster = afatfs_byteIndexInCluster(file->cursorOffset);

    afatfsOperationStatus_e status;

    // Keep advancing the cursor cluster forwards to consume seekOffset
    while (offsetInCluster + opState->seekOffset >= clusterSizeBytes && !afatfs_isEndOfAllocatedFile(file)) {
        uint32_t nextCluster;

        status = afatfs_fileGetNextCluster(file, file->cursorCluster, &nextCluster);

        if (status == AFATFS_OPERATION_SUCCESS) {
            // Seek to the beginning of the next cluster
            uint32_t bytesToSeek = clusterSizeBytes - offsetInCluster;

            file->cursorPreviousCluster = file->cursorCluster;
            file->cursorCluster = nextCluster;

            file->cursorOffset += bytesToSeek;
            opState->seekOffset -= bytesToSeek;
            offsetInCluster = 0;
        } else {
            // Try again later
            return false;
        }
    }

    // If we didn't already hit the end of the file, add any remaining offset needed inside the cluster
    if (!afatfs_isEndOfAllocatedFile(file)) {
        file->cursorOffset += opState->seekOffset;
    }

    file->directoryEntry.fileSize = MAX(file->directoryEntry.fileSize, file->cursorOffset);

    file->operation.operation = AFATFS_FILE_OPERATION_NONE;

    if (opState->callback) {
        opState->callback(file);
    }

    return true;
}

/**
 * Seek the file pointer forwards by offset bytes.
 */
static afatfsOperationStatus_e afatfs_fseekInternal(afatfsFilePtr_t file, uint32_t offset, afatfsFileCallback_t callback)
{
    // See if we can seek without queuing an operation
    if (afatfs_fseekAtomic(file, offset)) {
        if (callback) {
            callback(file);
        }

        return AFATFS_OPERATION_SUCCESS;
    } else {
        // Our operation must queue
        if (afatfs_fileIsBusy(file)) {
            return AFATFS_OPERATION_FAILURE;
        }

        afatfsSeek_t *opState = &file->operation.state.seek;

        file->operation.operation = AFATFS_FILE_OPERATION_SEEK;
        opState->callback = callback;
        opState->seekOffset = offset;

        return AFATFS_OPERATION_IN_PROGRESS;
    }
}

/**
 * Attempt to seek the file cursor from the given point (`whence`) by the given offset, just like C's fseek().
 *
 * AFATFS_SEEK_SET with offset 0 will always be successful.
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS     - The seek was completed immediately
 *     AFATFS_OPERATION_IN_PROGRESS - The seek was queued and will complete later
 *     AFATFS_OPERATION_FAILURE     - The seek could not be queued because the file was busy with another operation,
 *                                    try again later.
 */
afatfsOperationStatus_e afatfs_fseek(afatfsFilePtr_t file, int32_t offset, afatfsSeek_e whence)
{
    switch (whence) {
        case AFATFS_SEEK_CUR:
            if (offset >= 0) {
                // Only forwards seeks are supported by this routine:
                return afatfs_fseekInternal(file, offset, NULL);
            }

            // Convert a backwards relative seek into a SEEK_SET. TODO considerable room for improvement if within the same cluster
            offset += file->cursorOffset;
        break;

        case AFATFS_SEEK_END:
            // Convert into a SEEK_SET
            offset += file->directoryEntry.fileSize;
        break;

        case AFATFS_SEEK_SET:
            ;
            // Fall through
    }

    // Now we have a SEEK_SET with a positive offset. Begin by seeking to the start of the file
    afatfs_fileUnlockCacheSector(file);

    file->cursorPreviousCluster = 0;
    file->cursorCluster = (file->directoryEntry.firstClusterHigh << 16) | file->directoryEntry.firstClusterLow;
    file->cursorOffset = 0;

    // Then seek forwards by the offset
    return afatfs_fseekInternal(file, offset, NULL);
}

/**
 * Get the byte-offset in the file of the file's cursor.
 *
 * Returns true on success, or false if the file is busy (try again later).
 */
bool afatfs_ftell(afatfsFilePtr_t file, uint32_t *position)
{
    if (afatfs_fileIsBusy(file)) {
        return false;
    } else {
        *position = file->cursorOffset;
        return true;
    }
}

/**
 * Attempt to advance the directory pointer `finder` to the next entry in the directory, and if the directory is
 * not finished, the *entry pointer is updated to point inside the cached FAT sector at the position of the
 * fatDirectoryEntry_t. This cache could evaporate soon, so copy the entry away if you need it!
 *
 * Returns:
 *     AFATFS_OPERATION_SUCCESS -     A pointer to the next directory entry has been loaded into *dirEntry. If the
 *                                    directory was exhausted then *dirEntry will be set to NULL.
 *     AFATFS_OPERATION_IN_PROGRESS - The disk is busy. The pointer is not advanced, call again later to retry.
 */
afatfsOperationStatus_e afatfs_findNext(afatfsFilePtr_t directory, afatfsFinder_t *finder, fatDirectoryEntry_t **dirEntry)
{
    uint8_t *sector;

    if (finder->entryIndex == AFATFS_FILES_PER_DIRECTORY_SECTOR - 1) {
        if (afatfs_fseekAtomic(directory, AFATFS_SECTOR_SIZE)) {
            finder->entryIndex = -1;
            // Fall through to read the first entry of that new sector
        } else {
            return AFATFS_OPERATION_IN_PROGRESS;
        }
    }

    sector = afatfs_fileGetCursorSectorForRead(directory);

    if (sector) {
        finder->entryIndex++;

        *dirEntry = (fatDirectoryEntry_t*) sector + finder->entryIndex;

        finder->sectorNumberPhysical = afatfs_fileGetCursorPhysicalSector(directory);

        return AFATFS_OPERATION_SUCCESS;
    } else {
        if (afatfs_isEndOfAllocatedFile(directory)) {
            *dirEntry = NULL;

            return AFATFS_OPERATION_SUCCESS;
        }

        return AFATFS_OPERATION_IN_PROGRESS;
    }
}

/**
 * Initialise the finder so that the first call with the directory to findNext() will return the first file in the
 * directory.
 */
void afatfs_findFirst(afatfsFilePtr_t directory, afatfsFinder_t *finder)
{
    afatfs_fseek(directory, 0, AFATFS_SEEK_SET);
    finder->entryIndex = -1;
}

static afatfsOperationStatus_e afatfs_extendSubdirectoryContinue(afatfsFile_t *directory)
{
    afatfsExtendSubdirectory_t *opState = &directory->operation.state.extendSubdirectory;
    afatfsOperationStatus_e status;
    uint8_t *sectorBuffer;
    uint32_t clusterNumber, physicalSector;
    uint16_t sectorInCluster;

    doMore:
    switch (opState->phase) {
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_ADD_FREE_CLUSTER:
            status = afatfs_appendRegularFreeClusterContinue(directory);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE;
                goto doMore;
            }
        break;
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_WRITE_SECTORS:
            // Now, zero out that cluster
            afatfs_fileGetCursorClusterAndSector(directory, &clusterNumber, &sectorInCluster);
            physicalSector = afatfs_fileGetCursorPhysicalSector(directory);

            while (1) {
                status = afatfs_cacheSector(physicalSector, &sectorBuffer, AFATFS_CACHE_WRITE);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }

                memset(sectorBuffer, 0, AFATFS_SECTOR_SIZE);

                // If this is the first sector of the directory, create the "." and ".." entries
                if (directory->cursorOffset == 0) {
                    fatDirectoryEntry_t *dirEntries = (fatDirectoryEntry_t *) sectorBuffer;

                    memset(dirEntries[0].filename, ' ', sizeof(dirEntries[0].filename));
                    dirEntries[0].filename[0] = '.';
                    dirEntries[0].firstClusterHigh = directory->directoryEntry.firstClusterHigh;
                    dirEntries[0].firstClusterLow = directory->directoryEntry.firstClusterLow;
                    dirEntries[0].attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;

                    memset(dirEntries[1].filename, ' ', sizeof(dirEntries[1].filename));
                    dirEntries[1].filename[0] = '.';
                    dirEntries[1].filename[1] = '.';
                    dirEntries[1].firstClusterHigh = opState->parentDirectoryCluster >> 16;
                    dirEntries[1].firstClusterLow = opState->parentDirectoryCluster & 0xFFFF;
                    dirEntries[1].attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;
                }

                if (sectorInCluster < afatfs.sectorsPerCluster - 1) {
                    // Move to next sector
                    afatfs_assert(afatfs_fseekAtomic(directory, AFATFS_SECTOR_SIZE));
                    sectorInCluster++;
                    physicalSector++;
                } else {
                    break;
                }
            }

            // Seek back to the beginning of the cluster
            afatfs_assert(afatfs_fseekAtomic(directory, -AFATFS_SECTOR_SIZE * (afatfs.sectorsPerCluster - 1)));
            opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS;
            goto doMore;
        break;
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_SUCCESS:
            directory->operation.operation = AFATFS_FILE_OPERATION_NONE;

            if (opState->callback) {
                opState->callback(directory);
            }

            return AFATFS_OPERATION_SUCCESS;
        break;
        case AFATFS_EXTEND_SUBDIRECTORY_PHASE_FAILURE:
            directory->operation.operation = AFATFS_FILE_OPERATION_NONE;

            if (opState->callback) {
                opState->callback(NULL);
            }
            return AFATFS_OPERATION_FAILURE;
        break;
    }

    return AFATFS_OPERATION_IN_PROGRESS;
}

/**
 * Queue an operation to add a cluster to a sub-directory (either zero-filling it, or adding "." and ".." entries
 * depending if it is the new first cluster).
 *
 * The directory must not be busy, otherwise AFATFS_OPERATION_FAILURE is returned immediately.
 *
 * The directory's cursor must lie at the end of the directory file (i.e. isEndOfAllocatedFile() would return true).
 *
 * You must provide parentDirectory if this is the first extension to the subdirectory, otherwise pass NULL for that argument.
 */
static afatfsOperationStatus_e afatfs_extendSubdirectory(afatfsFile_t *directory, afatfsFilePtr_t parentDirectory, afatfsFileCallback_t callback)
{
    // FAT16 root directories cannot be extended
    if (directory->type == AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY || afatfs_fileIsBusy(directory)) {
        return AFATFS_OPERATION_FAILURE;
    }

    /*
     * We'll assume that we're never asked to append the first cluster of a root directory, since any
     * reasonably-formatted volume should have a root!
     */
    afatfsExtendSubdirectory_t *opState = &directory->operation.state.extendSubdirectory;

    directory->operation.operation = AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY;

    opState->phase = AFATFS_EXTEND_SUBDIRECTORY_PHASE_INITIAL;
    opState->parentDirectoryCluster = parentDirectory ? (parentDirectory->directoryEntry.firstClusterHigh << 16) | parentDirectory->directoryEntry.firstClusterLow : 0;
    opState->callback = callback;

    afatfs_appendRegularFreeClusterInitOperationState(&opState->appendFreeCluster, directory->cursorPreviousCluster);

    return afatfs_extendSubdirectoryContinue(directory);
}

/**
 * Allocate space for a new directory entry to be written, store the position of that entry in the finder, and set
 * the *dirEntry pointer to point to the entry within the cached FAT sector. This pointer's lifetime is only as good
 * as the life of the cache, so don't dawdle.
 *
 * Before the first call to this function, call afatfs_findFirst() on the directory.
 *
 * The FAT sector in the cache is marked as dirty, so any changes written through to the entry will be flushed out
 * in the next poll cycle.
 */
static afatfsOperationStatus_e afatfs_allocateDirectoryEntry(afatfsFilePtr_t directory, fatDirectoryEntry_t **dirEntry, afatfsFinder_t *finder)
{
    afatfsOperationStatus_e result;

    if (afatfs_fileIsBusy(directory)) {
        return AFATFS_OPERATION_IN_PROGRESS;
    }

    while ((result = afatfs_findNext(directory, finder, dirEntry)) == AFATFS_OPERATION_SUCCESS) {
        if (*dirEntry) {
            if (fat_isDirectoryEntryEmpty(*dirEntry) || fat_isDirectoryEntryTerminator(*dirEntry)) {
                afatfs_cacheSectorMarkDirty((uint8_t*) *dirEntry);

                return AFATFS_OPERATION_SUCCESS;
            }
        } else {
            // Need to extend directory size by adding a cluster
            result = afatfs_extendSubdirectory(directory, NULL, NULL);

            if (result == AFATFS_OPERATION_SUCCESS) {
                // Continue the search in the newly-extended directory
                continue;
            } else {
                // The status (in progress or failure) of extending the directory becomes our status
                break;
            }
        }
    }

    return result;
}

/**
 * Return a pointer to an entry in the open files table whose type is not "NONE". You should initialize the file
 * afterwards with afatfs_initFileHandle().
 */
static afatfsFilePtr_t afatfs_allocateFileHandle()
{
    for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        if (afatfs.openFiles[i].type == AFATFS_FILE_TYPE_NONE) {
            return &afatfs.openFiles[i];
        }
    }

    return NULL;
}

/**
 * Continue the file truncation.
 *
 * When truncation finally succeeds or fails, the current operation is cleared on the file (if the current operation
 * was a truncate), then the operation callback is called. This allows the truncation to be called as a sub-operation
 * without it clearing the parent file operation.
 */
static afatfsOperationStatus_e afatfs_ftruncateContinue(afatfsFilePtr_t file)
{
    afatfsTruncateFile_t *opState = &file->operation.state.truncateFile;
    afatfsOperationStatus_e status;
    uint32_t oldFreeFileStart;

    doMore:

    switch (opState->phase) {
        case AFATFS_TRUNCATE_FILE_UPDATE_DIRECTORY:
            status = afatfs_saveDirectoryEntry(file);

            if (status == AFATFS_OPERATION_SUCCESS) {
                if (opState->endCluster) {
                    opState->phase = AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS;
                } else {
                    opState->phase = AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL;
                }
                goto doMore;
            }
        break;
        case AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_CONTIGUOUS:
            // Prepare the clusters to be added back on to the beginning of the freefile
            status = afatfs_FATFillWithPattern(AFATFS_FAT_PATTERN_UNTERMINATED_CHAIN, &opState->startCluster, opState->endCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE;
                goto doMore;
            }
        break;
        case AFATFS_TRUNCATE_FILE_PREPEND_TO_FREEFILE:
            // Note, it's okay to run this code several times:
            oldFreeFileStart = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;

            afatfs.freeFile.directoryEntry.firstClusterHigh = opState->startCluster >> 16;
            afatfs.freeFile.directoryEntry.firstClusterLow = opState->startCluster & 0xFFFF;
            afatfs.freeFile.directoryEntry.fileSize += (oldFreeFileStart - opState->startCluster) * afatfs_clusterSize();

            status = afatfs_saveDirectoryEntry(&afatfs.freeFile);
            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_TRUNCATE_FILE_SUCCESS;
                goto doMore;
            }
        break;
        case AFATFS_TRUNCATE_FILE_ERASE_FAT_CHAIN_NORMAL:
            while (!afatfs_FATIsEndOfChainMarker(opState->startCluster)) {
                uint32_t nextCluster;

                status = afatfs_FATGetNextCluster(0, opState->startCluster, &nextCluster);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }

                status = afatfs_FATSetNextCluster(opState->startCluster, 0);

                if (status != AFATFS_OPERATION_SUCCESS) {
                    return status;
                }

                opState->startCluster = nextCluster;
            }

            opState->phase = AFATFS_TRUNCATE_FILE_SUCCESS;
            goto doMore;
        break;
        case AFATFS_TRUNCATE_FILE_SUCCESS:
            if (file->operation.operation == AFATFS_FILE_OPERATION_TRUNCATE) {
                file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            }

            if (opState->callback) {
                opState->callback(file);
            }

            return AFATFS_OPERATION_SUCCESS;
        break;
    }

    if (status == AFATFS_OPERATION_FAILURE && file->operation.operation == AFATFS_FILE_OPERATION_TRUNCATE) {
        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    }

    return status;
}

/**
 * Truncate the file to zero bytes in length.
 *
 * Returns true if the operation was successfully queued or false if the file is busy (try again later).
 */
bool afatfs_ftruncate(afatfsFilePtr_t file, afatfsFileCallback_t callback)
{
    afatfsTruncateFile_t *opState;

    if (afatfs_fileIsBusy(file))
        return false;

    afatfs_fseek(file, 0, AFATFS_SEEK_SET);

    file->operation.operation = AFATFS_FILE_OPERATION_TRUNCATE;

    opState = &file->operation.state.truncateFile;
    opState->callback = callback;
    opState->phase = AFATFS_TRUNCATE_FILE_INITIAL;
    opState->startCluster = (file->directoryEntry.firstClusterHigh << 16) | file->directoryEntry.firstClusterLow;

    if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
        // The file is contiguous and ends where the freefile begins
        opState->endCluster = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;
    } else {
        // The range of clusters to delete is not contiguous, so follow it as a linked-list instead
        opState->endCluster = 0;
    }

    file->directoryEntry.firstClusterHigh = 0;
    file->directoryEntry.firstClusterLow = 0;
    file->directoryEntry.fileSize = 0;

    return true;
}

static void afatfs_createFileContinue(afatfsFile_t *file)
{
    afatfsCreateFile_t *opState = &file->operation.state.createFile;
    fatDirectoryEntry_t *entry;
    afatfsOperationStatus_e status;

    doMore:

    switch (opState->phase) {
        case AFATFS_CREATEFILE_PHASE_INITIAL:
            afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);
            opState->phase = AFATFS_CREATEFILE_PHASE_FIND_FILE;
            goto doMore;
        break;
        case AFATFS_CREATEFILE_PHASE_FIND_FILE:
            do {
                status = afatfs_findNext(&afatfs.currentDirectory, &file->directoryEntryPos, &entry);

                if (status == AFATFS_OPERATION_SUCCESS) {
                    if (entry == NULL || fat_isDirectoryEntryTerminator(entry)) {
                        if ((file->mode & AFATFS_FILE_MODE_CREATE) != 0) {
                            // The file didn't already exist, so we can create it. Allocate a new directory entry
                            afatfs_findFirst(&afatfs.currentDirectory, &file->directoryEntryPos);

                            opState->phase = AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE;
                            goto doMore;
                        } else {
                            // File not found.
                            opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                            goto doMore;
                        }
                    } else if (strncmp(entry->filename, (char*) file->directoryEntry.filename, FAT_FILENAME_LENGTH) == 0) {
                        // We found a file with this name!
                        memcpy(&file->directoryEntry, entry, sizeof(file->directoryEntry));

                        opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                        goto doMore;
                    }
                } else if (status == AFATFS_OPERATION_FAILURE) {
                    opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                    goto doMore;
                }
            } while (status == AFATFS_OPERATION_SUCCESS);
        break;
        case AFATFS_CREATEFILE_PHASE_CREATE_NEW_FILE:
            status = afatfs_allocateDirectoryEntry(&afatfs.currentDirectory, &entry, &file->directoryEntryPos);

            if (status == AFATFS_OPERATION_SUCCESS) {
                memcpy(entry, &file->directoryEntry, sizeof(file->directoryEntry));

                opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                opState->phase = AFATFS_CREATEFILE_PHASE_FAILURE;
                goto doMore;
            }
        break;
        case AFATFS_CREATEFILE_PHASE_SUCCESS:
            if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
                /*
                 * For this high performance file type, we require the directory entry for the file to be retained
                 * in the cache at all times.
                 */
                uint8_t *directorySector;

                status = afatfs_cacheSector(
                    file->directoryEntryPos.sectorNumberPhysical,
                    &directorySector,
                    AFATFS_CACHE_READ | AFATFS_CACHE_RETAIN
                );

                if (status != AFATFS_OPERATION_SUCCESS) {
                    // Retry next time
                    break;
                }
            }

            afatfs_fseek(file, 0, AFATFS_SEEK_SET);

            // Is file empty?
            if (file->cursorCluster == 0) {
                if (file->type == AFATFS_FILE_TYPE_DIRECTORY && (file->mode & AFATFS_FILE_MODE_WRITE) != 0) {
                    // Opening an empty directory. Append an empty cluster to it. This replaces our open file operation
                    file->operation.operation = AFATFS_FILE_OPERATION_NONE;
                    afatfs_extendSubdirectory(file, &afatfs.currentDirectory, opState->callback);
                    break;
                }
            } else {
                // We can't guarantee that the existing file contents are contiguous
                file->mode &= ~AFATFS_FILE_MODE_CONTIGUOUS;

                // Seek to the end of the file if it is in append mode
                if ((file->mode & AFATFS_FILE_MODE_APPEND) != 0) {
                    // This replaces our open file operation
                    file->operation.operation = AFATFS_FILE_OPERATION_NONE;
                    afatfs_fseekInternal(file, file->directoryEntry.fileSize, opState->callback);
                    break;
                }

                // If we're only writing (not reading) the file must be truncated
                if (file->mode == (AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_WRITE)) {
                    // This replaces our open file operation
                    file->operation.operation = AFATFS_FILE_OPERATION_NONE;
                    afatfs_ftruncate(file, opState->callback);
                    break;
                }
            }

            file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            opState->callback(file);
        break;
        case AFATFS_CREATEFILE_PHASE_FAILURE:
            file->type = AFATFS_FILE_TYPE_NONE;
            opState->callback(NULL);
        break;
    }
}

/**
 * Reset the in-memory data for the given handle back to the zeroed initial state
 */
static void afatfs_initFileHandle(afatfsFilePtr_t file)
{
    memset(file, 0, sizeof(*file));
    file->lockedCacheIndex = -1;
}

static void afatfs_funlinkContinue(afatfsFilePtr_t file)
{
    afatfsUnlinkFile_t *opState = &file->operation.state.unlinkFile;
    afatfsOperationStatus_e status;

    doMore:
    switch (opState->phase) {
        case AFATFS_DELETE_FILE_DEALLOCATE_CLUSTERS:
            status = afatfs_ftruncateContinue(file);

            if (status == AFATFS_OPERATION_SUCCESS) {
                opState->phase = AFATFS_DELETE_FILE_ERASE_DIRECTORY_ENTRY;
                goto doMore;
            }
        break;
        case AFATFS_DELETE_FILE_ERASE_DIRECTORY_ENTRY:
            memset(&file->directoryEntry, 0, sizeof(file->directoryEntry));

            // Replace the unlink operation with a file close (that'll flush the zeroed directory entry out to disk)
            file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            afatfs_fclose(file);
        break;
    }
}

bool afatfs_funlink(afatfsFilePtr_t file)
{
    if (afatfs_fileIsBusy(file))
        return false;

    afatfs_fseek(file, 0, AFATFS_SEEK_SET);

    afatfsUnlinkFile_t *opState = &file->operation.state.unlinkFile;
    file->operation.operation = AFATFS_FILE_OPERATION_UNLINK;

    uint32_t fileStartCluster = (file->directoryEntry.firstClusterHigh << 16) | file->directoryEntry.firstClusterLow;

    if (fileStartCluster == 0) {
        opState->phase = AFATFS_DELETE_FILE_ERASE_DIRECTORY_ENTRY;
    } else {
        opState->phase = AFATFS_DELETE_FILE_DEALLOCATE_CLUSTERS;

        opState->truncateFile.phase = AFATFS_TRUNCATE_FILE_INITIAL;
        opState->truncateFile.callback = NULL;
        opState->truncateFile.startCluster = fileStartCluster;
        if ((file->mode & AFATFS_FILE_MODE_CONTIGUOUS) != 0) {
            opState->truncateFile.endCluster = (afatfs.freeFile.directoryEntry.firstClusterHigh << 16) | afatfs.freeFile.directoryEntry.firstClusterLow;
        } else {
            opState->truncateFile.endCluster = 0;
        }
    }

    afatfs_funlinkContinue(file);

    return true;
}

/**
 * Open (or create) a file in the CWD with the given filename.
 *
 * file             - Memory location to store the newly opened file details
 * name             - Filename in "name.ext" format. No path.
 * attrib           - FAT file attributes to give the file (if created)
 * fileMode         - Bitset of AFATFS_FILE_MODE_* constants. Include AFATFS_FILE_MODE_CREATE to create the file if
 *                    it does not exist.
 * callback         - Called when the operation is complete
 */
static afatfsFilePtr_t afatfs_createFile(afatfsFilePtr_t file, const char *name, uint8_t attrib, uint8_t fileMode,
        afatfsFileCallback_t callback)
{
    afatfsCreateFile_t *opState;

    afatfs_initFileHandle(file);

    file->mode = fileMode;

    if (strcmp(name, ".") == 0) {
        memcpy(&file->directoryEntry, &afatfs.currentDirectory.directoryEntry, sizeof(afatfs.currentDirectory.directoryEntry));

        file->type = afatfs.currentDirectory.type;
    } else {
        fat_convertFilenameToFATStyle(name, (uint8_t*)file->directoryEntry.filename);
        file->directoryEntry.attrib = attrib;

        if ((attrib & FAT_FILE_ATTRIBUTE_DIRECTORY) != 0) {
            file->type = AFATFS_FILE_TYPE_DIRECTORY;
        } else {
            file->type = AFATFS_FILE_TYPE_NORMAL;
        }
    }

    // Queue the operation to finish the file creation
    file->operation.operation = AFATFS_FILE_OPERATION_CREATE_FILE;

    opState = &file->operation.state.createFile;
    opState->callback = callback;

    if (strcmp(name, ".") == 0) {
        // Since we already have the directory entry details, we can skip straight to the final operations requried
        opState->phase = AFATFS_CREATEFILE_PHASE_SUCCESS;
    } else {
        opState->phase = AFATFS_CREATEFILE_PHASE_INITIAL;
    }

    afatfs_createFileContinue(file);

    return file;
}

static void afatfs_closeFileContinue(afatfsFilePtr_t file)
{
    afatfsCacheBlockDescriptor_t *descriptor;

    /*
     * Directories don't update their parent directory entries over time, because their fileSize field in the directory
     * never changes (when we add the first clsuter to the directory we save the directory entry at that point and it
     * doesn't change afterwards).
     *
     * So don't bother trying to save their directory entries during fclose().
     */
    if (file->type == AFATFS_FILE_TYPE_DIRECTORY || afatfs_saveDirectoryEntry(file) == AFATFS_OPERATION_SUCCESS) {
        // Release our reservation on the directory cache if needed
        if ((file->mode & AFATFS_FILE_MODE_RETAIN_DIRECTORY) != 0) {
            descriptor = afatfs_findCacheSector(file->directoryEntryPos.sectorNumberPhysical);

            if (descriptor) {
                descriptor->retainCount = MAX(descriptor->retainCount - 1, 0);
            }
        }

        afatfs_fileUnlockCacheSector(file);

        file->type = AFATFS_FILE_TYPE_NONE;
        file->operation.operation = AFATFS_FILE_OPERATION_NONE;
    }
}

/**
 * Returns true if an operation was successfully queued to close the file and destroy the file handle. If this function
 * returns true then you should not make any further calls to the file (the handle might be reused for a new file).
 */
bool afatfs_fclose(afatfsFilePtr_t file)
{
    if (!file || file->type == AFATFS_FILE_TYPE_NONE) {
        return true;
    } else if (afatfs_fileIsBusy(file)) {
        return false;
    } else {
        file->operation.operation = AFATFS_FILE_OPERATION_CLOSE;
        afatfs_closeFileContinue(file);
        return true;
    }
}

afatfsFilePtr_t afatfs_mkdir(const char *filename, afatfsFileCallback_t callback)
{
    afatfsFilePtr_t file = afatfs_allocateFileHandle();

    if (file) {
        afatfs_createFile(file, filename, FAT_FILE_ATTRIBUTE_DIRECTORY, AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_READ | AFATFS_FILE_MODE_WRITE, callback);
    }

    return file;
}

/**
 * Change the working directory to the directory with the given handle (use fopen). Pass NULL for filename in order to
 * change to the root directory.
 *
 * Returns true on success, false if you should call again later to retry. After changing into a directory, your handle
 * to that directory may be closed by fclose().
 */
bool afatfs_chdir(afatfsFilePtr_t dirHandle)
{
    if (dirHandle) {
        if (!afatfs_fileIsBusy(dirHandle)) {
            memcpy(&afatfs.currentDirectory, dirHandle, sizeof(*dirHandle));
            return true;
        } else {
            return false;
        }
    } else {
        afatfs_initFileHandle(&afatfs.currentDirectory);

        afatfs.currentDirectory.mode = AFATFS_FILE_MODE_READ | AFATFS_FILE_MODE_WRITE;
        
        if (afatfs.filesystemType == FAT_FILESYSTEM_TYPE_FAT16)
            afatfs.currentDirectory.type = AFATFS_FILE_TYPE_FAT16_ROOT_DIRECTORY;
        else
            afatfs.currentDirectory.type = AFATFS_FILE_TYPE_DIRECTORY;

        afatfs.currentDirectory.directoryEntry.firstClusterHigh = afatfs.rootDirectoryCluster >> 16;
        afatfs.currentDirectory.directoryEntry.firstClusterLow = afatfs.rootDirectoryCluster & 0xFFFF;
        afatfs.currentDirectory.directoryEntry.attrib = FAT_FILE_ATTRIBUTE_DIRECTORY;

        afatfs_fseek(&afatfs.currentDirectory, 0, AFATFS_SEEK_SET);

        return true;
    }
}

/**
 * Begin the process of opening a file with the given name in the current working directory (paths in the filename are
 * not supported) using the given mode.
 *
 * To open the current working directory, pass "." for filename.
 *
 * The complete() callback is called when finished with either a file handle (file was opened) or NULL upon failure.
 *
 * Supported file mode strings:
 *
 * r - Read from an existing file
 * w - Create a file for write access, if the file already exists then erase it TODO
 * a - Create a file for write access to the end of the file only, if the file already exists then append to it
 *
 * r+ - Read and write from an existing file
 * w+ - Read and write from an existing file, if the file doesn't already exist it is created
 * a+ - Read from or append to an existing file, if the file doesn't already exist it is created TODO
 *
 * as - Create a new file which is stored contiguously on disk (high performance mode/freefile) for append or write
 * ws   If the file is already non-empty or freefile support is not compiled in then it will fall back to non-contiguous
 *      operation.
 *
 * All other mode strings are illegal.
 *
 * Returns false if the the open failed really early (out of file handles).
 */
bool afatfs_fopen(const char *filename, const char *mode, afatfsFileCallback_t complete)
{
    uint8_t fileMode = 0;
    afatfsFilePtr_t file;

    switch (mode[0]) {
        case 'r':
            fileMode = AFATFS_FILE_MODE_READ;
        break;
        case 'w':
            fileMode = AFATFS_FILE_MODE_WRITE | AFATFS_FILE_MODE_CREATE;
        break;
        case 'a':
            fileMode = AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_CREATE;
        break;
    }

    switch (mode[1]) {
        case '+':
            fileMode |= AFATFS_FILE_MODE_READ;

            if (fileMode == AFATFS_FILE_MODE_READ) {
                fileMode |= AFATFS_FILE_MODE_WRITE;
            }
        break;
        case 's':
#ifdef AFATFS_USE_FREEFILE
            fileMode |= AFATFS_FILE_MODE_CONTIGUOUS | AFATFS_FILE_MODE_RETAIN_DIRECTORY;
#endif
        break;
    }

    file = afatfs_allocateFileHandle();

    if (file) {
        afatfs_createFile(file, filename, FAT_FILE_ATTRIBUTE_ARCHIVE, fileMode, complete);
    }

    return file != NULL;
}

/**
 * Get a reference to the cache sector at the file cursor position for write.
 */
static uint8_t* afatfs_fileLockCursorSectorForWrite(afatfsFilePtr_t file)
{
    afatfsOperationStatus_e status;
    uint8_t *result;


    if (file->lockedCacheIndex != -1) {
        uint32_t physicalSector = afatfs_fileGetCursorPhysicalSector(file);

        if (!afatfs_assert(physicalSector == afatfs.cacheDescriptor[file->lockedCacheIndex].sectorIndex)) {
            return NULL;
        }

        result = afatfs_cacheSectorGetMemory(file->lockedCacheIndex);
    } else {
        // Find / allocate a sector and lock it in the cache so we can rely on it sticking around
        if (afatfs_isEndOfAllocatedFile(file)) {
            ;
        }
        // Are we at the start of an empty file or the end of a non-empty file? If so we need to add a cluster
        if (afatfs_isEndOfAllocatedFile(file) && afatfs_appendFreeCluster(file) != AFATFS_OPERATION_SUCCESS) {
            // The extension of the file is in progress so please call us again later to try again
            return NULL;
        }

        uint32_t physicalSector = afatfs_fileGetCursorPhysicalSector(file);
        uint8_t cacheFlags = AFATFS_CACHE_WRITE | AFATFS_CACHE_LOCK;
        uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;

        /*
         * If there is data before the write point, or there could be data after the write-point
         * then we need to have the original contents of the sector in the cache for us to merge into
         */
        if (
            cursorOffsetInSector > 0
            || (file->cursorOffset & ~(AFATFS_SECTOR_SIZE - 1)) + AFATFS_SECTOR_SIZE < file->directoryEntry.fileSize
        ) {
            cacheFlags |= AFATFS_CACHE_READ;
        }

        status = afatfs_cacheSector(
            physicalSector,
            &result,
            cacheFlags
        );

        if (status != AFATFS_OPERATION_SUCCESS) {
            // Not enough cache available to accept this write / sector not ready for read
            return NULL;
        }

        file->lockedCacheIndex = afatfs_getCacheDescriptorIndexForBuffer(result);
    }

    return result;
}

void afatfs_fputc(afatfsFilePtr_t file, uint8_t c)
{
    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
    int cacheIndex = file->lockedCacheIndex;

    if (cacheIndex != -1 && cursorOffsetInSector != AFATFS_SECTOR_SIZE - 1) {
        afatfs_cacheSectorGetMemory(cacheIndex)[cursorOffsetInSector] = c;
        file->cursorOffset++;
        file->directoryEntry.fileSize = MAX(file->directoryEntry.fileSize, file->cursorOffset);
    } else {
        // Slow path
        afatfs_fwrite(file, &c, sizeof(c));
    }
}

/**
 * Attempt to write `len` bytes from `buffer` into the `file`.
 *
 * Returns the number of bytes actually written.
 *
 * 0 will be returned when:
 *     The filesystem is busy (try again later)
 *     You tried to extend the length of the file but the filesystem is full (check afatfs_isFull()).
 *
 * Fewer bytes will be written than requested when:
 *     The write spanned a sector boundary and the next sector's contents/location was not yet available in the cache.
 */
uint32_t afatfs_fwrite(afatfsFilePtr_t file, const uint8_t *buffer, uint32_t len)
{
    if ((file->mode & (AFATFS_FILE_MODE_APPEND | AFATFS_FILE_MODE_WRITE)) == 0) {
        return 0;
    }

    if (afatfs_fileIsBusy(file)) {
        // There might be a seek pending
        return 0;
    }

    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;
    uint32_t writtenBytes = 0;

    while (len > 0) {
        uint32_t bytesToWriteThisSector = MIN(AFATFS_SECTOR_SIZE - cursorOffsetInSector, len);
        uint8_t *sectorBuffer;

        sectorBuffer = afatfs_fileLockCursorSectorForWrite(file);
        if (!sectorBuffer) {
            // Cache is currently busy
            break;
        }

        memcpy(sectorBuffer + cursorOffsetInSector, buffer, bytesToWriteThisSector);

        writtenBytes += bytesToWriteThisSector;

        /*
         * If the seek doesn't complete immediately then we'll break and wait for that seek to complete by waiting for
         * the file to be non-busy on entry again.
         *
         * A seek operation should always be able to queue on the file since we have checked that the file wasn't busy
         * on entry (fseek will never return AFATFS_OPERATION_FAILURE).
         *
         * If the seek has to queue, when the seek completes, it'll update the fileSize for us to contain the cursor.
         */
        if (afatfs_fseek(file, bytesToWriteThisSector, AFATFS_SEEK_CUR) == AFATFS_OPERATION_IN_PROGRESS) {
            break;
        }

        len -= bytesToWriteThisSector;
        buffer += bytesToWriteThisSector;
        cursorOffsetInSector = 0;
    }

    file->directoryEntry.fileSize = MAX(file->directoryEntry.fileSize, file->cursorOffset);

    return writtenBytes;
}

/**
 * Attempt to read `len` bytes from `file` into the `buffer`.
 *
 * Returns the number of bytes actually read.
 *
 * 0 will be returned when:
 *     The filesystem is busy (try again later)
 *     EOF was reached (check afatfs_isEof())
 */
uint32_t afatfs_fread(afatfsFilePtr_t file, uint8_t *buffer, uint32_t len)
{
    if ((file->mode & AFATFS_FILE_MODE_READ) == 0) {
        return 0;
    }

    if (afatfs_fileIsBusy(file)) {
        // There might be a seek pending
        return 0;
    }

    len = MIN(file->directoryEntry.fileSize - file->cursorOffset, len);

    uint32_t readBytes = 0;
    uint32_t cursorOffsetInSector = file->cursorOffset % AFATFS_SECTOR_SIZE;

    while (len > 0) {
        uint32_t bytesToReadThisSector = MIN(AFATFS_SECTOR_SIZE - cursorOffsetInSector, len);
        uint8_t *sectorBuffer;

        sectorBuffer = afatfs_fileGetCursorSectorForRead(file);
        if (!sectorBuffer) {
            // Cache is currently busy
            return readBytes;
        }

        memcpy(buffer, sectorBuffer + cursorOffsetInSector, bytesToReadThisSector);

        readBytes += bytesToReadThisSector;

        /*
         * If the seek doesn't complete immediately then we'll break and wait for that seek to complete by waiting for
         * the file to be non-busy on entry again.
         *
         * A seek operation should always be able to queue on the file since we have checked that the file wasn't busy
         * on entry (fseek will never return AFATFS_OPERATION_FAILURE).
         */
        if (afatfs_fseek(file, bytesToReadThisSector, AFATFS_SEEK_CUR) == AFATFS_OPERATION_IN_PROGRESS) {
            break;
        }

        len -= bytesToReadThisSector;
        buffer += bytesToReadThisSector;
        cursorOffsetInSector = 0;
    }

    return readBytes;
}

bool afatfs_feof(afatfsFilePtr_t file)
{
    return file->cursorOffset >= file->directoryEntry.fileSize;
}

static void afatfs_fileOperationContinue(afatfsFile_t *file)
{
    if (file->type == AFATFS_FILE_TYPE_NONE)
        return;

    switch (file->operation.operation) {
        case AFATFS_FILE_OPERATION_CREATE_FILE:
            afatfs_createFileContinue(file);
        break;
        case AFATFS_FILE_OPERATION_SEEK:
            afatfs_fseekInternalContinue(file);
        break;
        case AFATFS_FILE_OPERATION_CLOSE:
            afatfs_closeFileContinue(file);
        break;
        case AFATFS_FILE_OPERATION_UNLINK:
             afatfs_funlinkContinue(file);
        break;
        case AFATFS_FILE_OPERATION_TRUNCATE:
            afatfs_ftruncateContinue(file);
        break;
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_FILE_OPERATION_APPEND_SUPERCLUSTER:
            if (afatfs_appendSuperclusterContinue(file) != AFATFS_OPERATION_IN_PROGRESS) {
                file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            }
        break;
#endif
        case AFATFS_FILE_OPERATION_APPEND_FREE_CLUSTER:
            if (afatfs_appendRegularFreeClusterContinue(file) != AFATFS_OPERATION_IN_PROGRESS) {
                file->operation.operation = AFATFS_FILE_OPERATION_NONE;
            }
        break;
        case AFATFS_FILE_OPERATION_EXTEND_SUBDIRECTORY:
            afatfs_extendSubdirectoryContinue(file);
        break;
        case AFATFS_FILE_OPERATION_NONE:
            ;
        break;
    }
}

static void afatfs_fileOperationsPoll()
{
    afatfs_fileOperationContinue(&afatfs.currentDirectory);

    for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
        afatfs_fileOperationContinue(&afatfs.openFiles[i]);
    }
}

#ifdef AFATFS_USE_FREEFILE

uint32_t afatfs_getContiguousFreeSpace()
{
    return afatfs.freeFile.directoryEntry.fileSize;
}

/**
 * Call to set up the initial state for finding the largest block of free space on the device whose corresponding FAT
 * sectors are themselves entirely free space (so the free space has dedicated FAT sectors of its own).
 */
static void afatfs_findLargestContiguousFreeBlockBegin()
{
    // The first FAT sector has two reserved entries, so it isn't eligible for this search. Start at the next FAT sector.
    afatfs.initState.freeSpaceSearch.candidateStart = afatfs_fatEntriesPerSector();
    afatfs.initState.freeSpaceSearch.candidateEnd = afatfs.initState.freeSpaceSearch.candidateStart;
    afatfs.initState.freeSpaceSearch.bestGapStart = 0;
    afatfs.initState.freeSpaceSearch.bestGapLength = 0;
    afatfs.initState.freeSpaceSearch.phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
}

/**
 * Call to continue the search for the largest contiguous block of free space on the device.
 *
 * Returns:
 *     AFATFS_OPERATION_IN_PROGRESS - SD card is busy, call again later to resume
 *     AFATFS_OPERATION_SUCCESS - When the search has finished and afatfs.initState.freeSpaceSearch has been updated with the details of the best gap.
 *     AFATFS_OPERATION_FAILURE - When a read error occured
 */
static afatfsOperationStatus_e afatfs_findLargestContiguousFreeBlockContinue()
{
    afatfsFreeSpaceSearch_t *opState = &afatfs.initState.freeSpaceSearch;
    uint32_t fatEntriesPerSector = afatfs_fatEntriesPerSector();
    uint32_t candidateGapLength, searchLimit;
    afatfsFindClusterStatus_e searchStatus;

    while (1) {
        switch (opState->phase) {
            case AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE:
                // Find the first free cluster
                switch (afatfs_findClusterWithCondition(CLUSTER_SEARCH_FREE_SECTOR_AT_BEGINNING_OF_FAT_SECTOR, &opState->candidateStart, afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER)) {
                    case AFATFS_FIND_CLUSTER_FOUND:
                        opState->candidateEnd = opState->candidateStart + 1;
                        opState->phase = AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE;
                    break;

                    case AFATFS_FIND_CLUSTER_FATAL:
                        // Some sort of read error occured
                        return AFATFS_OPERATION_FAILURE;

                    case AFATFS_FIND_CLUSTER_NOT_FOUND:
                        // We finished searching the volume (didn't find any more holes to examine)
                        return AFATFS_OPERATION_SUCCESS;

                    case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                        return AFATFS_OPERATION_IN_PROGRESS;
                }
            break;
            case AFATFS_FREE_SPACE_SEARCH_PHASE_GROW_HOLE:
                // Find the first used cluster after the beginning of the hole (that signals the end of the hole)

                // Don't search beyond the end of the volume, or such that the freefile size would exceed the max filesize
                searchLimit = MIN((uint64_t) opState->candidateStart + FAT_MAXIMUM_FILESIZE / afatfs_clusterSize(), afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);

                searchStatus = afatfs_findClusterWithCondition(CLUSTER_SEARCH_OCCUPIED_SECTOR, &opState->candidateEnd, searchLimit);

                switch (searchStatus) {
                    case AFATFS_FIND_CLUSTER_NOT_FOUND:
                    case AFATFS_FIND_CLUSTER_FOUND:
                        // Either we found a used sector, or the search reached the end of the volume or exceeded the max filesize
                        candidateGapLength = opState->candidateEnd - opState->candidateStart;

                        if (candidateGapLength > opState->bestGapLength) {
                            opState->bestGapStart = opState->candidateStart;
                            opState->bestGapLength = candidateGapLength;
                        }

                        if (searchStatus == AFATFS_FIND_CLUSTER_NOT_FOUND) {
                            // This is the best hole there can be
                            return AFATFS_OPERATION_SUCCESS;
                        } else {
                            // Start a new search for a new hole
                            opState->candidateStart = roundUpTo(opState->candidateEnd + 1, fatEntriesPerSector);
                            opState->phase = AFATFS_FREE_SPACE_SEARCH_PHASE_FIND_HOLE;
                        }
                    break;

                    case AFATFS_FIND_CLUSTER_FATAL:
                        // Some sort of read error occured
                        return AFATFS_OPERATION_FAILURE;

                    case AFATFS_FIND_CLUSTER_IN_PROGRESS:
                        return AFATFS_OPERATION_IN_PROGRESS;
                }
            break;
        }
    }
}

static void afatfs_freeFileCreated(afatfsFile_t *file)
{
    if (file) {
        // Did the freefile already have allocated space?
        if (file->directoryEntry.fileSize > 0) {
            afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
        } else {
            // Allocate clusters for the freefile
            afatfs_findLargestContiguousFreeBlockBegin();
            afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH;
        }
    } else {
        // Failed to allocate an entry
        afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
    }
}

#endif

static void afatfs_initContinue()
{
#ifdef AFATFS_USE_FREEFILE
    afatfsOperationStatus_e status;
#endif

    uint8_t *sector;

    doMore:

    switch (afatfs.substate) {
        case AFATFS_SUBSTATE_INITIALIZATION_READ_MBR:
            if (afatfs_cacheSector(0, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseMBR(sector)) {
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID;
                    goto doMore;
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_READ_VOLUME_ID:
            if (afatfs_cacheSector(afatfs.partitionStartSector, &sector, AFATFS_CACHE_READ | AFATFS_CACHE_DISCARDABLE) == AFATFS_OPERATION_SUCCESS) {
                if (afatfs_parseVolumeID(sector)) {
                    // Open the root directory
                    afatfs_chdir(NULL);

#ifdef AFATFS_USE_FREEFILE
                    afatfs_createFile(&afatfs.freeFile, AFATFS_FREESPACE_FILENAME, FAT_FILE_ATTRIBUTE_SYSTEM,
                        AFATFS_FILE_MODE_CREATE | AFATFS_FILE_MODE_RETAIN_DIRECTORY, afatfs_freeFileCreated);
                    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATING;
#else
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
#endif
                } else {
                    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
                }
            }
        break;
#ifdef AFATFS_USE_FREEFILE
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_CREATING:
            afatfs_fileOperationContinue(&afatfs.freeFile);
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_FAT_SEARCH:
            if (afatfs_findLargestContiguousFreeBlockContinue() == AFATFS_OPERATION_SUCCESS) {
                // If the freefile ends up being empty then we only have to save its directory entry:
                afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY;

                if (afatfs.initState.freeSpaceSearch.bestGapLength > AFATFS_FREEFILE_LEAVE_CLUSTERS + 1) {
                    afatfs.initState.freeSpaceSearch.bestGapLength -= AFATFS_FREEFILE_LEAVE_CLUSTERS;

                    /* So that the freefile never becomes empty, we want it to occupy a non-integer number of
                     * superclusters. So its size mod the number of clusters in a supercluster should be 1.
                     */
                    afatfs.initState.freeSpaceSearch.bestGapLength = ((afatfs.initState.freeSpaceSearch.bestGapLength - 1) & ~(afatfs_fatEntriesPerSector() - 1)) + 1;

                    // Anything useful left over?
                    if (afatfs.initState.freeSpaceSearch.bestGapLength > afatfs_fatEntriesPerSector()) {
                        uint32_t startCluster = afatfs.initState.freeSpaceSearch.bestGapStart;
                        // Points 1-beyond the final cluster of the freefile:
                        uint32_t endCluster = afatfs.initState.freeSpaceSearch.bestGapStart + afatfs.initState.freeSpaceSearch.bestGapLength;

                        afatfs_assert(endCluster < afatfs.numClusters + FAT_SMALLEST_LEGAL_CLUSTER_NUMBER);

                        afatfs.initState.freeSpaceFAT.startCluster = startCluster;
                        afatfs.initState.freeSpaceFAT.endCluster = endCluster;

                        afatfs.freeFile.directoryEntry.firstClusterHigh = startCluster >> 16;
                        afatfs.freeFile.directoryEntry.firstClusterLow = startCluster & 0xFFFF;

                        afatfs.freeFile.directoryEntry.fileSize = afatfs.initState.freeSpaceSearch.bestGapLength * afatfs_clusterSize();

                        // We can write the FAT table for the freefile now
                        afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT;
                    } // Else the freefile's FAT chain and filesize remains the default (empty)
                }

                goto doMore;
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_UPDATE_FAT:
            status = afatfs_FATFillWithPattern(AFATFS_FAT_PATTERN_TERMINATED_CHAIN, &afatfs.initState.freeSpaceFAT.startCluster, afatfs.initState.freeSpaceFAT.endCluster);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY;

                goto doMore;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
        break;
        case AFATFS_SUBSTATE_INITIALIZATION_FREEFILE_SAVE_DIR_ENTRY:
            status = afatfs_saveDirectoryEntry(&afatfs.freeFile);

            if (status == AFATFS_OPERATION_SUCCESS) {
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_READY;
            } else if (status == AFATFS_OPERATION_FAILURE) {
                afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_FATAL;
            }
        break;
#endif
    }
}

void afatfs_poll()
{
    sdcard_poll();

    afatfs_flush();

    switch (afatfs.filesystemState) {
        case AFATFS_FILESYSTEM_STATE_INITIALIZATION:
            afatfs_initContinue();
        break;
        case AFATFS_FILESYSTEM_STATE_READY:
            afatfs_fileOperationsPoll();
        break;
        default:
            ;
    }
}

// TODO don't expose this
afatfsFilesystemState_e afatfs_getFilesystemState()
{
    return afatfs.filesystemState;
}

void afatfs_init()
{
    afatfs.filesystemState = AFATFS_FILESYSTEM_STATE_INITIALIZATION;
    afatfs.substate = AFATFS_SUBSTATE_INITIALIZATION_READ_MBR;
    afatfs.lastClusterAllocated = 1;

    afatfs_poll();
}

/**
 * Shut down the filesystem, flushing all data to the disk. Keep calling until it returns true.
 */
bool afatfs_destroy()
{
    // Don't attempt detailed cleanup if the filesystem is in an odd state
    if (afatfs.filesystemState == AFATFS_FILESYSTEM_STATE_READY) {
#ifdef AFATFS_USE_FREEFILE
        afatfs_fclose(&afatfs.freeFile);
#endif

        for (int i = 0; i < AFATFS_MAX_OPEN_FILES; i++) {
            afatfs_fclose(&afatfs.openFiles[i]);
        }

        afatfs_poll();

        for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
            // Flush even if the pages are "locked"
            if (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_DIRTY) {
                if (afatfs_cacheFlushSector(i)) {
                    // Card will be busy making that write so don't bother trying to flush any other pages right now
                    return false;
                }
            }
        }
    }

    // Clear the afatfs so it's as if we never ran
    memset(&afatfs, 0, sizeof(afatfs));

    return true;

}

/**
 * Get a pessimistic estimate of the amount of buffer space that we have available to write to immediately.
 */
uint32_t afatfs_getFreeBufferSpace()
{
    uint32_t result = 0;
    for (int i = 0; i < AFATFS_NUM_CACHE_SECTORS; i++) {
        if (!afatfs.cacheDescriptor[i].locked && (afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_EMPTY || afatfs.cacheDescriptor[i].state == AFATFS_CACHE_STATE_IN_SYNC)) {
            result += AFATFS_SECTOR_SIZE;
        }
    }
    return result;
}
