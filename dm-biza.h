#ifndef DM_BIZA_H
#define DM_BIZA_H

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/device-mapper.h>
#include <linux/dm-kcopyd.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/rbtree.h>
#include <linux/radix-tree.h>
#include <linux/module.h>
#include <linux/log2.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/blkzoned.h>
#include <linux/xarray.h>
#include <linux/raid/xor.h>
#include <linux/kfifo.h>
#include <linux/nvme.h>
#include <linux/nvme_ioctl.h>
#include <linux/random.h>
#include <linux/compat.h>
#include <linux/min_heap.h>
#include <linux/blk-mq.h>
// #define BIZA_LOG_DEBUG 1

#define RAUM_BIG_CHUNK_PAGES 64
#define RAUM_LARGER_CHUNK_PAGES 64

/** 0 means max **/
#define NUM_SUBMIT_WORKER 2

#define NUM_DM_BIZA_PARAM 3
#define MIN_DEVS 3

#define BIZA_BIO_POOL_SIZE 2048
#define BIZA_DATA_CACHE_SIZE 8192
#define BIZA_PARITY_CACHE_SIZE 8192

#define BIZA_MAP_UNMAPPED (~((sector_t)0))
#define BIZA_MAP_INVALID (BIZA_MAP_UNMAPPED - 1)
#define BIZA_MAP_PARITY (BIZA_MAP_UNMAPPED - 2)

#define BIZA_ZRWASZ 1024 // in KiB
#define BIZA_NR_MAX_OPEN_ZONE 14 // per drive
#define BIZA_NR_ISOLATION_DOMAIN 2 // number of channels per drive
/** BIZA-DEBUG **/
#define BIZA_HIGH_LAT_AWARE_THRESHOLD 3
#define BIZA_ISO_DOMAIN_CONFIDENCE 3

#define BIZA_GC_LIMIT_HIGH 30
/** BIZA-DEBUG **/
#define BIZA_GC_LIMIT_LOW 10
#define BIZA_GC_LOWEST_SPEED 10
/** BIZA-DEBUG **/
#define BIZA_GC_DETECT_PERIOD msecs_to_jiffies(1000)
#define BIZA_IDLE_PERIOD (10UL * HZ)

#define ZONE_GET_WPTR_FAIL_DELAY 5 // in usec
#define NR_ZRWA_AWARE_OPEN_ZONES \
	6 // try to use ZRWA for reducing invalid chunks (reduce GC)
#define NR_LIFETIME_AWARE_OPEN_ZONES \
	0 // try to cluster chunks with similar lifetime for GC reducing
#define NR_TRIVIAL_OPEN_ZONES 6
#define NR_GC_OPEN_ZONES 2

#define BIZA_PRED_SET_CAPACITY 262144 // 1 GB for 4 KB chunk
#define BIZA_LIFETIME_AWARE_SET_CAPACITY 65536
#define BIZA_ZRWA_AWARE_SET_CAPACITY 2048
#define BIZA_LIFETIME_AWARE_REUSE_CNT_THRESHOLD 3
#define BIZA_ZRWA_AWARE_REUSE_DIST_THRESHOLD 4096

#define WRITE_AMP_STAT 1

#define nvme_cmd_flush_raum 0x80

static int print_flag = 0;
#ifdef BIZA_LOG_DEBUG
#define log(fmt, ...)                       \
	do {                                \
		pr_err(fmt, ##__VA_ARGS__); \
	} while (0)
#else

#define log(fmt, ...)                               \
	do {                                        \
		if (print_flag)                     \
			pr_err(fmt, ##__VA_ARGS__); \
	} while (0)
#endif
// parameters
struct biza_params {
	uint8_t nr_drives;
	uint8_t k; // num of data chunks in a stripe
	uint8_t m; // num of parity chunks in a stripe i.e., fault tolerance

	uint64_t chunk_size_byte; // in byte
	sector_t chunk_size_sector; // in sector
	uint8_t chunk_size_sector_shift; // sector <-> chunk

	uint64_t max_chunk_size_byte; // in byte
	sector_t max_chunk_size_sector; // in sector
	uint8_t max_chunk_size_sector_shift; // sector <-> chunk

	// All SSD should be the same
	uint32_t nr_zones_per_drive;
	sector_t zone_capacity_chunk;
	sector_t zrwa_size_chunk; // ZRWA size per open zone (in chunk)
	uint8_t nr_isolation_domains;
	uint8_t max_nr_open_zones;

	// for GC reduction
	uint8_t max_nr_zrwa_aware_open_zones;
	uint8_t max_nr_lifetime_aware_open_zones;
	uint8_t max_nr_trivial_open_zones;
	uint8_t max_nr_gc_open_zones;

	sector_t nr_chunks; // number of chunks for users (logical chunks)
	sector_t nr_internal_chunks; // number of chunks in all zones of all SSDs (physical chunks)
	sector_t nr_total_raum_chunks; // number of chunks in all RAUM spaces
	sector_t nr_raum_chunks_per_drive; // number of chunks in a drive (4k ones)
	sector_t nr_raum_big_chunks_per_drive; // number of big chunks in a drive (64/256k ones)
};

typedef enum biza_aware_type {
	BIZA_ZRWA_AWARE = 0,
	BIZA_LIFETIME_AWARE,
	BIZA_TRIVIAL,
	BIZA_GC
} biza_aware_type;

// zone in a drive (ZNS SSD)
struct biza_zone {
	sector_t start; /* Zone start sector */
	sector_t wp; /* Zone write pointer position, in sector */
	sector_t capacity; /* Zone capacity in number of sectors */
	sector_t len; /* Zone length (size) in number of sectors */
	uint8_t cond; /* Zone condition */

	uint64_t nr_invalid_chunks;

	// For active zone:
	ulong *zrwa_wd; // zrwa window, if set, the chunk is in use;
	atomic_t debug_cnt;
	biza_aware_type aware_type; // aware type of data write in this zone
	uint8_t iso_dm; // which isolation domain this zone is in
	int8_t iso_dm_conf; // confidence of the isolation domain (if 0, correct)
	int8_t iso_dm_vote;
	uint8_t high_lat_score;

	atomic64_t in_flight_ios;
	atomic64_t finished_ios;

	spinlock_t zlock;

	struct mutex gc_lock;
};

typedef enum biza_iso_dm_state {
	BIZA_GC_NORMAL = 0,
	BIZA_GC_SRC,
	BIZA_GC_DST
} biza_iso_dm_state_t;

// drive in biza
struct biza_dev {
	struct dm_dev *dev;

	uint32_t nr_zones;
	sector_t capacity; // capacity of the dev in number of sectors
	sector_t len; // length (size) of the dev in number of sectors
	sector_t zrwa_size_chunk; // size of zrwa (in chunk)
	uint32_t ns_id;

	struct biza_zone *zones;

	// open zones, for GC reduction
	uint32_t *open_zones;
	uint8_t nr_zrwa_aware_open_zones;
	uint8_t nr_lifetime_aware_open_zones;
	uint8_t nr_trivial_open_zones;
	uint8_t nr_gc_open_zones;
	atomic_t open_zone_cnt; // use to predict the isolation domain when open zone
	struct rw_semaphore ozlock;

	// for GC avoidance
	biza_iso_dm_state_t *iso_dm_state;
	uint32_t gc_dst_zone_idx;
	ulong avg_lat; // average write lat
	uint64_t avg_lat_cnt;
};

// struct lru_heap_entry {
// 	struct biza_raum_location_entry *content;
// 	uint64_t access_time;
// };

// static bool my_less(const void *a, const void *b)
// {
// 	return ((const struct lru_heap_entry *)a)->access_time <
// 	       ((const struct lru_heap_entry *)b)->access_time;
// }

// static void my_swap(void *a, void *b)
// {
// 	struct biza_raum_location_entry *temp_content =
// 		((struct lru_heap_entry *)a)->content;
// 	const uint64_t temp_access_time =
// 		((struct lru_heap_entry *)a)->access_time;
// 	((struct lru_heap_entry *)a)->content =
// 		((struct lru_heap_entry *)a)->content;
// 	((struct lru_heap_entry *)a)->access_time =
// 		((struct lru_heap_entry *)a)->access_time;
// 	((struct lru_heap_entry *)b)->content = temp_content;
// 	((struct lru_heap_entry *)b)->access_time = temp_access_time;
// }

// static struct min_heap_callbacks lru_funcs = {
// 	.elem_size = sizeof(struct lru_heap_entry),
// 	.less = my_less,
// 	.swp = my_swap,
// };

typedef struct biza_raum_big_chunk {
	struct work_struct work;
	struct list_head list;
	struct biza_chunkioctx *ctx;
	struct biza_target *bt;
	sector_t start_sector;
	sector_t start_pcn;
	uint32_t chunk_count;
	uint32_t this_write_start_sector;
	uint32_t this_write_start_chunk;
	uint32_t this_write_chunk_count;
	struct bio *chunkio;
	uint8_t drive_idx;
	atomic64_t updates_in_flight;
	atomic64_t flush_in_flight;
	bool can_be_flushed_to_zone;
} biza_raum_big_chunk_t;

// RAUM drives
struct biza_raum_dev {
	struct block_device *bdev;

	sector_t capacity; // capacity of the dev in number of sectors
	sector_t capacity_in_chunks; // capacity in number of chunks
	sector_t len; // length (size) of the dev in number of sectors

	struct list_head free_raum_chunks;
	struct list_head in_use_raum_chunks;
	struct list_head full_raum_chunks;
	struct list_head lru_list;
	struct xarray big_chunk_list;
	spinlock_t free_raum_chunks_lock;
	spinlock_t lru_list_lock;
	// sector_t chunk_usage_list; // Chunk type (0: In-place Data; 1: Partial parity)
	uint8_t **chunk_content;
	// struct lru_heap_entry *heap_buf;
	// struct min_heap *heap;

	uint32_t ns_id;
	struct rw_semaphore ozlock;
};

// type of chunk io
typedef enum biza_chunk_io_type {
	BIZA_DATA_WRITE,
	BIZA_PARITY_WRITE,
	BIZA_DATA_UPDATE,
	BIZA_PARITY_UPDATE,
	BIZA_DATA_READ
} biza_chunk_io_type_t;

// type of stripe head io
typedef enum biza_sh_io_type {
	BIZA_SH_WRITE,
	BIZA_SH_IN_PLACE_UPDATE
} biza_sh_io_type_t;

// ctx of current io that use the stripe head
struct biza_stripe_head_ioctx {
	struct bio *bio;
	uint8_t data_wrt_cnt; // data chunk write of this use
	sector_t lcn_start;
	sector_t *data_pcns; // for p2l table update
	sector_t *parity_pcns; // for stripe table
	refcount_t ref;
	biza_sh_io_type_t type;
	blk_status_t status;
};

struct biza_nvme_request {
	struct nvme_command *cmd;
	union nvme_result_biza {
		__le32 u32;
		__le64 u64;
	} result;
	u8 retries;
	u8 flags;
	u16 status;
	/* There are more fields, but we only need these for the result */
};

// stripe head (run time) in biza
typedef struct biza_stripe_head {
	struct work_struct work;
	struct biza_target *bt;
	uint64_t no;
	uint64_t lcn;
	uint8_t nr_data_written;
	uint8_t *parity_cache; // for buffering partial parity
	bool larger_chunk;
	uint64_t chunks_in_shard;
	// ctx of this use
	struct biza_stripe_head_ioctx *ioctx;

	struct list_head link; // enter point of partial stripe list
} biza_stripe_head_t;

typedef struct biza_free_raum_chunk {
	sector_t chunk;
	struct list_head link;
} biza_free_raum_chunk_t;

// stripe (for mapping)
struct biza_stripe {
	sector_t *parity_pcns; // for l2p map
	sector_t *data_lcns; // for p2l map
	uint8_t used;
	uint8_t valid;
	bool larger_chunk;
	uint64_t chunks_in_shard;
};

// biza addr for mapping tables
typedef struct biza_addr {
	sector_t chunk_no;
	uint64_t stripe_no;
	uint8_t slot;
	bool in_raum;
} biza_addr_t;

// mapping tables for biza
struct biza_map {
	biza_addr_t *l2p;
	biza_addr_t *p2l;

	struct xarray stripe_table; // 自带锁
};

/*
 * GC state flags.
 */
enum {
	BIZA_GC_KCOPY,
};

// context for garbage collection of biza target
struct biza_gc {
	struct biza_target *bt;

	uint32_t nr_free_zones;
	uint8_t p_free_zones; // percent of free zones in total zones

	struct dm_kcopyd_client *kc;
	struct dm_kcopyd_throttle kc_throttle;

	struct delayed_work work;
	struct workqueue_struct *wq;

	int kc_err;

	ulong flags;

	/* Last target access time */
	unsigned long atime;
};

/** LRU **/
struct biza_lru {
	struct list_head list;
	uint32_t size;
};

/** Prediction Entry **/
struct biza_pred_entry {
	int lcn;

	int reuse_cnt;
	int reuse_dist;

	uint32_t last_wrt_time;
	biza_aware_type aware_type;

	struct list_head lru_link;
};

struct biza_heap_entry {
	uint32_t key;
	int priority; // reuse dist / reaccess number (reuse cnt)

	struct biza_pred_entry *pred_entry;
};

/** max heap **/
struct biza_heap {
	struct biza_heap_entry **elements;
	int size;
	int capacity;
};

/** Hash Table **/
struct biza_htable_entry {
	uint32_t lcn; // hash key
	struct biza_pred_entry *pred_entry;

	struct hlist_node link;
};

struct biza_raum_location_entry {
	struct list_head link;
	sector_t lcn; // In RAID
	sector_t raum_chunk; // In RAUM
	sector_t stripe_no;
	uint8_t slot;
	uint8_t drive_idx;
	bool parity;
};

// pre-allocate pages for data/parity
struct biza_mempool {
	uint8_t **elements;
	int cur_nr;
	int min_nr;
	int order;
	spinlock_t lock;
};

// biza dm target
struct biza_target {
	// parameter for biza
	struct biza_params *params;

	// drivers in biza
	struct biza_dev *devs;
	struct biza_raum_dev *raum_devs;

	// Queue for biza target
	struct radix_tree_root io_rxtree;
	struct workqueue_struct *iowq; // work queue
	struct workqueue_struct *end_iowq; // work queue
	struct workqueue_struct *end_bigchunk_iowq; // work queue
	struct mutex io_lock;

	// mapping tables
	struct biza_map *map;

	// stripe no counter
	atomic64_t strip_no_cnt;

	/** zrwa cache **/
	struct list_head pshl; // partial stripe head list
	spinlock_t pshl_lock;
	struct xarray
		fshc; // full stripe head cache (free sh when the parity is evict from zrwa)
	struct xarray dc; // data cache (cache data in zrwa for in place update)
	struct xarray raum_data; // data chunks in RAUM
	struct xarray raum_parity; // parity chunks in RAUM
	struct biza_mempool dcpool;
	struct biza_mempool pcpool;
	struct biza_mempool largepcpool64;
	struct biza_mempool largepcpool32;
	struct biza_mempool largepcpool16;
	struct biza_mempool largepcpool8;
	struct biza_mempool largepcpool4;
	struct biza_mempool largepcpool2;

	// data feature prediction for GC reduction
	struct biza_lru *
		lru; /** if lcn is in lru, we start to track its reuse cnt & reuse distance **/
	struct biza_heap *heap_cnt;
	struct biza_heap *heap_dist;
	struct hlist_head htable[BIZA_PRED_SET_CAPACITY];
	int max_dist_lfta_set, min_dist_lfta_set;
	uint64_t wrt_time; // virtual time decided by write cnt, used for compute reuse distance
	struct mutex pred_lock;

	// for garbage collection
	struct biza_gc *gc;
	uint8_t gc_limit_high; // gc start threshold
	uint8_t gc_limit_low; // fast gc threshold
	struct mutex gc_schedule_lock;

	// for bio clone
	struct bio_set bio_set;

	// statistics for write amplification (in sector, i.e., 512B)
	atomic64_t user_send;
	atomic64_t user_read;
	atomic64_t data_write;
	atomic64_t gc_write;
	atomic64_t num_chunks;
	atomic64_t parity_write;
	atomic64_t data_in_place_update;
	atomic64_t parity_in_place_update;
	atomic64_t data_flush;
	atomic64_t parity_flush;

	atomic64_t previous_print_time;
};

// ctx for each bio on biza
struct biza_bioctx {
	struct biza_target *bt;
	refcount_t ref;
};

// ctx for each sub bio (cloned) in biza
struct biza_chunkioctx {
	union {
		struct bio *bio; // if parent is original bio : for data read
		biza_stripe_head_t *
			sh; // if parent is stripe head : for data write / parity write
	};
	biza_chunk_io_type_t type;

	struct biza_target *bt;
	sector_t lcn;
	sector_t pcn;
	sector_t parity_start_data_lcn;
	uint8_t slot;
	uint8_t drive_idx;
	bool in_raum;
	bool raum_flush_to_zone;

	unsigned long stime; // I/O start time (in jiffies)
};

// io work of biza
struct biza_io_work {
	struct work_struct work;
	refcount_t ref;
	struct biza_target *bt;
	sector_t lcn;
	struct bio_list bio_list;
};

/** Functions defined in dm-biza-target.c **/
int biza_reset_zone(struct biza_target *bt, struct biza_dev *dev,
		    uint32_t zone_idx, bool all);
uint32_t biza_open_empty_zone(struct biza_target *bt, struct biza_dev *dev,
			      bool zrwa, biza_aware_type type);
void biza_chunkio_endio(struct bio *chunkio);
biza_stripe_head_t *biza_get_stripe_head_with_no(struct biza_target *bt,
						 uint64_t no);
struct biza_stripe_head_ioctx *
biza_alloc_stripe_head_ioctx(struct biza_target *bt, uint8_t data_wrt_cnt);
int biza_finish_zone(struct biza_target *bt, struct biza_dev *dev,
		     uint32_t zone_idx);
inline void biza_get_write_location(struct biza_target *bt, uint64_t hint,
				    uint8_t drive_idx, uint32_t *zone_idx,
				    uint64_t *offset, sector_t size);
void biza_free_stripe_head(struct biza_target *bt, biza_stripe_head_t *sh);
void biza_mempool_free(struct biza_mempool *pool, uint8_t *element);
void biza_bigchunkio_endio(struct bio *chunkio);

/** Functions defined in dm-biza-gc.c **/
int biza_ctr_gc(struct biza_target *bt);
void biza_dtr_gc(struct biza_target *bt);
void biza_gc_update_accese_time(struct biza_target *bt);
inline void biza_schedule_gc(struct biza_target *bt);
uint8_t biza_get_oz_idx_gc_avoid(struct biza_target *bt, struct biza_dev *dev,
				 uint8_t ozg_idx);
void biza_gc_avoid_stat(struct biza_chunkioctx *chunkioctx);

/** Functions defined in dm-biza-map.c **/
inline sector_t biza_sector_to_pcn(struct biza_target *bt, uint8_t drive_idx,
				   sector_t sector);
int biza_ctr_map(struct biza_target *bt);
void biza_dtr_map(struct biza_target *bt);
inline sector_t biza_raum_idx_to_sector(struct biza_target *bt,
					uint64_t offset);
inline sector_t biza_raum_idx_to_pcn(struct biza_target *bt, uint8_t drive_idx,
				     uint64_t offset);
inline void biza_raum_pcn_to_idx(struct biza_target *bt, sector_t pcn,
				 uint8_t *drive_idx, uint64_t *offset);
inline bool biza_check_pcn_in_raum(struct biza_target *bt, sector_t pcn);
inline sector_t biza_idx_to_sector(struct biza_target *bt, uint8_t drive_idx,
				   uint32_t zone_idx, uint64_t offset,
				   bool ignore_offset);
inline sector_t biza_idx_to_pcn(struct biza_target *bt, uint8_t drive_idx,
				uint32_t zone_idx, uint64_t offset);
inline void biza_pcn_to_idx(struct biza_target *bt, sector_t pcn,
			    uint8_t *drive_idx, uint32_t *zone_idx,
			    uint64_t *offset);
inline sector_t biza_map_lcn_lookup_pcn(struct biza_target *bt, sector_t lcn);
inline sector_t biza_map_pcn_lookup_lcn(struct biza_target *bt, sector_t pcn);
inline sector_t biza_map_parity_lookup_pcn(struct biza_target *bt, uint64_t no,
					   uint8_t slot);
inline sector_t biza_map_lcn_lookup_stripe_no(struct biza_target *bt,
					      sector_t lcn);
inline struct biza_stripe *biza_map_lcn_lookup_stripe(struct biza_target *bt,
						      sector_t lcn);
inline uint64_t biza_map_pcn_lookup_stripe_no(struct biza_target *bt,
					      sector_t pcn);
inline bool biza_map_is_data_in_pcn_useful(struct biza_target *bt,
					   sector_t pcn);
void biza_map_update_data_wrt(struct biza_target *bt, sector_t lcn,
			      sector_t pcn, uint64_t no, uint8_t slot,
			      bool in_raum, bool larger_chunk,
			      uint64_t chunks_in_shard);
void biza_map_update_parity_wrt(struct biza_target *bt, sector_t pcn,
				uint64_t no, uint8_t slot, bool in_raum,
				bool larger_chunk, uint64_t chunks_in_shard);
void biza_map_remap(struct biza_target *bt, sector_t src_pcn, sector_t dst_pcn,
		    struct xarray *parity_pcn);

/** Functions defined in dm-biza-ds.c **/
enum biza_aware_type biza_check_aware_type(struct biza_target *bt,
					   uint32_t hint);
int biza_ctr_pred(struct biza_target *bt);
void biza_dtr_pred(struct biza_target *bt);
void biza_update_pred(struct biza_target *bt, sector_t lcn);
uint8_t biza_choose_open_zone_to_write(struct biza_target *bt,
				       uint8_t drive_idx, uint32_t hint);
sector_t biza_round_chunk_no(sector_t chunk_no);

int biza_do_gc_on_drive(struct biza_target *bt, uint8_t drive);

int biza_do_gc(struct biza_target *bt);
// Should do gc?
static inline bool biza_should_gc(struct biza_target *bt)
{
	BUG_ON(bt->gc->nr_free_zones >
	       bt->params->nr_zones_per_drive * bt->params->nr_drives);
	BUG_ON(bt->gc->p_free_zones > 100);

	// pr_err("total free zones: %u, free zones percent: %u%%, limit high: %u%%", bt->gc->nr_free_zones, bt->gc->p_free_zones, bt->gc_limit_high);

	return bt->gc->p_free_zones < bt->gc_limit_high;
}

static bool biza_is_valid_pcn(struct biza_target *bt, sector_t pcn)
{
	if (pcn >=
	    bt->params->nr_internal_chunks + bt->params->nr_total_raum_chunks) {
		// pr_err("Invalid pcn 0x%llx\n", pcn);
		return false;
	}

	return true;
}
#endif
