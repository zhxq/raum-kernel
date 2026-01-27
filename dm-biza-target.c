#include "dm-biza.h"

/**
 * When open a zone, predict which isolation domain (i.e., I/O channel) it map with.
 * in a round-robin manner
 */
static inline uint8_t biza_predict_isolation_domain(struct biza_target *bt,
						    struct biza_dev *dev)
{
	return atomic_inc_return(&dev->open_zone_cnt) %
	       bt->params->nr_isolation_domains;
}

// sector_t biza_flush_raum_area(struct biza_target *bt, uint8_t drive_idx,
// 			      uint32_t zone_idx, sector_t wp, sector_t nlb,
// 			      sector_t raum_location)
// {
// 	struct biza_raum_dev *dev = &bt->raum_devs[drive_idx];
// 	struct gendisk *disk = dev->bdev->bd_disk;
// 	struct nvme_passthru_cmd cmd = {};
// 	uint64_t result;
// 	int err;
// 	cmd.opcode = nvme_cmd_zone_mgmt_send;
// 	cmd.cdw10 = wp & 0xffffffff;
// 	cmd.cdw11 = wp >> 32;
// 	cmd.cdw12 = nlb - 1;
// 	cmd.cdw13 = (0x12 & 0xff);
// 	// RAUM addrs should be small enough to fit in 32-bit ints
// 	cmd.cdw14 = raum_location & 0xffffffff;
// 	// But just for the convention...
// 	cmd.cdw15 = raum_location >> 32;
// 	cmd.nsid = dev->ns_id;
// 	err = disk->fops->ioctl(dev->bdev, 0, NVME_IOCTL_IO_CMD,
// 				(unsigned long)&cmd);
// 	if (err) {
// 		pr_err("Error code: %d\n", err);
// 	}
// 	result = cmd.result;
// 	atomic64_inc(&bt->devs[drive_idx].zones[zone_idx].finished_ios);
// 	// pr_err("FLUSHING!!!! wp: 0x%llx, cdw12: 0x%llx, raum_location: 0x%llx, ns_id: %u\n",
// 	//        wp, (nlb - 1), raum_location, dev->ns_id);
// 	return (sector_t)result;
// }

sector_t biza_flush_raum_area(struct biza_target *bt, uint8_t drive_idx,
			      uint32_t zone_idx, sector_t wp, sector_t zone_pcn,
			      sector_t nlb, sector_t raum_chunk)
{
	struct biza_raum_dev *dev = &bt->raum_devs[drive_idx];
	struct request_queue *q = dev->bdev->bd_disk->queue;
	struct request *req;
	struct biza_nvme_request *nrq;
	struct nvme_command *cmd;
	uint64_t result = 0;
	sector_t raum_location = raum_chunk
				 << bt->params->chunk_size_sector_shift;
	int err;

	req = blk_mq_alloc_request(q, REQ_OP_DRV_IN, 0);
	if (IS_ERR(req)) {
		BUG_ON(1);
		return (sector_t)PTR_ERR(req);
	}

	req->rq_flags |= RQF_DONTPREP;
	nrq = blk_mq_rq_to_pdu(req);
	cmd = nrq->cmd;
	memset(cmd, 0, sizeof(*cmd));

	cmd->common.opcode =
		nvme_cmd_flush_raum; // nvme_cmd_zone_mgmt_send (check your headers)
	cmd->common.nsid = cpu_to_le32(dev->ns_id);
	cmd->common.cdw10 = cpu_to_le32(wp & 0xffffffff);
	cmd->common.cdw11 = cpu_to_le32(wp >> 32);
	cmd->common.cdw12 = cpu_to_le32(nlb - 1);
	cmd->common.cdw13 = 0;
	cmd->common.cdw14 = cpu_to_le32(raum_location & 0xffffffff);
	cmd->common.cdw15 = cpu_to_le32(raum_location >> 32);

	err = blk_execute_rq(dev->bdev->bd_disk, req, 0);
	if (err) {
		pr_err("Biza: Command failed with %d\n", err);
		BUG_ON(1);
	} else {
		result = le64_to_cpu(nrq->result.u64);
	}
	blk_mq_free_request(req);
	atomic64_inc(&bt->devs[drive_idx].zones[zone_idx].finished_ios);

	return (sector_t)result;
}

// int biza_flush_raum_area(struct biza_target *bt, uint8_t drive_idx,
// 			 uint32_t zone_idx, sector_t wp, sector_t zone_pcn,
// 			 sector_t nlb, sector_t raum_chunk)
// {
// 	int ret, i;
// 	sector_t raum_location = raum_chunk
// 				 << bt->params->chunk_size_sector_shift;
// 	nvme_req();
// 	sector_t raum_pcn = biza_raum_idx_to_pcn(bt, drive_idx, raum_chunk);
// 	uint8_t *data_buffer = xa_load(&bt->dc, raum_pcn);
// 	struct bio *chunkio = bio_alloc_bioset(GFP_NOIO, 1, &bt->bio_set);
// 	struct biza_chunkioctx *chunkioctx;
// 	biza_stripe_head_t *sh = biza_get_stripe_head_with_no(
// 		bt, bt->map->p2l[raum_pcn].stripe_no);
// 	BUG_ON(sh == NULL);
// 	bool parity = bt->map->p2l[raum_pcn].chunk_no == BIZA_MAP_PARITY;
// 	sh->ioctx = biza_alloc_stripe_head_ioctx(bt, 1);
// 	struct biza_stripe_head_ioctx *shioctx = sh->ioctx;
// 	BUG_ON(shioctx == NULL);

// 	if (!chunkio)
// 		return -ENOMEM;
// 	// pr_err("Before add_page is_parity? %d\n", parity);
// 	if (!parity) {
// 		ret = bio_add_page(chunkio, virt_to_page(data_buffer),
// 				   bt->params->chunk_size_byte, 0);
// 	} else {
// 		for (i = 0; i < bt->params->m; ++i) {
// 			ret = bio_add_page(
// 				chunkio,
// 				virt_to_page(sh->parity_cache +
// 					     i * bt->params->chunk_size_byte),
// 				bt->params->chunk_size_byte, 0);
// 		}
// 	}

// 	// pr_err("After add_page\n");
// 	if (ret != bt->params->chunk_size_byte)
// 		return -EIO;

// 	bio_set_dev(chunkio, bt->devs[drive_idx].dev->bdev);
// 	chunkio->bi_opf = REQ_OP_ZONE_APPEND | REQ_SYNC;
// 	chunkio->bi_iter.bi_sector = wp;
// 	chunkio->bi_iter.bi_size = bt->params->chunk_size_byte;
// 	chunkio->bi_end_io = biza_chunkio_endio;

// 	chunkioctx = kzalloc(sizeof(struct biza_chunkioctx), GFP_NOIO);
// 	if (!chunkioctx)
// 		return -ENOMEM;
// 	chunkioctx->sh = sh;
// 	chunkioctx->type = parity ? BIZA_PARITY_UPDATE : BIZA_DATA_UPDATE;

// 	if (!parity) {
// 		chunkioctx->stime = jiffies;
// 	}
// 	chunkioctx->bt = bt;
// 	chunkioctx->lcn = bt->map->p2l[raum_pcn].chunk_no;
// 	chunkioctx->pcn = zone_pcn;
// 	chunkioctx->drive_idx = drive_idx;
// 	chunkioctx->slot = bt->map->p2l[raum_pcn].slot;
// 	chunkioctx->in_raum = false;
// 	chunkioctx->raum_flush_to_zone = true;

// 	chunkio->bi_private = chunkioctx;

// 	if (parity) {
// 		shioctx->parity_pcns[bt->map->p2l[raum_pcn].slot] = zone_pcn;
// 	}

// 	refcount_inc(&shioctx->ref);

// 	submit_bio_noacct(chunkio);
// 	return 0;
// }

// open an empty zone with zrwa
// return opend zone idx, idx = dev->nr_zones means no empty zone or open error
/** WARN: This function is real malicious now **/
/** WARN: Move it to blk layer or nvme driver will be better **/
uint32_t biza_open_empty_zone(struct biza_target *bt, struct biza_dev *dev,
			      bool zrwa, biza_aware_type type)
{
	struct gendisk *disk = dev->dev->bdev->bd_disk;
	struct request_queue *q = disk->queue;
	struct request *req;
	struct biza_nvme_request *nrq;
	// struct nvme_passthru_cmd cmd = {};
	struct nvme_command *cmd;
	uint32_t i;
	int err;

	for (i = 0; i < dev->nr_zones; ++i) {
		if (dev->zones[i].cond == BLK_ZONE_COND_EMPTY) {
			req = blk_mq_alloc_request(q, REQ_OP_DRV_IN, 0);
			if (IS_ERR(req)) {
				BUG_ON(1);
				return (sector_t)PTR_ERR(req);
			}

			req->rq_flags |= RQF_DONTPREP;
			nrq = blk_mq_rq_to_pdu(req);
			cmd = nrq->cmd;
			memset(cmd, 0, sizeof(*cmd));

			cmd->common.opcode =
				nvme_cmd_zone_mgmt_send; // nvme_cmd_zone_mgmt_send (check your headers)
			cmd->common.nsid = cpu_to_le32(dev->ns_id);
			cmd->common.cdw10 = (dev->zones[i].start) & 0xffffffff;
			cmd->common.cdw11 = (dev->zones[i].start) >> 32;
			cmd->common.cdw13 = (0x3 & 0xff);

			pr_err("dm-biza: GC: %s, opening dev: %s, zone %d, start: 0x%llx",
			       type == BIZA_GC ? "Yes" : "No", disk->disk_name,
			       i, dev->zones[i].start);

			// i.e., nvme_ioctl
			err = blk_execute_rq(disk, req, 0);
			if (err) {
				pr_err("dm-biza: open zone error: dev: %s, zone_idx %d, err %d\n",
				       disk->disk_name, i, err);
				return dev->nr_zones;
			}

			dev->zones[i].cond = BLK_ZONE_COND_EXP_OPEN;
			dev->zones[i].nr_invalid_chunks = 0;

			dev->zones[i].zrwa_wd =
				kzalloc(BITS_TO_BYTES(dev->zrwa_size_chunk),
					GFP_KERNEL);
			if (!dev->zones[i].zrwa_wd) {
				pr_err("dm-biza: open zone error: cannot alloc zrwa window bitmap for zone_idx %d\n",
				       i);
				return dev->nr_zones;
			}
			atomic_set(&dev->zones[i].debug_cnt, 0);

			dev->zones[i].aware_type = type;
			dev->zones[i].iso_dm =
				biza_predict_isolation_domain(bt, dev);
			dev->zones[i].iso_dm_conf = BIZA_ISO_DOMAIN_CONFIDENCE;
			dev->zones[i].iso_dm_vote = 0;
			dev->zones[i].high_lat_score = 0;

			bt->gc->nr_free_zones--;
			bt->gc->p_free_zones = bt->gc->nr_free_zones * 100 /
					       (bt->params->nr_zones_per_drive *
						bt->params->nr_drives);

			break;
		}
	}

	return i;
}

// Finish a zone and release the zrwa resources
int biza_finish_zone(struct biza_target *bt, struct biza_dev *dev,
		     uint32_t zone_idx)
{
	int ret;
	struct gendisk *disk = dev->dev->bdev->bd_disk;
	pr_err("dm-biza: finishing dev: %s, zone %d, start: 0x%llx",
	       disk->disk_name, zone_idx, dev->zones[zone_idx].start);
	dev->zones[zone_idx].wp =
		dev->zones[zone_idx].start + dev->zones[zone_idx].capacity;
	atomic64_set(&dev->zones[zone_idx].in_flight_ios, 0);
	atomic64_set(&dev->zones[zone_idx].finished_ios, 0);
	ret = blkdev_zone_mgmt(dev->dev->bdev, REQ_OP_ZONE_FINISH,
			       dev->zones[zone_idx].start,
			       dev->zones[zone_idx].len, GFP_NOIO);

	return ret;
}

// Reset a zone or all zone
int biza_reset_zone(struct biza_target *bt, struct biza_dev *dev,
		    uint32_t zone_idx, bool all)
{
	int i = 0, ret = 0;

	if (all) {
		for (i = 0; i < dev->nr_zones; ++i) {
			dev->zones[i].wp = dev->zones[i].start;
			// atomic64_set(&dev->zones[i].wp, dev->zones[i].start);
			dev->zones[i].cond = BLK_ZONE_COND_EMPTY;
			dev->zones[i].nr_invalid_chunks = 0;
		}
		for (i = 0; i < dev->nr_zrwa_aware_open_zones +
					dev->nr_lifetime_aware_open_zones +
					dev->nr_trivial_open_zones;
		     ++i) {
			zone_idx = dev->open_zones[i];
			if (dev->zones[zone_idx].zrwa_wd) {
				kfree(dev->zones[zone_idx].zrwa_wd);
				dev->zones[zone_idx].zrwa_wd = NULL;
			}
		}
		bt->gc->nr_free_zones =
			bt->params->nr_zones_per_drive * bt->params->nr_drives;
		bt->gc->p_free_zones = 100;
		ret = blkdev_zone_mgmt(dev->dev->bdev, REQ_OP_ZONE_RESET, 0,
				       dev->len, GFP_NOIO);
	} else {
		dev->zones[zone_idx].wp = dev->zones[zone_idx].start;
		// atomic64_set(&dev->zones[i].wp, dev->zones[zone_idx].start);
		dev->zones[zone_idx].cond = BLK_ZONE_COND_EMPTY;
		dev->zones[zone_idx].nr_invalid_chunks = 0;
		if (dev->zones[zone_idx].zrwa_wd) {
			kfree(dev->zones[zone_idx].zrwa_wd);
			dev->zones[zone_idx].zrwa_wd = NULL;
		}
		bt->gc->nr_free_zones++;
		bt->gc->p_free_zones = bt->gc->nr_free_zones * 100 /
				       (bt->params->nr_zones_per_drive *
					bt->params->nr_drives);
		ret = blkdev_zone_mgmt(dev->dev->bdev, REQ_OP_ZONE_RESET,
				       dev->zones[zone_idx].start,
				       dev->zones[zone_idx].len, GFP_NOIO);
	}

	BUG_ON(ret);

	return ret;
}

// Initialize a zone
static int biza_init_zone(struct blk_zone *blkz, unsigned int idx, void *data)
{
	struct biza_dev *dev = data;
	struct biza_zone *zone = &dev->zones[idx];

	BUG_ON(blkz->cond == BLK_ZONE_COND_NOT_WP);
	// pr_err("init zone %u, start=0x%llx, capacity=0x%llx, len=0x%llx", idx, blkz->start, blkz->capacity, blkz->len);
	zone->cond = blkz->cond;

	zone->wp = blkz->wp;
	// atomic64_set(&zone->wp, blkz->wp);
	zone->start = blkz->start;
	zone->capacity = blkz->capacity;
	zone->len = blkz->len;
	zone->nr_invalid_chunks = 0;
	atomic64_set(&zone->in_flight_ios, 0);
	atomic64_set(&zone->finished_ios, 0);

	dev->capacity += zone->capacity;
	dev->len += zone->len;

	spin_lock_init(&zone->zlock);

	return 0;
}

// Free devs
static inline void biza_free_devs(struct biza_target *bt, uint8_t cnt)
{
	struct biza_dev *dev;
	struct biza_raum_dev *raum_dev;
	int i = 0;

	BUG_ON(bt == NULL);

	for (i = 0; i < cnt; ++i) {
		dev = &bt->devs[i];
		raum_dev = &bt->raum_devs[i];
		biza_reset_zone(bt, dev, 0, true);
		kfree(dev->open_zones);
		kfree(dev->iso_dm_state);
		kfree(dev->zones);
		kfree(raum_dev->chunk_content);
		// kfree(raum_dev->heap_buf);
		// kfree(raum_dev->heap);
	}
}

static int biza_init_devs_open_zones(struct dm_target *ti)
{
	struct biza_target *bt = ti->private;
	struct biza_dev *dev = NULL;
	int i = 0, j = 0;
	int ret = 0;

	for (i = 0; i < bt->params->nr_drives; ++i) {
		dev = &bt->devs[i];

		for (j = 0; j < bt->params->max_nr_zrwa_aware_open_zones; ++j) {
			dev->open_zones[j] = biza_open_empty_zone(
				bt, dev, true, BIZA_ZRWA_AWARE);
			if (dev->open_zones[j] == dev->nr_zones) {
				ti->error = "Failed to open an zrwa aware zone";
				ret = -EBUSY;
				goto err;
			}
			dev->nr_zrwa_aware_open_zones++;
		}
		for (; j < bt->params->max_nr_zrwa_aware_open_zones +
				   bt->params->max_nr_lifetime_aware_open_zones;
		     ++j) {
			dev->open_zones[j] = biza_open_empty_zone(
				bt, dev, true, BIZA_LIFETIME_AWARE);
			if (dev->open_zones[j] == dev->nr_zones) {
				ti->error =
					"Failed to open an liftime aware zone";
				ret = -EBUSY;
				goto err;
			}
			dev->nr_lifetime_aware_open_zones++;
		}
		for (;
		     j < bt->params->max_nr_zrwa_aware_open_zones +
				 bt->params->max_nr_lifetime_aware_open_zones +
				 bt->params->max_nr_trivial_open_zones;
		     ++j) {
			dev->open_zones[j] = biza_open_empty_zone(bt, dev, true,
								  BIZA_TRIVIAL);
			if (dev->open_zones[j] == dev->nr_zones) {
				ti->error = "Failed to open an trivial zone";
				ret = -EBUSY;
				goto err;
			}
			dev->nr_trivial_open_zones++;
		}
		for (;
		     j < bt->params->max_nr_zrwa_aware_open_zones +
				 bt->params->max_nr_lifetime_aware_open_zones +
				 bt->params->max_nr_trivial_open_zones +
				 bt->params->max_nr_gc_open_zones;
		     ++j) {
			dev->open_zones[j] =
				biza_open_empty_zone(bt, dev, true, BIZA_GC);
			if (dev->open_zones[j] == dev->nr_zones) {
				ti->error = "Failed to open an gc zone";
				ret = -EBUSY;
				goto err;
			}
			dev->nr_gc_open_zones++;
		}
	}

	return 0;

err:
	biza_free_devs(bt, i - 1);
	return ret;
}

// Initialize biza drives
static int biza_init_devs(struct dm_target *ti)
{
	struct biza_target *bt = ti->private;
	struct biza_dev *dev = NULL;
	int i = 0;
	int ret = 0;

	for (i = 0; i < bt->params->nr_drives; ++i) {
		dev = &bt->devs[i];

		dev->nr_zones = blkdev_nr_zones(dev->dev->bdev->bd_disk);

		dev->zones = kzalloc(dev->nr_zones * sizeof(struct biza_zone),
				     GFP_KERNEL);
		if (!dev->zones) {
			ti->error = "Failed to allocate dev zones";
			ret = -ENOMEM;
			goto err;
		}

		if (!blkdev_report_zones(dev->dev->bdev, 0, BLK_ALL_ZONES,
					 biza_init_zone, dev)) {
			ti->error = "Failed to report zones";
			ret = -EINVAL;
			goto err_zones;
		}

		/** WARN: Stupid codes **/
		/** WARN: In the future, should get ns_id with ioctl **/
		dev->ns_id = dev->dev->bdev->bd_disk->disk_name[6] - '0';
		dev->zrwa_size_chunk =
			BIZA_ZRWASZ * 1024 / bt->params->chunk_size_byte;

		dev->open_zones = kzalloc(sizeof(uint32_t) *
						  bt->params->max_nr_open_zones,
					  GFP_KERNEL);
		if (!dev->open_zones) {
			ti->error = "Failed to allocate dev open zones";
			ret = -ENOMEM;
			goto err_report;
		}

		dev->iso_dm_state = kzalloc(bt->params->nr_isolation_domains *
						    sizeof(biza_iso_dm_state_t),
					    GFP_KERNEL);
		if (!dev->iso_dm_state) {
			ti->error = "Failed to allocate isolation domain state";
			ret = -ENOMEM;
			goto err_report;
		}

		init_rwsem(&dev->ozlock);
		atomic_set(&dev->open_zone_cnt, -1);

		dev->gc_dst_zone_idx = dev->nr_zones;
		dev->avg_lat = 0;
		dev->avg_lat_cnt = 0;
	}

	return 0;

err_report:
	biza_reset_zone(bt, dev, 0, true);
err_zones:
	kfree(dev->zones);
err:
	biza_free_devs(bt, i - 1);
	return ret;
}

// Initialize RAUM drives
static int biza_init_raum_devs(struct dm_target *ti)
{
	struct biza_target *bt = ti->private;
	struct biza_raum_dev *dev = NULL;
	biza_free_raum_chunk_t *chunk;
	int i = 0, j = 0;

	for (i = 0; i < bt->params->nr_drives; ++i) {
		dev = &bt->raum_devs[i];

		dev->capacity = bdev_nr_sectors(dev->bdev);
		dev->len = dev->capacity;

		dev->capacity_in_chunks = dev->capacity >>
					  bt->params->chunk_size_sector_shift;

		bt->params->nr_total_raum_chunks += dev->capacity_in_chunks;

		if (bt->params->nr_raum_chunks_per_drive == 0) {
			bt->params->nr_raum_chunks_per_drive =
				dev->capacity_in_chunks;
		}

		BUG_ON(bt->params->nr_raum_chunks_per_drive !=
		       dev->capacity_in_chunks); // Every drive should have the same number

		dev->chunk_content =
			kzalloc(sizeof(uint8_t *) * dev->capacity_in_chunks,
				GFP_KERNEL);

		if (!dev->chunk_content) {
			pr_err("dm-biza: open zone error: cannot alloc chunk content list for RAUM device %d\n",
			       i);
			return -1;
		}

		INIT_LIST_HEAD(&dev->free_raum_chunks);
		INIT_LIST_HEAD(&dev->lru_list);
		spin_lock_init(&dev->lru_list_lock);

		spin_lock_irq(&dev->lru_list_lock);
		for (j = 0; j < dev->capacity_in_chunks; j++) {
			chunk = kzalloc(sizeof(biza_free_raum_chunk_t),
					GFP_KERNEL);
			chunk->chunk = j;
			list_add_tail(&chunk->link, &dev->free_raum_chunks);
		}
		spin_unlock_irq(&dev->lru_list_lock);

		// dev->heap_buf = kzalloc(sizeof(struct min_heap) *
		// 				dev->capacity_in_chunks,
		// 			GFP_KERNEL);
		// if (!dev->heap_buf) {
		// 	pr_err("dm-biza: open zone error: cannot alloc free heap buffer for RAUM device %d\n",
		// 	       i);
		// 	return -1;
		// }
		// heap_init(dev->h, dev->heap_buf, dev->capacity_in_chunks,
		// 	  sizeof(struct min_heap), my_less);

		// dev->chunk_usage_list = kzalloc(
		// 	BITS_TO_BYTES(dev->capacity_in_chunks), GFP_KERNEL);
		// if (!dev->chunk_usage_list) {
		// 	pr_err("dm-biza: open zone error: cannot alloc chunk usage list bitmap for RAUM device %d\n",
		// 	       i);
		// 	return -1;
		// }

		pr_err("RAUM Capacity: 0x%llx sectors (%llu bytes, %llu chunks)\n",
		       dev->capacity,
		       dev->capacity << bt->params->chunk_size_sector_shift,
		       dev->capacity >> bt->params->chunk_size_sector_shift);

		/** WARN: Stupid codes **/
		/** WARN: In the future, should get ns_id with ioctl **/
		dev->ns_id = dev->bdev->bd_disk->disk_name[6] - '0';
		init_rwsem(&dev->ozlock);
	}

	return 0;
}

static int biza_ctr_mempool(struct biza_mempool *pool, int min_nr, int order)
{
	int ret, i = 0, j = 0;

	pool->min_nr = min_nr;
	pool->cur_nr = min_nr;
	pool->order = order;

	spin_lock_init(&pool->lock);

	pool->elements = kzalloc(sizeof(uint8_t *) * min_nr, GFP_KERNEL);
	if (!pool->elements) {
		pr_err("cannot alloc pool\n");
		ret = -ENOMEM;
		goto err;
	}

	for (i = 0; i < min_nr; ++i) {
		pool->elements[i] =
			(uint8_t *)__get_free_pages(GFP_KERNEL, order);
		if (!pool->elements[i]) {
			pr_err("cannot alloc pages\n");
			ret = -ENOMEM;
			goto err_pool;
		}
	}

	return 0;

err_pool:
	for (j = 0; j < i; ++j)
		free_pages((unsigned long)pool->elements[j], order);
	kfree(pool->elements);
err:
	return ret;
}

static void biza_dtr_mempool(struct biza_mempool *pool)
{
	int i;

	/** TODO: Risk of memory leak **/
	for (i = 0; i < pool->cur_nr; ++i) {
		free_pages((unsigned long)pool->elements[i], pool->order);
	}

	kfree(pool->elements);
}

static uint8_t *biza_mempool_alloc(struct biza_mempool *pool)
{
	uint8_t *element = NULL;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	if (pool->cur_nr > 0) {
		element = pool->elements[--pool->cur_nr];
	}

	if (!element) {
		element = (uint8_t *)__get_free_pages(GFP_KERNEL, pool->order);
		// pr_err("mempool run out\n");
	}

	spin_unlock_irqrestore(&pool->lock, flags);

	return element;
}

static void biza_mempool_free(struct biza_mempool *pool, uint8_t *element)
{
	unsigned long flags;

	if (unlikely(element == NULL))
		return;

	spin_lock_irqsave(&pool->lock, flags);
	if (likely(pool->cur_nr < pool->min_nr)) {
		pool->elements[pool->cur_nr++] = element;
	} else
		free_pages((unsigned long)element, pool->order);
	spin_unlock_irqrestore(&pool->lock, flags);
}

/**
 * Entry of creating biza objects
 */
static int biza_ctr(struct dm_target *ti, unsigned int argc, char **argv)
{
	struct biza_target *bt = NULL;
	struct block_device *bdev = NULL;
	int ret = 0, i = 0;

	if (argc < NUM_DM_BIZA_PARAM + MIN_DEVS) {
		ti->error =
			"Not enough arguments: <number of drives (k+m)> <fault tolerance (m)> <chunk size (KiB)> \
                    [ZNS drives] [RAUM drives]";
		ret = -EINVAL;
		goto err;
	}

	// Allocate memory for target
	bt = kzalloc(sizeof(struct biza_target), GFP_KERNEL);
	if (!bt) {
		ti->error = "Failed to allocate biza target";
		ret = -ENOMEM;
		goto err;
	}
	ti->private = bt;

	// Allocate memory for params
	bt->params = kzalloc(sizeof(struct biza_params), GFP_KERNEL);
	if (!bt->params) {
		ti->error = "Failed to allocate biza params";
		ret = -ENOMEM;
		goto err_target;
	}

	// Handle params of AFA
	if (kstrtou8(argv[0], 0, &bt->params->nr_drives)) {
		ti->error = "Invalid number of drives";
		ret = -EINVAL;
		goto err_params;
	}
	if (bt->params->nr_drives < (argc - NUM_DM_BIZA_PARAM) / 2) {
		ti->error = "Insufficient number of [drives]";
		ret = -EINVAL;
		goto err_params;
	}
	if (kstrtou8(argv[1], 0, &bt->params->m)) {
		ti->error = "Invalid number of fault tolerance";
		ret = -EINVAL;
		goto err_params;
	}
	if (bt->params->m >= bt->params->nr_drives) {
		ti->error =
			"Fault tolerance shoule be less than the number of drives";
		ret = -EINVAL;
		goto err_params;
	}
	bt->params->k = bt->params->nr_drives - bt->params->m;

	// Handle chunk size
	if (kstrtoull(argv[2], 0, &bt->params->chunk_size_byte)) {
		ti->error = "Invalid chunk size";
		ret = -EINVAL;
		goto err_params;
	}
	bt->params->chunk_size_byte *= 1024;
	bt->params->chunk_size_sector = bt->params->chunk_size_byte >>
					SECTOR_SHIFT;
	bt->params->chunk_size_sector_shift =
		ilog2(bt->params->chunk_size_sector);

	/** TODO: Get BIZA_NR_MAX_OPEN_ZONE with ioctl, e.g., nvme_ioctl, nvme_report_zones, nvme_submit_sync_cmd **/
	bt->params->max_nr_open_zones = BIZA_NR_MAX_OPEN_ZONE;

	bt->params->max_nr_zrwa_aware_open_zones = NR_ZRWA_AWARE_OPEN_ZONES;
	bt->params->max_nr_lifetime_aware_open_zones =
		NR_LIFETIME_AWARE_OPEN_ZONES;
	bt->params->max_nr_trivial_open_zones = NR_TRIVIAL_OPEN_ZONES;
	bt->params->max_nr_gc_open_zones = NR_GC_OPEN_ZONES;

	// Get # of isolation domains, i.e., # of I/O channels of a ZNS SSD
	bt->params->nr_isolation_domains = BIZA_NR_ISOLATION_DOMAIN;

	// Allocate memory for devs
	pr_err("Allocating devs\n");
	bt->devs = kcalloc(bt->params->nr_drives, sizeof(struct biza_dev),
			   GFP_KERNEL);
	if (!bt->devs) {
		ti->error = "Failed to allocate devs";
		ret = -ENOMEM;
		goto err_params;
	}

	pr_err("Allocating RAUM devs\n");
	// Allocate memory for RAUM devs
	bt->raum_devs = kcalloc(bt->params->nr_drives,
				sizeof(struct biza_raum_dev), GFP_KERNEL);
	if (!bt->raum_devs) {
		ti->error = "Failed to allocate RAUM devs";
		ret = -ENOMEM;
		goto err_params;
	}

	pr_err("Getting devs\n");
	// Get drives
	for (i = 0; i < bt->params->nr_drives; ++i) {
		if (dm_get_device(ti, argv[NUM_DM_BIZA_PARAM + i],
				  dm_table_get_mode(ti->table),
				  &bt->devs[i].dev)) {
			ti->error = "Failed to get drives";
			ret = -EINVAL;
			goto err_dev;
		}
	}

	pr_err("Init devs\n");
	// Initialize drives
	ret = biza_init_devs(ti);
	if (ret) {
		ti->error = "Cannot init drives";
		goto err_dev;
	}

	pr_err("Getting RAUM devs\n");
	// Get RAUM drives
	for (i = bt->params->nr_drives; i < 2 * bt->params->nr_drives; ++i) {
		bdev = blkdev_get_by_path(argv[NUM_DM_BIZA_PARAM + i],
					  FMODE_READ | FMODE_WRITE, NULL);
		if (IS_ERR(bdev)) {
			ti->error = "Failed to get RAUM drives";
			ret = -EINVAL;
			goto err_raum_dev;
		}
		bt->raum_devs[i - bt->params->nr_drives].bdev = bdev;
	}

	pr_err("Init RAUM devs\n");
	bt->params->nr_total_raum_chunks = 0;
	bt->params->nr_raum_chunks_per_drive = 0;
	// Initialize RAUM drives
	ret = biza_init_raum_devs(ti);
	if (ret) {
		ti->error = "Cannot init RAUM drives";
		goto err_raum_dev;
	}

	/** WARN: All SSD should be the same **/
	bt->params->nr_zones_per_drive = bt->devs[0].nr_zones;
	bt->params->zone_capacity_chunk = bt->devs[0].zones[0].capacity >>
					  bt->params->chunk_size_sector_shift;
	bt->params->nr_chunks = bt->params->k * bt->params->nr_zones_per_drive *
				bt->params->zone_capacity_chunk;
	bt->params->nr_internal_chunks = bt->params->nr_drives *
					 bt->params->nr_zones_per_drive *
					 bt->params->zone_capacity_chunk;

	// Handle GC limit
	bt->gc_limit_high = BIZA_GC_LIMIT_HIGH;
	bt->gc_limit_low = BIZA_GC_LIMIT_LOW;

	// Initialize GC context
	ret = biza_ctr_gc(bt);
	if (ret) {
		ti->error = "Failed to init gc context";
		goto err_zones;
	}

	// Initialize mapping tables
	ret = biza_ctr_map(bt);
	if (ret) {
		ti->error = "Failed to init map context";
		goto err_gc;
	}

	// Initialize I/O queues and locks
	bt->iowq = alloc_workqueue("biza_iowq", WQ_MEM_RECLAIM | WQ_UNBOUND,
				   NUM_SUBMIT_WORKER);
	if (!bt->iowq) {
		ti->error = "Failed to create io workqueue";
		ret = -ENOMEM;
		goto err_map;
	}
	mutex_init(&bt->io_lock);
	// spin_lock_init(&bt->io_lock);
	INIT_RADIX_TREE(&bt->io_rxtree, GFP_NOIO);

	// Initialize stripe number counter
	atomic64_set(&bt->strip_no_cnt, -1);

	// Initialize partial stripe list
	INIT_LIST_HEAD(&bt->pshl);
	spin_lock_init(&bt->pshl_lock);

	// Initialize full stripe head cache
	xa_init(&bt->fshc);

	// Initialize data cache;
	xa_init(&bt->dc);

	// Initialize data lcn tracker in RAUM;
	xa_init(&bt->raum_data);
	// hash_init(bt->raum_data);

	// Initialize parity lcn tracker in RAUM;
	xa_init(&bt->raum_parity);
	// hash_init(bt->raum_parity);

	ret = biza_ctr_mempool(&bt->dcpool, BIZA_DATA_CACHE_SIZE,
			       bt->params->chunk_size_sector_shift -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create data cache";
		ret = -ENOMEM;
		goto err_fshc;
	}

	// Initialize parity cache
	ret = biza_ctr_mempool(&bt->pcpool, BIZA_PARITY_CACHE_SIZE,
			       bt->params->chunk_size_sector_shift -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create parity cache";
		ret = -ENOMEM;
		goto err_dc;
	}

	// Initialize pred context (i.e., zone group selector related data structures)
	mutex_init(&bt->pred_lock);
	// spin_lock_init(&bt->pred_lock);
	ret = biza_ctr_pred(bt);
	if (ret) {
		ti->error = "Failed to init pred context";
		goto err_gr;
	}

	// Initialize bio set
	ret = bioset_init(&bt->bio_set, BIZA_BIO_POOL_SIZE, 0,
			  BIOSET_NEED_BVECS);
	if (ret) {
		ti->error = "Failed to create bio set";
		goto err_lru;
	}

	ret = biza_init_devs_open_zones(ti);
	if (ret) {
		goto err_lru;
	}

	// statistics for write amplification
	atomic64_set(&bt->user_send, 0);
	atomic64_set(&bt->data_write, 0);
	atomic64_set(&bt->parity_write, 0);
	atomic64_set(&bt->data_in_place_update, 0);
	atomic64_set(&bt->parity_in_place_update, 0);

	// Settings for block device layer
	ti->per_io_data_size = sizeof(struct biza_bioctx);
	ti->len = bt->params->nr_chunks << bt->params->chunk_size_sector_shift;

	return 0;

err_lru:
	biza_dtr_pred(bt);
err_gr:
	// mutex_destroy(&bt->pred_lock);
	biza_dtr_mempool(&bt->pcpool);
err_dc:
	biza_dtr_mempool(&bt->dcpool);
	xa_destroy(&bt->dc);
err_fshc:
	xa_destroy(&bt->fshc);
	// mutex_destroy(&bt->io_lock);
	destroy_workqueue(bt->iowq);
err_map:
	biza_dtr_map(bt);
err_gc:
	biza_dtr_gc(bt);
err_zones:
	biza_free_devs(bt, bt->params->nr_drives);
err_raum_dev:
	kfree(bt->raum_devs);
err_dev:
	kfree(bt->devs);
err_params:
	kfree(bt->params);
err_target:
	kfree(bt);
err:
	pr_err("dm-biza: ctr error: %s\n", ti->error);
	return ret;
}

// 析构函数，biza对象退出前调用
static void biza_dtr(struct dm_target *ti)
{
	struct biza_target *bt = ti->private;
	struct biza_stripe *stripe;
	int i;

	biza_dtr_pred(bt);
	// mutex_destroy(&bt->pred_lock);
	biza_dtr_mempool(&bt->pcpool);
	biza_dtr_mempool(&bt->dcpool);
	xa_destroy(&bt->dc);
	xa_destroy(&bt->fshc);
	// mutex_destroy(&bt->io_lock);
	flush_workqueue(bt->iowq);
	destroy_workqueue(bt->iowq);
	for (i = 0; i < atomic64_read(&bt->strip_no_cnt); ++i) {
		stripe = xa_load(&bt->map->stripe_table, i);
		if (stripe)
			kfree(stripe);
		xa_erase(&bt->map->stripe_table, i);
	}
	biza_dtr_map(bt);
	biza_free_devs(bt, bt->params->nr_drives);
	biza_dtr_gc(bt);
	kfree(bt->devs);
	kfree(bt->raum_devs);
	kfree(bt->params);
	kfree(bt);
}

// Initialize the bio context
static inline void biza_init_bioctx(struct biza_target *bt, struct bio *bio)
{
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));

	bioctx->bt = bt;
	refcount_set(&bioctx->ref, 1);
}

// Free stripe head
static void biza_free_stripe_head(struct biza_target *bt,
				  biza_stripe_head_t *sh)
{
	// kvfree(sh->parity_cache);
	biza_mempool_free(&bt->pcpool, sh->parity_cache);
	kfree(sh);
}

// Allocate an empty stripe head & init
static biza_stripe_head_t *biza_alloc_empty_stripe_head(struct biza_target *bt)
{
	biza_stripe_head_t *sh =
		kzalloc(sizeof(biza_stripe_head_t), GFP_KERNEL);

	if (sh) {
		sh->no = atomic64_inc_return(
			&bt->strip_no_cnt); // Started with 0
		sh->nr_data_written = 0;

		// sh->parity_cache = kvzalloc(bt->params->m * bt->params->chunk_size_byte, GFP_KERNEL);
		sh->parity_cache = biza_mempool_alloc(&bt->pcpool);
		if (!sh->parity_cache) {
			pr_err("dm-biza: io error: cannot alloc parity cache");
			goto err;
		}
	}

	return sh;

err:
	kfree(sh);
	return NULL;
}

// Get a partially written stripe head
static biza_stripe_head_t *biza_get_partial_stripe_head(struct biza_target *bt)
{
	biza_stripe_head_t *sh = NULL;

	spin_lock_irq(&bt->pshl_lock);
	// sh = list_first_or_null_rcu(&bt->pshl, biza_stripe_head_t, link);
	sh = list_first_entry_or_null(&bt->pshl, biza_stripe_head_t, link);
	if (sh) {
		list_del(&sh->link);
		spin_unlock_irq(&bt->pshl_lock);
	} else {
		spin_unlock_irq(&bt->pshl_lock);
		sh = biza_alloc_empty_stripe_head(bt);
	}

	return sh;
}

// Get a stripe head from cache using stripe no
biza_stripe_head_t *biza_get_stripe_head_with_no(struct biza_target *bt,
						 uint64_t no)
{
	biza_stripe_head_t *sh = NULL, *cur = NULL, *tmp = NULL;

	// Try to get from pshl
	spin_lock_irq(&bt->pshl_lock);
	list_for_each_entry_safe(cur, tmp, &bt->pshl, link) {
		if (cur->no == no) {
			sh = cur;
			list_del(&cur->link);
		}
	}
	spin_unlock_irq(&bt->pshl_lock);

	// Try to get from fshc
	if (!sh) {
		sh = xa_load(&bt->fshc, no);
		if (sh) {
			xa_erase_irq(&bt->fshc, no);
		}
	}

	return sh;
}

// Compute parities
static int biza_compute_parity(struct biza_target *bt, struct bio *bio,
			       biza_stripe_head_t *sh, uint8_t chunk_cnt)
{
	void **chunks = kzalloc(
		(chunk_cnt + bt->params->m) * sizeof(uint64_t *), GFP_KERNEL);
	uint8_t *bvec_start, *data_start;
	int i = 0, this_cnt = 0, src_off = 0;

	if (bt->params->m != 1) {
		pr_err("dm-biza: io error: only support RAID 5 now");
		return -EDOM;
	}

	bvec_start = bvec_kmap_local(&bio->bi_io_vec[0]);
	data_start = bvec_start + bio->bi_iter.bi_bvec_done;
	for (i = 0; i < chunk_cnt; ++i) {
		chunks[i] = data_start + i * bt->params->chunk_size_byte;
	}
	for (i = 0; i < bt->params->m; ++i) {
		chunks[chunk_cnt + i] =
			sh->parity_cache + i * bt->params->chunk_size_byte;
	}

	for (i = 0; i < bt->params->m; ++i) {
		src_off = 0;
		while (chunk_cnt > 0) {
			this_cnt = min(chunk_cnt, (uint8_t)MAX_XOR_BLOCKS);

			xor_blocks(this_cnt, bt->params->chunk_size_byte,
				   chunks[chunk_cnt + i], chunks + src_off);

			chunk_cnt -= this_cnt;
			src_off += this_cnt;
		}
	}
	kunmap_local(bvec_start);
	kfree(chunks);

	return 0;
}

// Get aware type (i.e., ZRWA aware, GC aware, and trival)
static inline biza_aware_type biza_oz_idx_to_aware_type(struct biza_target *bt,
							uint8_t drive_idx,
							uint8_t oz_idx)
{
	struct biza_dev *dev = &bt->devs[drive_idx];
	BUG_ON(bt->params->max_nr_zrwa_aware_open_zones !=
	       dev->nr_zrwa_aware_open_zones);
	BUG_ON(bt->params->max_nr_lifetime_aware_open_zones !=
	       dev->nr_lifetime_aware_open_zones);
	BUG_ON(bt->params->max_nr_trivial_open_zones !=
	       dev->nr_trivial_open_zones);

	if (oz_idx < dev->nr_zrwa_aware_open_zones)
		return BIZA_ZRWA_AWARE;
	else if (oz_idx < dev->nr_zrwa_aware_open_zones +
				  dev->nr_lifetime_aware_open_zones)
		return BIZA_LIFETIME_AWARE;
	else if (oz_idx < dev->nr_zrwa_aware_open_zones +
				  dev->nr_lifetime_aware_open_zones +
				  dev->nr_trivial_open_zones)
		return BIZA_TRIVIAL;
	else
		BUG_ON(1);
}

// if (lcn == BIZA_MAP_PARITY) {
// 				stripe_no =
// 					biza_map_pcn_lookup_stripe_no(bt, pcn);
// 				sh = xa_load(&bt->fshc, stripe_no);
// 				if (sh) {
// 					// pr_err("!!Free parity buffer, pcn=0x%llx, sh->parity_cache=0x%px\n", pcn, sh->parity_cache);
// 					xa_erase(&bt->fshc, stripe_no);
// 					biza_free_stripe_head(bt, sh);
// 				}
// 			} else {
// 				// 清除data cache
// 				data_buffer = xa_load(&bt->dc, pcn);
// 				BUG_ON(!data_buffer);
// 				xa_erase(&bt->dc, pcn);
// 				// pr_err("!!Free data buffer, drive_idx %u, zone_idx %u, offset 0x%llx, pointer: 0x%px\n", drive_idx, zone_idx, wp_off, data_buffer);
// 				biza_mempool_free(&bt->dcpool, data_buffer);
// 			}

// Allocate write location
static inline bool biza_allocate_wp(struct biza_target *bt, uint8_t drive_idx,
				    uint8_t oz_idx, uint32_t zone_idx)
{
	struct biza_dev *dev = &bt->devs[drive_idx];
	struct biza_zone *zone = &dev->zones[zone_idx];
	uint64_t wp_off = (zone->wp - zone->start) >>
			  bt->params->chunk_size_sector_shift;
	// uint64_t wp_off = (atomic64_read(&zone->wp) - zone->start) >> bt->params->chunk_size_sector_shift;
	sector_t pcn, lcn;
	uint64_t stripe_no;
	biza_stripe_head_t *sh;
	int ret;

	// other worker is opening a new empty zone and it release the zone lock
	if (wp_off >= bt->params->zone_capacity_chunk)
		return false;

	// not in using & used
	pcn = biza_idx_to_pcn(bt, drive_idx, zone_idx, wp_off);
	lcn = biza_map_pcn_lookup_lcn(bt, pcn);
	zone->wp += bt->params->chunk_size_sector;
	// pr_err("drive_idx %u, zone_idx %u, zone_wp add, now: %llu\n", drive_idx, zone_idx, zone->wp);
	// atomic64_add(bt->params->chunk_size_sector, &zone->wp);

	if (zone->wp >= zone->start + zone->capacity) { // 这个zone使用完了
		zone->cond = BLK_ZONE_COND_FULL;
		// pr_err("drive_idx %u, zone_idx %u full\n", drive_idx, dev->open_zones[oz_idx]);
		spin_unlock_irq(&zone->zlock);

		up_read(&dev->ozlock);
		down_write(&dev->ozlock);
		while (atomic64_read(&zone->in_flight_ios) !=
		       atomic64_read(&zone->finished_ios)) {
			pr_err("Waiting for in flight io (%llu/%llu finished)\n",
			       atomic64_read(&zone->finished_ios),
			       atomic64_read(&zone->in_flight_ios));
			cpu_relax();
		}
		ret = biza_finish_zone(bt, dev, zone_idx);
		pr_err("finish zone %u\n", zone_idx);
		dev->open_zones[oz_idx] = biza_open_empty_zone(
			bt, dev, true,
			biza_oz_idx_to_aware_type(bt, drive_idx, oz_idx));
		pr_err("drive_idx %u, oz_idx %u open new zone %u\n", drive_idx,
		       oz_idx, dev->open_zones[oz_idx]);

		if (dev->open_zones[oz_idx] == dev->nr_zones)
			BUG_ON(1);
		downgrade_write(&dev->ozlock);

		zone_idx = dev->open_zones[oz_idx];
		zone = &dev->zones[zone_idx];
		spin_lock_irq(&zone->zlock);
	}

	return true;
}

// Find a empty zrwa enry for a active zone, return zrwa_size_chunk if none
// return ~((ulong) 0) if no empty
// need zone lock
static inline ulong biza_find_empty_zrwa_enry(struct biza_target *bt,
					      uint8_t drive_idx,
					      uint32_t zone_idx)
{
	struct biza_dev *dev = &bt->devs[drive_idx];
	struct biza_zone *zone = &dev->zones[zone_idx];
	uint64_t wp_off = (zone->wp - zone->start) >>
			  bt->params->chunk_size_sector_shift;
	// uint64_t wp_off = (atomic64_read(&zone->wp) - zone->start) >> bt->params->chunk_size_sector_shift;
	ulong bit_off = 0;
	sector_t pcn, lcn, left;

	left = min(dev->zrwa_size_chunk,
		   bt->params->zone_capacity_chunk - wp_off);
	while (bit_off < left) {
		// not in use
		bit_off = bitmap_find_next_zero_area(zone->zrwa_wd, left,
						     bit_off, 1, 0);
		if (bit_off >= left)
			break;
		// & not used, i.e., this entry has no valid data
		pcn = biza_idx_to_pcn(bt, drive_idx, zone_idx,
				      wp_off + bit_off);
		lcn = biza_map_pcn_lookup_lcn(bt, pcn);
		if (lcn == BIZA_MAP_UNMAPPED || lcn == BIZA_MAP_INVALID)
			break;
		bit_off++;
	}

	if (bit_off >= left)
		return ~((ulong)0);
	else
		return bit_off;
}

// test and clear the bit in zrwa window
static inline bool biza_test_and_clear_zrwa_bit(struct biza_target *bt,
						sector_t pcn, bool irq)
{
	uint8_t drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	struct biza_dev *dev;
	struct biza_zone *zone;
	uint64_t wp_off;
	ulong bit_off;
	bool org_bit;

	biza_pcn_to_idx(bt, pcn, &drive_idx, &zone_idx, &offset);
	dev = &bt->devs[drive_idx];
	zone = &dev->zones[zone_idx];

	if (irq)
		spin_lock_irq(&zone->zlock);
	else
		spin_lock(&zone->zlock);
	wp_off = (zone->wp - zone->start) >>
		 bt->params->chunk_size_sector_shift;
	// wp_off = (atomic64_read(&zone->wp) - zone->start) >> bt->params->chunk_size_sector_shift;
	bit_off = offset - wp_off;
	BUG_ON(wp_off + bit_off > bt->params->zone_capacity_chunk);
	org_bit = test_and_clear_bit(bit_off, zone->zrwa_wd);
	// pr_err("drive_idx %u, zone_idx %u, wp %llu, wp_off %llu, bit_off %lu, clear, cnt %u, zrwa %x\n", drive_idx, zone_idx, zone->wp, wp_off, bit_off, atomic_inc_return(&zone->debug_cnt), (uint16_t)*zone->zrwa_wd);
	if (irq)
		spin_unlock_irq(&zone->zlock);
	else
		spin_unlock(&zone->zlock);

	return org_bit;
}

// test and set the bit in zrwa window
static inline bool biza_test_and_set_zrwa_bit(struct biza_target *bt,
					      sector_t pcn)
{
	uint8_t drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	struct biza_dev *dev;
	struct biza_zone *zone;
	uint64_t wp_off;
	ulong bit_off;
	bool org_bit;

	biza_pcn_to_idx(bt, pcn, &drive_idx, &zone_idx, &offset);
	dev = &bt->devs[drive_idx];
	zone = &dev->zones[zone_idx];

	spin_lock_irq(&zone->zlock);
	wp_off = (zone->wp - zone->start) >>
		 bt->params->chunk_size_sector_shift;
	// wp_off = (atomic64_read(&zone->wp) - zone->start) >> bt->params->chunk_size_sector_shift;
	bit_off = offset - wp_off;
	BUG_ON(wp_off + bit_off > bt->params->zone_capacity_chunk);
	org_bit = test_and_set_bit(bit_off, zone->zrwa_wd);
	// pr_err("drive_idx %u, zone_idx %u, wp %llu, wp_off %llu, bit_off %lu, set, biza_test_and_set_zrwa_bit, cnt %u, zrwa %x\n", drive_idx, zone_idx, zone->wp, wp_off, bit_off, atomic_inc_return(&zone->debug_cnt), (uint16_t)*zone->zrwa_wd);
	spin_unlock_irq(&zone->zlock);

	return org_bit;
}

// Which zone to write?
// For parallel write
static bool biza_get_zone_write_location(struct biza_target *bt,
					 uint8_t drive_idx, uint8_t oz_idx,
					 uint32_t *zone_idx, uint64_t *offset)
{
	struct biza_dev *dev = &bt->devs[drive_idx];
	struct biza_zone *zone;
	uint64_t wp_off;
	while (1) {
		down_read(&dev->ozlock);
		*zone_idx = dev->open_zones[oz_idx];
		zone = &dev->zones[*zone_idx];
		while (zone->cond == BLK_ZONE_COND_FULL) {
			up_read(&dev->ozlock);
			udelay(1);
			continue;
		}

		spin_lock_irq(&zone->zlock);

		wp_off = (zone->wp - zone->start) >>
			 bt->params->chunk_size_sector_shift;

		biza_allocate_wp(bt, drive_idx, oz_idx, *zone_idx);
		*zone_idx = dev->open_zones[oz_idx];
		zone = &dev->zones[*zone_idx];
		atomic64_inc(&zone->in_flight_ios);

		// wp_off = (atomic64_read(&zone->wp) - zone->start) >> bt->params->chunk_size_sector_shift;
		spin_unlock_irq(&zone->zlock);
		up_read(&dev->ozlock);
		*offset = wp_off;

		return true;
	}
	return false;
}

// Get a write location in a zone
static inline void biza_get_write_location(struct biza_target *bt,
					   uint64_t hint, uint8_t drive_idx,
					   uint32_t *zone_idx, uint64_t *offset)
{
	uint8_t oz_idx;
	int ret;

	oz_idx = biza_choose_open_zone_to_write(bt, drive_idx, hint);
	ret = biza_get_zone_write_location(bt, drive_idx, oz_idx, zone_idx,
					   offset);
	BUG_ON(!ret);
}

// Get a write location in RAUM
static inline void biza_get_raum_write_location(struct biza_target *bt,
						uint64_t lcn,
						uint64_t stripe_no,
						uint8_t drive_idx, uint8_t slot,
						uint64_t *offset, bool parity)
{
	struct biza_raum_location_entry *oldest, *content;
	uint64_t raid_offset;
	uint32_t zone_idx;
	sector_t wp;
	sector_t pcn, old_lcn, old_pcn, chunk;
	biza_free_raum_chunk_t *free_chunk = NULL;
	struct biza_stripe *stripe = NULL;
	biza_stripe_head_t *sh;
	uint8_t *data_buffer;
	unsigned long flags;

	struct biza_raum_dev *dev = &bt->raum_devs[drive_idx];
	// spin_lock_irq(&dev->free_raum_chunks_lock);

	// If in RAUM, then just return the original location

	// Try get a location in that drive's RAUM area

	spin_lock_irqsave(&dev->lru_list_lock, flags);
	free_chunk = list_first_entry_or_null(&dev->free_raum_chunks,
					      biza_free_raum_chunk_t, link);
	if (!free_chunk) {
		// If full, evict an entry
		// print_flag = 1;
		// dump_stack();
		oldest = list_first_entry(
			&dev->lru_list, struct biza_raum_location_entry, link);
		chunk = oldest->raum_chunk;
		// spin_lock(&dev->lru_list_lock);
		list_del(&oldest->link);
		// list_add_tail(&free_chunk->link, &dev->free_raum_chunks);
		// spin_unlock(&dev->lru_list_lock);
		spin_unlock_irqrestore(&dev->lru_list_lock, flags);
		biza_get_write_location(bt, oldest->lcn, drive_idx, &zone_idx,
					&raid_offset);

		wp = biza_idx_to_sector(bt, drive_idx, zone_idx, raid_offset,
					true);
		// pr_err("Flushing loc: lcn 0x%llx, drive_idx: %u, target pcn: 0x%llx\n",
		//        oldest->lcn, drive_idx, wp);
		WARN_ONCE(1, "Start evict\n");
		pcn = biza_idx_to_pcn(bt, drive_idx, zone_idx, raid_offset);
		wp = biza_flush_raum_area(bt, drive_idx, zone_idx, wp, pcn,
					  bt->params->chunk_size_sector,
					  oldest->raum_chunk);

		pcn = wp >> bt->params->chunk_size_sector_shift;

		// pr_err("%s LRU Evicted to pcn 0x%llx, wp 0x%llx, oldest 0x%px\n",
		//        oldest->parity ? "parity" : "data", pcn, wp, oldest);
		stripe = xa_load(&bt->map->stripe_table, oldest->stripe_no);
		if (oldest->parity) {
			// Update mapping table for parity chunks
			// pr_err("pcn 0x%llx stripe 0x%px 0x%llx slot 0x%x parity_pcns 0x%px\n",
			//        pcn, stripe, oldest->stripe_no, oldest->slot,
			//        stripe->parity_pcns);
			old_pcn = stripe->parity_pcns[oldest->slot];
			// pr_err("old_pcn 0x%llx stripe 0x%px\n", old_pcn,
			//        stripe);
			bt->map->p2l[pcn].chunk_no = BIZA_MAP_PARITY;
			bt->map->p2l[pcn].stripe_no = oldest->stripe_no;
			bt->map->p2l[pcn].slot = oldest->slot;
			bt->map->p2l[pcn].in_raum = false;
			if (old_pcn != BIZA_MAP_INVALID &&
			    old_pcn != BIZA_MAP_UNMAPPED) {
				bt->map->p2l[old_pcn].chunk_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[old_pcn].stripe_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[old_pcn].slot =
					(uint8_t)BIZA_MAP_INVALID;
				bt->map->p2l[old_pcn].in_raum = false;
			}
			stripe->parity_pcns[oldest->slot] = pcn;

			sh = xa_load(&bt->fshc, oldest->stripe_no);
			// pr_err("pcn 0x%llx sh 0x%px\n", pcn, sh);
			if (sh) {
				// pr_err("!!Free parity buffer, pcn=0x%llx, sh->parity_cache=0x%px\n", pcn, sh->parity_cache);
				xa_erase(&bt->fshc, stripe_no);
				biza_free_stripe_head(bt, sh);
			}
			xa_erase(&bt->raum_parity, oldest->stripe_no);
		} else {
			// Update mapping table for data chunks
			old_lcn = stripe->data_lcns[oldest->slot];
			old_pcn = bt->map->l2p[old_lcn].chunk_no;
			bt->map->l2p[old_lcn].chunk_no = pcn;
			bt->map->l2p[old_lcn].stripe_no = oldest->stripe_no;
			bt->map->l2p[old_lcn].slot = oldest->slot;
			bt->map->l2p[old_lcn].in_raum = false;
			bt->map->p2l[pcn].chunk_no = old_lcn;
			bt->map->p2l[pcn].stripe_no = oldest->stripe_no;
			bt->map->p2l[pcn].slot = oldest->slot;
			bt->map->p2l[pcn].in_raum = false;
			bt->map->p2l[old_pcn].chunk_no = BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].stripe_no = BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].slot = (uint8_t)BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].in_raum = false;
			// pr_err("%s LRU Evicted to pcn 0x%llx, old_lcn 0x%llx, old_pcn 0x%llx, biza_entry old_lcn 0x%llx, biza_entry old_pcn 0x%llx, wp 0x%llx, oldest 0x%px\n",
			//        oldest->parity ? "parity" : "data", pcn, old_lcn,
			//        old_pcn, oldest->lcn, oldest->raum_chunk, wp,
			//        oldest);
			data_buffer = xa_load(&bt->dc, old_pcn);
			BUG_ON(!data_buffer);
			xa_erase(&bt->dc, old_pcn);
			// pr_err("!!Free data buffer, drive_idx %u, zone_idx %u, offset 0x%llx, pointer: 0x%px\n", drive_idx, zone_idx, wp_off, data_buffer);
			biza_mempool_free(&bt->dcpool, data_buffer);
			xa_erase(&bt->raum_data, oldest->lcn);
		}

		kfree(oldest);
	} else {
		chunk = free_chunk->chunk;
		list_del(&free_chunk->link);
		kfree(free_chunk);
		spin_unlock_irqrestore(&dev->lru_list_lock, flags);
	}

	content = kzalloc(sizeof(struct biza_raum_location_entry), GFP_KERNEL);

	if (!content) {
		BUG_ON(1);
	}

	content->parity = parity;
	content->stripe_no = stripe_no;
	content->lcn = lcn;
	content->slot = slot;
	content->drive_idx = drive_idx;
	spin_lock_irqsave(&dev->lru_list_lock, flags);
	content->raum_chunk = chunk;
	*offset = chunk;

	if (parity) {
		xa_store(&bt->raum_parity, stripe_no, content,
			 GFP_NOWAIT | GFP_NOIO);
	} else {
		xa_store(&bt->raum_data, lcn, content, GFP_NOWAIT | GFP_NOIO);
	}

	// spin_lock(&dev->lru_list_lock);

	list_add_tail(&content->link, &dev->lru_list);
	spin_unlock_irqrestore(&dev->lru_list_lock, flags);
	// spin_unlock(&dev->lru_list_lock);

	// spin_unlock_irq(&dev->free_raum_chunks_lock);
}

// alloc a stripe head io ctx
struct biza_stripe_head_ioctx *
biza_alloc_stripe_head_ioctx(struct biza_target *bt, uint8_t data_wrt_cnt)
{
	struct biza_stripe_head_ioctx *shioctx = NULL;

	shioctx = kzalloc(sizeof(struct biza_stripe_head_ioctx), GFP_KERNEL);
	if (!shioctx)
		goto err;

	shioctx->data_pcns =
		kvzalloc(data_wrt_cnt * sizeof(sector_t), GFP_KERNEL);
	if (!shioctx->data_pcns)
		goto err_shioctx;

	shioctx->parity_pcns =
		kvzalloc(bt->params->m * sizeof(sector_t), GFP_KERNEL);
	if (!shioctx->parity_pcns)
		goto err_data;

	refcount_set(&shioctx->ref, 1);

	return shioctx;

err_data:
	kvfree(shioctx->data_pcns);
err_shioctx:
	kfree(shioctx);
err:
	return NULL;
}

// Free stripe head io ctx
inline void biza_free_stripe_head_ioctx(struct biza_stripe_head_ioctx *shioctx)
{
	kvfree(shioctx->parity_pcns);
	kvfree(shioctx->data_pcns);
	kfree(shioctx);
}

// can the data of pcn be updated in place (i.e., in ZRWA window & not in use)
static bool biza_can_chunk_update_in_place(struct biza_target *bt, uint64_t pcn)
{
	uint8_t drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	struct biza_dev *dev;
	struct biza_zone *zone;
	uint64_t wp_off;
	ulong bit_off;
	bool ret;

	biza_pcn_to_idx(bt, pcn, &drive_idx, &zone_idx, &offset);
	dev = &bt->devs[drive_idx];
	zone = &dev->zones[zone_idx];

	spin_lock_irq(&zone->zlock);
	wp_off = (zone->wp - zone->start) >>
		 bt->params->chunk_size_sector_shift;
	// wp_off = (atomic64_read(&zone->wp) - zone->start) >> bt->params->chunk_size_sector_shift;
	bit_off = offset - wp_off;
	if (wp_off > offset)
		ret = false;
	else if (bit_off > dev->zrwa_size_chunk)
		ret = false;
	else
		ret = !test_bit(bit_off, zone->zrwa_wd);
	spin_unlock_irq(&zone->zlock);

	return ret;
}

// can the data be updated in place?
static bool biza_can_raum_data_update_in_place(struct biza_target *bt,
					       uint64_t lcn)
{
	struct biza_raum_location_entry *entry = xa_load(&bt->raum_data, lcn);
	// biza_raum_lru_htable_find(bt, lcn, false);
	if (entry) {
		return true;
	}
	return false;
}

// can the parity be updated in place?
static inline bool biza_can_raum_parity_update_in_place(struct biza_target *bt,
							uint64_t chunk_no)
{
	struct biza_raum_location_entry *entry =
		// biza_raum_lru_htable_find(bt, chunk_no, true);
		xa_load(&bt->raum_parity, chunk_no);
	if (entry) {
		return true;
	}
	return false;
}

// pin the zrwa (data and parities) for date update in place & get sh
static biza_stripe_head_t *biza_data_update_get_sh(struct biza_target *bt,
						   uint64_t lcn)
{
	sector_t pcn;
	uint64_t stripe_no;
	biza_stripe_head_t *sh = NULL;
	struct biza_stripe *stripe;
	int i, j;

	stripe_no = biza_map_lcn_lookup_stripe_no(bt, lcn);
	sh = biza_get_stripe_head_with_no(bt, stripe_no);
	if (!sh)
		return NULL;

	sh->ioctx = biza_alloc_stripe_head_ioctx(bt, 1);

	pcn = biza_map_lcn_lookup_pcn(bt, lcn);
	stripe = biza_map_lcn_lookup_stripe(bt, lcn);
	if (!stripe)
		goto fail_sh;

	if (!biza_is_valid_pcn(bt, pcn)) {
		pr_err("Failed lcn: 0x%llx", lcn);
		goto fail_sh;
	}
	sh->ioctx->data_pcns[0] = pcn;

	for (i = 0; i < bt->params->m; ++i) {
		pcn = stripe->parity_pcns[i];
		if (!biza_is_valid_pcn(bt, pcn)) {
			pcn = sh->ioctx->data_pcns[0];
			for (j = 0; j < i; ++j) {
				pcn = stripe->parity_pcns[j];
			}

			goto fail_sh;
		}
		sh->ioctx->parity_pcns[i] = pcn;
	}

	return sh;

fail_sh:
	pr_err("Fail SH!");
	if (sh->nr_data_written == bt->params->k) {
		xa_store_irq(&bt->fshc, sh->no, sh, GFP_KERNEL);
	} else {
		spin_lock_irq(&bt->pshl_lock);
		list_add_tail(&sh->link, &bt->pshl);
		spin_unlock_irq(&bt->pshl_lock);
	}
	biza_free_stripe_head_ioctx(sh->ioctx);
	return NULL;
}

// Target BIO completion.
inline void biza_bio_endio(struct bio *bio, blk_status_t status)
{
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));

	if (status != BLK_STS_OK && bio->bi_status == BLK_STS_OK)
		bio->bi_status = status;

	if (refcount_dec_and_test(&bioctx->ref)) {
		bio_endio(bio);
	}
}

// A stripe head is completed
static void stripe_head_endio(biza_stripe_head_t *sh)
{
	struct biza_stripe_head_ioctx *shioctx = sh->ioctx;
	struct bio *bio = shioctx->bio;
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	struct biza_target *bt = bioctx->bt;
	blk_status_t status = shioctx->status;

	// Update mapping tables
	if (shioctx->type == BIZA_SH_WRITE) {
		sh->nr_data_written += shioctx->data_wrt_cnt;
		BUG_ON(sh->nr_data_written > bt->params->k);
	}

	if (sh->nr_data_written == bt->params->k) {
		/** Add to another list. Release until ZRWA window has slided left. **/
		sh->ioctx = NULL;
		xa_store_irq(&bt->fshc, sh->no, sh, GFP_ATOMIC);
	} else {
		// May deadlock without _IRQ?
		spin_lock(&bt->pshl_lock);
		sh->ioctx = NULL;
		list_add_tail(&sh->link, &bt->pshl);
		spin_unlock(&bt->pshl_lock);
	}

	biza_free_stripe_head_ioctx(shioctx);

	biza_bio_endio(bio, status);
}

static inline void biza_end_stripe_head_io(biza_stripe_head_t *sh,
					   blk_status_t status)
{
	struct biza_stripe_head_ioctx *shioctx = sh->ioctx;

	if (status != BLK_STS_OK && shioctx->status == BLK_STS_OK)
		shioctx->status = status;

	if (refcount_dec_and_test(&shioctx->ref)) {
		stripe_head_endio(sh);
	}
}

void biza_chunkio_endio(struct bio *chunkio)
{
	struct biza_chunkioctx *chunkioctx = chunkio->bi_private;
	struct bio *bio;
	biza_stripe_head_t *sh;
	blk_status_t status = chunkio->bi_status;
	uint8_t drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	sector_t bi_sector;
	sector_t old_pcn;
	sector_t lcn;
	int ret;

	if (chunkioctx->type == BIZA_DATA_WRITE ||
	    chunkioctx->type == BIZA_PARITY_WRITE ||
	    chunkioctx->type == BIZA_DATA_UPDATE ||
	    chunkioctx->type == BIZA_PARITY_UPDATE) {
		sh = chunkioctx->sh;

		if (unlikely(status != BLK_STS_OK)) {
			// pr_err("dm-biza: io failed! io_type %d, bi_status %d, offset %lld, sectors %u",
			//        bio_op(chunkio), status, chunkio->bi_iter.bi_sector,
			//        bio_sectors(chunkio));
			pr_err("dm-biza: io failed! bi_status %u lcn: 0x%llx, io_type %d, bi_status %d, offset 0x%llx, sectors %u\n",
			       status, chunkioctx->lcn, bio_op(chunkio), status,
			       chunkio->bi_iter.bi_sector,
			       bio_sectors(chunkio));

			BUG_ON(1);
		}

		old_pcn = chunkioctx->pcn;

		if (!chunkioctx->in_raum) {
			bi_sector = chunkio->bi_iter.bi_sector;
			chunkioctx->pcn = biza_sector_to_pcn(
				chunkioctx->bt, chunkioctx->drive_idx,
				bi_sector);
			biza_pcn_to_idx(chunkioctx->bt, chunkioctx->pcn,
					&drive_idx, &zone_idx, &offset);
			atomic64_inc(&chunkioctx->bt->devs[drive_idx]
					      .zones[zone_idx]
					      .finished_ios);
			// if (chunkioctx->raum_flush_to_zone) {
			// 	pr_err("flushed lcn: 0x%llx, old pcn: 0x%llx, new_pcn: 0x%llx, stripe_no: 0x%llx, slot: %u, in_raum: %d, sector: 0x%llx\n",
			// 	       chunkioctx->lcn, old_pcn,
			// 	       chunkioctx->pcn, sh->no,
			// 	       chunkioctx->slot, chunkioctx->in_raum,
			// 	       bi_sector);
			// }
		}

		if (chunkioctx->type == BIZA_DATA_WRITE) {
			biza_map_update_data_wrt(chunkioctx->bt,
						 chunkioctx->lcn,
						 chunkioctx->pcn, sh->no,
						 chunkioctx->slot,
						 chunkioctx->in_raum);
		} else if (chunkioctx->type == BIZA_PARITY_WRITE) {
			biza_map_update_parity_wrt(chunkioctx->bt,
						   chunkioctx->pcn, sh->no,
						   chunkioctx->slot,
						   chunkioctx->in_raum);
		}

		// ret = biza_test_and_clear_zrwa_bit(chunkioctx->bt,
		// 				   chunkioctx->pcn, false);
		// if (!ret) {
		// 	biza_pcn_to_idx(chunkioctx->bt, chunkioctx->pcn,
		// 			&drive_idx, &zone_idx, &offset);
		// 	BUG_ON(1);
		// }

		if (!chunkioctx->in_raum) {
			biza_gc_avoid_stat(chunkioctx);
		}
		kfree(chunkioctx);
		bio_put(chunkio);

		biza_end_stripe_head_io(sh, status);
	} else if (chunkioctx->type == BIZA_DATA_READ) {
		bio = chunkioctx->bio;

		kfree(chunkioctx);
		bio_put(chunkio);

		biza_bio_endio(bio, status);
	} else
		BUG_ON(1);
}

// Send stripe I/O to SSDs
static int biza_submit_stripe_head_write(struct biza_target *bt,
					 struct bio *bio,
					 biza_stripe_head_t *sh,
					 uint8_t chunk_cnt, uint8_t type)
{
	// pr_err("enter: type:%u chunks:%u bio=0x%px bt=0x%px bi_size=%u vcnt=%u idx=%u done=%u\n",
	//    type, chunk_cnt, bio, bt, bio->bi_iter.bi_size, bio->bi_vcnt,
	//    bio->bi_iter.bi_idx, bio->bi_iter.bi_bvec_done);
	struct biza_stripe_head_ioctx *shioctx = sh->ioctx;
	struct bio *chunkio;
	struct biza_chunkioctx *chunkioctx;
	uint8_t drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	sector_t lcn, pcn;
	uint8_t *data_buffer, *bvec_start;
	struct biza_raum_dev *raum_dev;
	int i, ret;
	struct bvec_iter iter;
	struct bio_vec bvec;
	struct biza_raum_location_entry *entry;
	unsigned long flags;

	struct block_device *bdev;
	sector_t bi_sector;

	bool data_in_raum;

	enum biza_aware_type aware_type;

	BUG_ON(shioctx == NULL);

	// send data chunk I/O
	for (i = 0; i < chunk_cnt; ++i) {
		lcn = sh->ioctx->lcn_start + i;

		// update
		biza_update_pred(bt, lcn);

		// Data update in place
		if (shioctx->type == BIZA_SH_IN_PLACE_UPDATE) {
			// pcn = sh->ioctx->data_pcns[0];
			// BUG_ON(pcn == BIZA_MAP_UNMAPPED ||
			//        pcn == BIZA_MAP_INVALID);
			// biza_pcn_to_idx(bt, pcn, &drive_idx, &zone_idx,
			// 		&offset);
			data_in_raum = true;
			entry = xa_load(&bt->raum_data, lcn);
			drive_idx = entry->drive_idx;
			offset = entry->raum_chunk;

			raum_dev = &bt->raum_devs[drive_idx];
			// spin_lock(&raum_dev->lru_list_lock);
			spin_lock_irqsave(&raum_dev->lru_list_lock, flags);
			list_del(&entry->link);
			list_add_tail(&entry->link, &raum_dev->lru_list);
			spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
			// spin_unlock(&raum_dev->lru_list_lock);

			bdev = raum_dev->bdev;
			bi_sector = biza_raum_idx_to_sector(bt, offset);
			pcn = biza_raum_idx_to_pcn(bt, drive_idx, offset);

			log("Writing RAUM in-place lcn 0x%llx pcn 0x%llx drive %u, offset 0x%llx sector 0x%llx shno 0x%llx\n",
			    lcn, pcn, drive_idx, offset, bi_sector, sh->no);

			if (WRITE_AMP_STAT)
				atomic64_add(bt->params->chunk_size_sector,
					     &bt->data_in_place_update);
		} else {
			drive_idx = (sh->nr_data_written + i + sh->no +
				     bt->params->m) %
				    bt->params->nr_drives;
			// TODO: check if is ZRWA aware zone
			// If so, write to RAUM

			aware_type = biza_check_aware_type(bt, lcn);
			if (aware_type == BIZA_ZRWA_AWARE) {
				// write to RAUM

				data_in_raum = true;
				raum_dev = &bt->raum_devs[drive_idx];
				bdev = raum_dev->bdev;
				// store data in cache
				biza_get_raum_write_location(
					bt, lcn, sh->no, drive_idx,
					sh->nr_data_written + i, &offset,
					false);
				pcn = biza_raum_idx_to_pcn(bt, drive_idx,
							   offset);
				bi_sector = biza_raum_idx_to_sector(bt, offset);
				log("Writing RAUM ZRWA lcn 0x%llx pcn 0x%llx drive %u, offset 0x%llx sector 0x%llx shno 0x%llx\n",
				    lcn, pcn, drive_idx, offset, bi_sector,
				    sh->no);

				// data_in_raum = false;
				// bdev = bt->devs[drive_idx].dev->bdev;
				// biza_get_write_location(bt, lcn, drive_idx,
				// 			&zone_idx, &offset);
				// pcn = biza_idx_to_pcn(bt, drive_idx, zone_idx,
				// 		      offset);
				// bi_sector = biza_idx_to_sector(
				// 	bt, drive_idx, zone_idx, offset, true);
				// pr_err("Writing zone lcn 0x%llx pcn 0x%llx drive %u zone %u offset 0x%llx sector 0x%llx\n",
				//        lcn, pcn, drive_idx, zone_idx, offset,
				//        bi_sector);

			} else {
				// write to flash
				data_in_raum = false;
				bdev = bt->devs[drive_idx].dev->bdev;
				biza_get_write_location(bt, lcn, drive_idx,
							&zone_idx, &offset);
				pcn = biza_idx_to_pcn(bt, drive_idx, zone_idx,
						      offset);
				bi_sector = biza_idx_to_sector(
					bt, drive_idx, zone_idx, offset, true);
				log("Writing zone lcn 0x%llx pcn 0x%llx drive %u zone %u offset 0x%llx sector 0x%llx shno 0x%llx\n",
				    lcn, pcn, drive_idx, zone_idx, offset,
				    bi_sector, sh->no);
			}

			if (WRITE_AMP_STAT)
				atomic64_add(bt->params->chunk_size_sector,
					     &bt->data_write);
		}

		if (data_in_raum) {
			data_buffer = biza_mempool_alloc(&bt->dcpool);
			// pr_err("alloc data buffer, drive_idx %u, zone_idx %u, offset %llu, lcn %llu\n", drive_idx, zone_idx, offset, lcn);
			if (!data_buffer)
				BUG_ON(1);
			iter = bio->bi_iter;
			bvec = bio_iter_iovec(bio, iter);
			bvec_start = bvec_kmap_local(&bvec);
			memcpy(data_buffer, bvec_start,
			       bt->params->chunk_size_byte);

			kunmap_local(bvec_start);
			xa_store(&bt->dc, pcn, data_buffer, GFP_KERNEL);
		}

		// TODO: when zone append, open a new zone if we know we are writing to the end of the pointer
		// Using previously known pointer location of that zone

		chunkio = bio_clone_fast(bio, GFP_NOIO, &bt->bio_set);
		if (!chunkio)
			return -ENOMEM;

		bio_set_dev(chunkio, bdev);
		if (!data_in_raum) {
			chunkio->bi_opf = REQ_OP_ZONE_APPEND |
					  (bio->bi_opf & ~REQ_OP_MASK);
		}

		chunkio->bi_iter.bi_sector = bi_sector;
		chunkio->bi_iter.bi_size = bt->params->chunk_size_byte;
		chunkio->bi_end_io = biza_chunkio_endio;

		chunkioctx = kzalloc(sizeof(struct biza_chunkioctx), GFP_NOIO);
		if (!chunkioctx)
			return -ENOMEM;
		chunkioctx->sh = sh;
		chunkioctx->type = shioctx->type == BIZA_SH_IN_PLACE_UPDATE ?
					   BIZA_DATA_UPDATE :
					   BIZA_DATA_WRITE;
		chunkioctx->stime = jiffies;
		chunkioctx->bt = bt;
		chunkioctx->lcn = lcn;
		chunkioctx->pcn = pcn;
		chunkioctx->drive_idx = drive_idx;
		chunkioctx->slot = sh->nr_data_written + i;
		chunkioctx->in_raum = data_in_raum;
		chunkioctx->raum_flush_to_zone = false;

		chunkio->bi_private = chunkioctx;

		shioctx->data_pcns[i] = pcn;
		refcount_inc(&shioctx->ref);

		submit_bio_noacct(chunkio);

		bio_advance(bio, bt->params->chunk_size_byte);
	}

	// send parity chunk I/O
	for (i = 0; i < bt->params->m; ++i) {
		if (sh->nr_data_written > 0) { // try in place update
			if (biza_can_raum_parity_update_in_place(bt, sh->no)) {
				entry = xa_load(&bt->raum_parity, sh->no);
				// entry = biza_raum_lru_htable_find(bt, sh->no,
				// 				  true);
				drive_idx = entry->drive_idx;
				offset = entry->raum_chunk;
				raum_dev = &bt->raum_devs[drive_idx];
				spin_lock_irqsave(&raum_dev->lru_list_lock,
						  flags);
				// spin_lock(&raum_dev->lru_list_lock);
				list_del(&entry->link);
				list_add_tail(&entry->link,
					      &raum_dev->lru_list);
				// spin_unlock(&raum_dev->lru_list_lock);
				spin_unlock_irqrestore(&raum_dev->lru_list_lock,
						       flags);
				bi_sector = biza_raum_idx_to_sector(bt, offset);
				pcn = biza_raum_idx_to_pcn(bt, drive_idx,
							   offset);
				log("Writing RAUM parity in-place lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
				    lcn, pcn, drive_idx, offset, bi_sector);

				if (WRITE_AMP_STAT)
					atomic64_add(
						bt->params->chunk_size_sector,
						&bt->parity_in_place_update);
			} else {
				// pr_err("out of place paritial parity update 1\n");
				drive_idx =
					(sh->no + i) % bt->params->nr_drives;
				biza_get_raum_write_location(
					bt, sh->ioctx->lcn_start, sh->no,
					drive_idx, i, &offset, true);
				// biza_get_write_location(bt,
				// 			sh->ioctx->lcn_start,
				// 			drive_idx, &zone_idx,
				// 			&offset);
				bi_sector = biza_raum_idx_to_sector(bt, offset);
				pcn = biza_raum_idx_to_pcn(bt, drive_idx,
							   offset);
				log("Writing RAUM parity OOP lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
				    lcn, pcn, drive_idx, offset, bi_sector);

				if (WRITE_AMP_STAT)
					atomic64_add(
						bt->params->chunk_size_sector,
						&bt->parity_write);
			}
		} else {
			drive_idx = (sh->no + i) % bt->params->nr_drives;
			biza_get_raum_write_location(bt, sh->ioctx->lcn_start,
						     sh->no, drive_idx, i,
						     &offset, true);
			bi_sector = biza_raum_idx_to_sector(bt, offset);
			pcn = biza_raum_idx_to_pcn(bt, drive_idx, offset);
			log("Writing RAUM parity else lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
			    lcn, pcn, drive_idx, offset, bi_sector);

			if (WRITE_AMP_STAT)
				atomic64_add(bt->params->chunk_size_sector,
					     &bt->parity_write);
		}

		// pr_err("parity drive_idx=%u, offset=0x%llx, bi_sector=0x%llx, pcn=0x%llx\n",
		//        drive_idx, offset, bi_sector, pcn);

		chunkio = bio_alloc_bioset(GFP_NOIO, 1, &bt->bio_set);
		if (!chunkio)
			return -ENOMEM;

		bio_set_op_attrs(chunkio, REQ_OP_WRITE, bio->bi_opf);
		ret = bio_add_page(
			chunkio,
			virt_to_page(sh->parity_cache +
				     i * bt->params->chunk_size_byte),
			bt->params->chunk_size_byte, 0);
		if (ret != bt->params->chunk_size_byte)
			return -EIO;

		bio_set_dev(chunkio, bt->raum_devs[drive_idx].bdev);
		chunkio->bi_iter.bi_sector = bi_sector;
		chunkio->bi_iter.bi_size = bt->params->chunk_size_byte;
		chunkio->bi_end_io = biza_chunkio_endio;

		chunkioctx = kzalloc(sizeof(struct biza_chunkioctx), GFP_NOIO);
		if (!chunkioctx)
			return -ENOMEM;
		chunkioctx->sh = sh;
		chunkioctx->type = shioctx->type == BIZA_SH_IN_PLACE_UPDATE ?
					   BIZA_PARITY_UPDATE :
					   BIZA_PARITY_WRITE;
		;
		chunkioctx->bt = bt;
		chunkioctx->lcn = BIZA_MAP_PARITY;
		chunkioctx->pcn = pcn;
		chunkioctx->drive_idx = drive_idx;
		chunkioctx->slot = i;
		// We always write parity chunks to RAUM
		// Even if it has been previously evicted to flash
		chunkioctx->in_raum = true;
		chunkioctx->raum_flush_to_zone = false;

		chunkio->bi_private = chunkioctx;

		shioctx->parity_pcns[i] = pcn;
		refcount_inc(&shioctx->ref);

		submit_bio_noacct(chunkio);
	}

	biza_end_stripe_head_io(sh, BLK_STS_OK);

	return 0;
}

// Process write request of a full stripe
static int biza_handle_full_stripe_write(struct biza_target *bt,
					 struct bio *bio)
{
	biza_stripe_head_t *sh;
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	int ret;

	sh = biza_alloc_empty_stripe_head(bt);
	if (!sh) {
		pr_err("dm-biza: io error: cannot alloc empty stripe");
		return -ENOMEM;
	}
	sh->ioctx = biza_alloc_stripe_head_ioctx(bt, bt->params->k);
	if (!sh->ioctx) {
		pr_err("dm-biza: io error: cannot alloc stripe head ioctx");
		return -ENOMEM;
	}
	sh->ioctx->bio = bio;
	sh->ioctx->data_wrt_cnt = bt->params->k;
	sh->ioctx->lcn_start = bio->bi_iter.bi_sector >>
			       bt->params->chunk_size_sector_shift;
	sh->ioctx->type = BIZA_SH_WRITE;
	refcount_inc(&bioctx->ref);

	ret = biza_compute_parity(bt, bio, sh, bt->params->k);
	if (ret) {
		pr_err("dm-biza: io error: compute parity error");
		return -EIO;
	}

	ret = biza_submit_stripe_head_write(bt, bio, sh, bt->params->k, 1);
	if (ret) {
		pr_err("dm-biza: io error: cannot submit full stripe write");
		return -EIO;
	}

	return 0;
}

// Process write request of a partial stripe
static int biza_handle_partial_stripe_write(struct biza_target *bt,
					    struct bio *bio, uint8_t chunk_cnt)
{
	biza_stripe_head_t *sh;
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	uint8_t nr_data_write;
	int ret;

	while (chunk_cnt > 0) {
		sh = biza_get_partial_stripe_head(bt);
		if (!sh) {
			pr_err("dm-biza: io error: cannot get partial stripe");
			return -ENOMEM;
		}

		nr_data_write = min(chunk_cnt, (uint8_t)(bt->params->k -
							 sh->nr_data_written));
		sh->ioctx = biza_alloc_stripe_head_ioctx(bt, nr_data_write);
		if (!sh->ioctx) {
			pr_err("dm-biza: io error: cannot alloc stripe head ioctx");
			return -ENOMEM;
		}
		sh->ioctx->bio = bio;
		sh->ioctx->data_wrt_cnt = nr_data_write;
		sh->ioctx->lcn_start = bio->bi_iter.bi_sector >>
				       bt->params->chunk_size_sector_shift;
		sh->ioctx->type = BIZA_SH_WRITE;
		refcount_inc(&bioctx->ref);

		ret = biza_compute_parity(bt, bio, sh, nr_data_write);
		if (ret) {
			pr_err("dm-biza: io error: compute parity error");
			return -ENOMEM;
		}

		ret = biza_submit_stripe_head_write(bt, bio, sh, nr_data_write,
						    2);
		if (ret) {
			pr_err("dm-biza: io error: cannot submit paritial stripe write");
			return -EIO;
		}

		chunk_cnt -= nr_data_write;
	}

	return 0;
}

// Process in place update write request
static int biza_handle_data_in_place_update(struct biza_target *bt,
					    struct bio *bio,
					    biza_stripe_head_t *sh)
{
	void **srcs = kzalloc(sizeof(uint64_t *), GFP_KERNEL);
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	sector_t lcn, pcn;
	uint8_t *org_data, *bvec_start;
	int ret;

	lcn = bio->bi_iter.bi_sector >> bt->params->chunk_size_sector_shift;
	pcn = biza_map_lcn_lookup_pcn(bt, lcn);

	org_data = xa_load(&bt->dc, pcn);
	BUG_ON(!org_data);

	if (!sh->ioctx)
		BUG_ON(1);
	sh->ioctx->bio = bio;
	sh->ioctx->data_wrt_cnt = 1;
	sh->ioctx->lcn_start = lcn;
	sh->ioctx->type = BIZA_SH_IN_PLACE_UPDATE;
	refcount_inc(&bioctx->ref);

	/** recompute parity **/
	BUG_ON(bt->params->m != 1);
	// P' = P + D + D'
	// P + D = D^ (result in P)
	srcs[0] = org_data;
	xor_blocks(1, bt->params->chunk_size_byte, sh->parity_cache, srcs);

	// compute D^ + D' = P'
	bvec_start = bvec_kmap_local(&bio->bi_io_vec[0]);
	srcs[0] = bvec_start + bio->bi_iter.bi_bvec_done;
	xor_blocks(1, bt->params->chunk_size_byte, sh->parity_cache, srcs);

	// update data_buffer
	memcpy(org_data, srcs[0], bt->params->chunk_size_byte);
	kunmap_local(bvec_start);

	kfree(srcs);

	/** submit stripe head **/
	ret = biza_submit_stripe_head_write(bt, bio, sh, 1, 3);
	if (ret) {
		pr_err("dm-biza: io error: cannot submit data update in place");
		return -EIO;
	}

	return 0;
}

// Process a write request
static int biza_handle_write(struct biza_target *bt, struct bio *bio)
{
	sector_t left, cur_lcn;
	uint8_t chunk_cnt;
	biza_stripe_head_t *sh;
	int ret;

	if (bio_sectors(bio) % bt->params->chunk_size_sector ||
	    bio->bi_iter.bi_sector % bt->params->chunk_size_sector) {
		pr_crit("bio_sectors(bio) = 0x%x, bt->params->chunk_size_sector = 0x%llx, mod = 0x%llx\n",
			bio_sectors(bio), bt->params->chunk_size_sector,
			bio_sectors(bio) % bt->params->chunk_size_sector);
		pr_crit("bio->bi_iter.bi_sector = 0x%llx, bt->params->chunk_size_sector = 0x%llx, mod = 0x%llx\n",
			bio->bi_iter.bi_sector, bt->params->chunk_size_sector,
			bio->bi_iter.bi_sector % bt->params->chunk_size_sector);
		BUG_ON(1);
	}

	left = bio_sectors(bio) >> bt->params->chunk_size_sector_shift;

	if (WRITE_AMP_STAT)
		atomic64_add(bio_sectors(bio), &bt->user_send);

	while (left > 0) {
		cur_lcn = bio->bi_iter.bi_sector >>
			  bt->params->chunk_size_sector_shift;

		if (biza_can_raum_data_update_in_place(bt, cur_lcn)) {
			sh = biza_data_update_get_sh(bt, cur_lcn);
			if (sh) {
				ret = biza_handle_data_in_place_update(bt, bio,
								       sh);
				if (ret)
					return -EIO;

				left = bio_sectors(bio) >>
				       bt->params->chunk_size_sector_shift;
				continue;
			}
		}

		chunk_cnt = 0;

		// while (chunk_cnt < left && chunk_cnt < bt->params->k) {
		while (chunk_cnt < left && chunk_cnt < bt->params->k &&
		       !biza_can_raum_data_update_in_place(bt, cur_lcn)) {
			cur_lcn++;
			chunk_cnt++;
		}

		if (chunk_cnt == bt->params->k) {
			ret = biza_handle_full_stripe_write(bt, bio);
			if (ret)
				return -EIO;
		} else if (chunk_cnt > 0 && chunk_cnt < bt->params->k) {
			ret = biza_handle_partial_stripe_write(bt, bio,
							       chunk_cnt);
			if (ret)
				return -EIO;
		} else
			BUG_ON(chunk_cnt != 0);
		left = bio_sectors(bio) >> bt->params->chunk_size_sector_shift;
	}

	return 0;
}

// Send chunk I/O to SSD
static int biza_submit_chunk_read(struct biza_target *bt, struct bio *bio,
				  sector_t lcn, sector_t pcn, sector_t size)
{
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	struct bio *chunkio;
	struct biza_chunkioctx *chunkioctx;
	uint8_t drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	sector_t bi_sector;
	struct block_device *bdev;

	if (pcn == BIZA_MAP_UNMAPPED || pcn == BIZA_MAP_INVALID) {
		swap(bio->bi_iter.bi_size, size);
		zero_fill_bio(bio);
		swap(bio->bi_iter.bi_size, size);
	} else {
		if (biza_check_pcn_in_raum(bt, pcn)) {
			biza_raum_pcn_to_idx(bt, pcn, &drive_idx, &offset);
			bi_sector = biza_raum_idx_to_sector(bt, offset);
			bdev = bt->raum_devs[drive_idx].bdev;
			log("Reading RAUM lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
			    lcn, pcn, drive_idx, offset, bi_sector);
		} else {
			biza_pcn_to_idx(bt, pcn, &drive_idx, &zone_idx,
					&offset);
			bi_sector = biza_idx_to_sector(bt, drive_idx, zone_idx,
						       offset, false);
			log("Reading zone lcn 0x%llx pcn 0x%llx drive %u zone %u, offset 0x%llx sector 0x%llx\n",
			    lcn, pcn, drive_idx, zone_idx, offset, bi_sector);
			bdev = bt->devs[drive_idx].dev->bdev;
		}

		chunkio = bio_clone_fast(bio, GFP_NOIO, &bt->bio_set);
		if (!chunkio)
			return -ENOMEM;

		bio_set_dev(chunkio, bdev);
		chunkio->bi_iter.bi_sector = bi_sector;
		chunkio->bi_iter.bi_size = size;
		chunkio->bi_end_io = biza_chunkio_endio;

		chunkioctx = kzalloc(sizeof(struct biza_chunkioctx), GFP_NOIO);
		if (!chunkioctx)
			return -ENOMEM;
		chunkioctx->bio = bio;
		chunkioctx->type = BIZA_DATA_READ;
		chunkioctx->bt = bt;
		chunkioctx->pcn = pcn;
		chunkioctx->drive_idx = drive_idx;

		chunkio->bi_private = chunkioctx;

		refcount_inc(&bioctx->ref);
		submit_bio_noacct(chunkio);
	}

	bio_advance(bio, size);

	return 0;
}

// Process a read request
static int biza_handle_read(struct biza_target *bt, struct bio *bio)
{
	sector_t cur_sec, nxt_sec, size, left;
	sector_t lcn, pcn;
	int ret;

	left = bio_sectors(bio);

	while (left > 0) {
		cur_sec = bio->bi_iter.bi_sector;
		// e.g., chunk = 128 sec, 0->128, 32->128
		nxt_sec = min(round_up(cur_sec + 1,
				       bt->params->chunk_size_sector),
			      bio_end_sector(bio));
		size = (nxt_sec - cur_sec) << SECTOR_SHIFT;

		lcn = cur_sec >> bt->params->chunk_size_sector_shift;
		pcn = biza_map_lcn_lookup_pcn(bt, lcn);

		ret = biza_submit_chunk_read(bt, bio, lcn, pcn, size);
		if (ret) {
			pr_err("dm-biza: io error: cannot submit chunk read");
			return -EIO;
		}

		left = bio_sectors(bio);
	}

	return 0;
}

// Entry of I/O handling
static void biza_handle_bio(struct biza_target *bt, struct bio *bio)
{
	enum req_opf op;
	int ret;
	unsigned long flags;

	if (bio->bi_vcnt > 1) {
		/** TODO: support bi_vcnt > 1 **/
		pr_err("dm-biza: map error: bvec cnt > 1 %d", bio->bi_vcnt);
		biza_bio_endio(bio, -EIO);
		return;
	}

	op = bio_op(bio);
	if (op == REQ_OP_WRITE) {
		mutex_lock(&bt->gc_schedule_lock);
		biza_schedule_gc(bt);
		mutex_unlock(&bt->gc_schedule_lock);
	}

	switch (op) {
	case REQ_OP_READ:
		ret = biza_handle_read(bt, bio);
		break;
	case REQ_OP_WRITE:
		ret = biza_handle_write(bt, bio);
		break;
	default:
		pr_err("dm-biza: map error: Unsupported bio type 0x%x",
		       bio_op(bio));
		ret = -EIO;
	}

	biza_bio_endio(bio, errno_to_blk_status(ret));
}

/*
 * Increment a chunk reference counter.
 */
static inline void biza_get_io_work(struct biza_io_work *iowork)
{
	refcount_inc(&iowork->ref);
}

/*
 * Decrement a io work reference count and
 * free it if it becomes 0.
 */
static void biza_put_io_work(struct biza_io_work *iowork)
{
	if (refcount_dec_and_test(&iowork->ref)) {
		BUG_ON(!bio_list_empty(&iowork->bio_list));
		radix_tree_delete(&iowork->bt->io_rxtree, iowork->lcn);
		kfree(iowork);
	}
}

// IO work
static void biza_io_work(struct work_struct *work)
{
	struct biza_io_work *iowork =
		container_of(work, struct biza_io_work, work);
	struct biza_target *bt = iowork->bt;
	struct bio *bio;
	unsigned long flags;

	mutex_lock(&bt->io_lock);

	/* Process the BIOs target at the lcn */
	while ((bio = bio_list_pop(&iowork->bio_list))) {
		mutex_unlock(&bt->io_lock);
		bio_get(bio);
		biza_handle_bio(bt, bio);
		bio_put(bio);
		mutex_lock(&bt->io_lock);
		biza_put_io_work(iowork);
	}

	/* Queueing the work incremented the work refcount */
	biza_put_io_work(iowork);

	mutex_unlock(&bt->io_lock);
}

/*
 * Get a I/O work and start it to process a new BIO.
 * If the BIO chunk has no work yet, create one.
 */
static int biza_queue_io_work(struct biza_target *bt, struct bio *bio)
{
	sector_t lcn = bio->bi_iter.bi_sector >>
		       bt->params->chunk_size_sector_shift;
	struct biza_io_work *iowork;
	int ret = 0;
	unsigned long flags;

	mutex_lock(&bt->io_lock);

	/* Get the BIO chunk work. If one is not active yet, create one */
	iowork = radix_tree_lookup(&bt->io_rxtree, lcn);
	if (iowork) {
		biza_get_io_work(iowork);
	} else {
		iowork = kzalloc(sizeof(struct biza_io_work), GFP_NOIO);
		if (unlikely(!iowork)) {
			ret = -ENOMEM;
			goto out;
		}

		INIT_WORK(&iowork->work, biza_io_work);
		refcount_set(&iowork->ref, 1);
		iowork->bt = bt;
		iowork->lcn = lcn;
		bio_list_init(&iowork->bio_list);

		ret = radix_tree_insert(&bt->io_rxtree, lcn, iowork);
		if (unlikely(ret)) {
			kfree(iowork);
			goto out;
		}
	}

	bio_list_add(&iowork->bio_list, bio);

	/* Upadate access time*/
	biza_gc_update_accese_time(bt);

	if (queue_work(bt->iowq, &iowork->work))
		biza_get_io_work(iowork);

out:
	mutex_unlock(&bt->io_lock);
	return ret;
}

static int biza_map(struct dm_target *ti, struct bio *bio)
{
	struct biza_target *bt = ti->private;
	sector_t nr_sectors = bio_sectors(bio);
	int ret;

	if (!nr_sectors && bio_op(bio) != REQ_OP_WRITE)
		return DM_MAPIO_REMAPPED;

	biza_init_bioctx(bt, bio);

	/* Set the BIO pending in the flush list */
	if (!nr_sectors && bio_op(bio) == REQ_OP_WRITE) {
		/** TODO: support flush **/
		pr_err("dm-biza: io error, nr_sectors = 0 & op = write");
		return DM_MAPIO_KILL;
	}

	/* Now ready to handle this BIO */
	ret = biza_queue_io_work(bt, bio);
	if (ret) {
		pr_debug("BIO op %d, can't process offset %llu, err %i",
			 bio_op(bio), bio->bi_iter.bi_sector, ret);
		return DM_MAPIO_REQUEUE;
	}

	return DM_MAPIO_SUBMITTED;
}

/*
 * Setup target request queue limits.
 */
static void biza_io_hints(struct dm_target *ti, struct queue_limits *limits)
{
	struct biza_target *bt = ti->private;

	limits->logical_block_size = bt->params->chunk_size_byte;
	limits->physical_block_size = bt->params->chunk_size_byte;

	blk_limits_io_min(limits, bt->params->chunk_size_byte);
	blk_limits_io_opt(limits, bt->params->chunk_size_byte * bt->params->k);

	/* FS hint to try to align to the device zone size */
	limits->chunk_sectors = bt->params->chunk_size_sector;

	/* We are exposing a host-managed zoned block device */
	limits->zoned = BLK_ZONED_NONE;
}

// Module
static struct target_type biza_target = { .name = "raum",
					  .version = { 1, 0, 0 },
					  .module = THIS_MODULE,
					  .ctr = biza_ctr,
					  .dtr = biza_dtr,
					  .map = biza_map,
					  .io_hints = biza_io_hints };

static int __init init_biza(void)
{
	return dm_register_target(&biza_target);
}

static void __exit cleanup_biza(void)
{
	dm_unregister_target(&biza_target);
}

module_init(init_biza);
module_exit(cleanup_biza);

MODULE_DESCRIPTION("RAUM: Random-write Allowed Unified Memory");
MODULE_AUTHOR("Xiangqun Zhang <xzhang84@syr.edu>");
MODULE_LICENSE("GPL");
