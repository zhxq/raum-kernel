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

void biza_put_big_chunk_back(struct biza_target *bt,
			     biza_raum_big_chunk_t *big_chunk)
{
	unsigned long flags;
	if (!list_empty(&big_chunk->list)) {
		pr_err("Big chunk list link is not empty!\n");
		BUG_ON(1);
	}
	struct biza_raum_dev *raum_dev = &bt->raum_devs[big_chunk->drive_idx];
	if (big_chunk->chunk_count == RAUM_BIG_CHUNK_PAGES) {
		spin_lock_irqsave(&raum_dev->lru_list_lock, flags);
		log("Adding pcn 0x%llx back to drive %u full_raum_chunks",
		    big_chunk->start_pcn, big_chunk->drive_idx);
		list_add_tail(&big_chunk->list, &raum_dev->full_raum_chunks);
		spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
	} else if (big_chunk->chunk_count == 0) {
		spin_lock_irqsave(&raum_dev->lru_list_lock, flags);
		log("Adding pcn 0x%llx back to drive %u free_raum_chunks",
		    big_chunk->start_pcn, big_chunk->drive_idx);
		list_add_tail(&big_chunk->list, &raum_dev->free_raum_chunks);
		spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
	} else {
		spin_lock_irqsave(&raum_dev->lru_list_lock, flags);
		log("Adding pcn 0x%llx back to drive %u in_use_raum_chunks",
		    big_chunk->start_pcn, big_chunk->drive_idx);
		list_add_tail(&big_chunk->list, &raum_dev->in_use_raum_chunks);
		spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
	}
}

sector_t biza_flush_raum_area(struct biza_target *bt, uint8_t drive_idx,
			      uint32_t zone_idx, sector_t wp, sector_t nlb,
			      sector_t raum_sector)
{
	struct biza_raum_dev *dev = &bt->raum_devs[drive_idx];
	struct request_queue *q = dev->bdev->bd_disk->queue;
	struct request *req;
	struct biza_nvme_request *nrq;
	struct nvme_command *cmd;
	uint64_t result = 0;
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
	cmd->common.cdw14 = cpu_to_le32(raum_sector & 0xffffffff);
	cmd->common.cdw15 = cpu_to_le32(raum_sector >> 32);

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

void biza_flush_big_chunk(struct biza_target *bt,
			  biza_raum_big_chunk_t *big_chunk)
{
	sector_t wp, old_pcn, old_lcn, append_pcn_base, pcn, original_pcn,
		raid_offset = 0;
	uint8_t drive_idx = big_chunk->drive_idx;
	uint32_t zone_idx, i;
	uint64_t offset;
	uint64_t stripe_no;
	bool parity;
	uint8_t *data_buffer;
	struct biza_raum_dev *raum_dev = &bt->raum_devs[big_chunk->drive_idx];
	struct biza_stripe *stripe = NULL;
	struct biza_chunkioctx *chunkioctx;
	biza_stripe_head_t *sh;
	sector_t size = RAUM_BIG_CHUNK_PAGES * bt->params->chunk_size_sector;
	BUG_ON(!big_chunk);
	BUG_ON(big_chunk->chunk_count != RAUM_BIG_CHUNK_PAGES);
	biza_get_write_location(bt, big_chunk->start_pcn, drive_idx, &zone_idx,
				&offset, size);
	wp = biza_idx_to_sector(bt, drive_idx, zone_idx, raid_offset, true);
	log("Flushing loc: big_chunk start pcn 0x%llx, drive_idx: %u, zone_idx %u, target wp: 0x%llx, size 0x%llx\n",
	    big_chunk->start_pcn, drive_idx, zone_idx, wp, size);
	wp = biza_flush_raum_area(bt, drive_idx, zone_idx, wp, size,
				  big_chunk->start_sector);
	append_pcn_base = biza_sector_to_pcn(bt, drive_idx, wp);
	log("Flushed loc: big_chunk start pcn 0x%llx, drive_idx: %u, zone_idx %u, target wp: 0x%llx, size 0x%llx, base 0x%llx\n",
	    big_chunk->start_pcn, drive_idx, zone_idx, wp, size,
	    append_pcn_base);
	for (i = 0; i < RAUM_BIG_CHUNK_PAGES; i++) {
		pcn = append_pcn_base + i;
		if (!biza_is_valid_pcn(bt, pcn)) {
			pr_err("pcn 0x%llx base 0x%llx i %u drive %u zone %u wp 0x%llx\n",
			       pcn, append_pcn_base, i, drive_idx, zone_idx,
			       wp);
			BUG_ON(1);
		}
		chunkioctx = &big_chunk->ctx[i];
		// log("i %d, chunkioctx 0x%px, sh 0x%px, lcn 0x%llx, pcn 0x%llx\n",
		//     i, chunkioctx, chunkioctx->sh, chunkioctx->lcn,
		//     chunkioctx->pcn);

		parity = chunkioctx->lcn == BIZA_MAP_PARITY;
		old_pcn = chunkioctx->pcn;
		if (parity) {
			if (WRITE_AMP_STAT) {
				atomic64_inc(&bt->parity_flush);
			}
			stripe_no = bt->map->p2l[old_pcn].stripe_no;
			stripe = xa_load(&bt->map->stripe_table, stripe_no);
			bt->map->p2l[pcn].chunk_no = BIZA_MAP_PARITY;
			bt->map->p2l[pcn].stripe_no = stripe_no;
			bt->map->p2l[pcn].slot = chunkioctx->slot;
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
			stripe->parity_pcns[chunkioctx->slot] = pcn;
			sh = xa_load(&bt->fshc, stripe_no);
			// pr_err("pcn 0x%llx sh 0x%px\n", pcn, sh);
			if (sh) {
				// pr_err("!!Free parity buffer, pcn=0x%llx, sh->parity_cache=0x%px\n", pcn, sh->parity_cache);
				xa_erase_irq(&bt->fshc, stripe_no);
				biza_free_stripe_head(bt, sh);
			}
		} else {
			if (WRITE_AMP_STAT) {
				atomic64_inc(&bt->data_flush);
			}
			// Update mapping table for data chunks
			old_lcn = chunkioctx->lcn;
			bt->map->l2p[old_lcn].chunk_no = pcn;
			bt->map->l2p[old_lcn].stripe_no =
				bt->map->p2l[old_pcn].stripe_no;
			bt->map->l2p[old_lcn].slot = chunkioctx->slot;
			bt->map->l2p[old_lcn].in_raum = false;
			bt->map->p2l[pcn].chunk_no = old_lcn;
			bt->map->p2l[pcn].stripe_no =
				bt->map->p2l[old_pcn].stripe_no;
			bt->map->p2l[pcn].slot = chunkioctx->slot;
			bt->map->p2l[pcn].in_raum = false;
			bt->map->p2l[old_pcn].chunk_no = BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].stripe_no = BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].slot = (uint8_t)BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].in_raum = false;
			data_buffer = xa_load(&bt->dc, old_pcn);
			BUG_ON(!data_buffer);
			xa_erase_irq(&bt->dc, old_pcn);
			// pr_err("!!Free data buffer, drive_idx %u, zone_idx %u, offset 0x%llx, pointer: 0x%px\n", drive_idx, zone_idx, wp_off, data_buffer);
			biza_mempool_free(&bt->dcpool, data_buffer);
		}
	}

	big_chunk->chunk_count = 0;
	big_chunk->this_write_start_chunk = 0;
	big_chunk->this_write_chunk_count = 0;
	big_chunk->this_write_start_sector = big_chunk->start_sector;
	bio_put(big_chunk->chunkio);

	big_chunk->chunkio =
		bio_alloc_bioset(GFP_NOIO, RAUM_BIG_CHUNK_PAGES, &bt->bio_set);
	big_chunk->chunkio->bi_private = big_chunk;
	big_chunk->chunkio->bi_end_io = biza_bigchunkio_endio;
	big_chunk->chunkio->bi_iter.bi_sector =
		big_chunk->this_write_start_sector;
	bio_set_dev(big_chunk->chunkio, raum_dev->bdev);
	bio_set_op_attrs(big_chunk->chunkio, REQ_OP_WRITE, 0);

	atomic64_set(&big_chunk->flush_in_flight, 0);
}

biza_raum_big_chunk_t *biza_get_big_chunk(struct biza_target *bt,
					  uint8_t drive_idx)
{
	int i;
	unsigned long flags;
	struct biza_raum_dev *raum_dev = &bt->raum_devs[drive_idx];
	biza_raum_big_chunk_t *big_chunk = NULL;

	// Try until success
	while (true) {
		spin_lock_irqsave(&raum_dev->lru_list_lock, flags);

		// Get a new big chunk from currently in-use ones
		big_chunk =
			list_first_entry_or_null(&raum_dev->in_use_raum_chunks,
						 biza_raum_big_chunk_t, list);

		if (big_chunk) {
			list_del_init(&big_chunk->list);
			spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
			log("In-use bigchunk drive %u start_pcn 0x%llx\n",
			    drive_idx, big_chunk->start_pcn);
			return big_chunk;
		}

		// If not available, get a new big chunk
		big_chunk =
			list_first_entry_or_null(&raum_dev->free_raum_chunks,
						 biza_raum_big_chunk_t, list);

		if (big_chunk) {
			list_del_init(&big_chunk->list);
			spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
			log("New bigchunk drive %u start_pcn 0x%llx\n",
			    drive_idx, big_chunk->start_pcn);
			return big_chunk;
		}

		// If no big chunk available, flush a full chunk to flash
		big_chunk =
			list_first_entry_or_null(&raum_dev->full_raum_chunks,
						 biza_raum_big_chunk_t, list);

		if (big_chunk) {
			list_del_init(&big_chunk->list);
			atomic64_inc(&big_chunk->flush_in_flight);
			spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
			log("Full bigchunk drive %u start_pcn 0x%llx\n",
			    drive_idx, big_chunk->start_pcn);
			while (atomic64_read(&big_chunk->updates_in_flight)) {
				pr_err("Drive %u big_chunk update in flight, val %llu\n",
				       drive_idx,
				       atomic64_read(
					       &big_chunk->updates_in_flight));
				cpu_relax();
				cond_resched();
			}

			biza_flush_big_chunk(bt, big_chunk);
			return big_chunk;
		}

		// Give others a chance to put something back to the big chunk list
		spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
		cpu_relax();
		pr_err("Drive %u no bigchunk available\n", drive_idx);
		cond_resched();
		continue;
	}
}

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

			blk_mq_free_request(req);

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
	biza_raum_big_chunk_t *big_chunk;
	int i = 0, j = 0, k = 0;

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

		if (bt->params->nr_raum_big_chunks_per_drive == 0) {
			bt->params->nr_raum_big_chunks_per_drive =
				dev->capacity_in_chunks / RAUM_BIG_CHUNK_PAGES;
		}

		BUG_ON(bt->params->nr_raum_big_chunks_per_drive !=
		       dev->capacity_in_chunks /
			       RAUM_BIG_CHUNK_PAGES); // Every drive should have the same number

		dev->chunk_content =
			kzalloc(sizeof(uint8_t *) * dev->capacity_in_chunks,
				GFP_KERNEL);

		if (!dev->chunk_content) {
			pr_err("dm-biza: open zone error: cannot alloc chunk content list for RAUM device %d\n",
			       i);
			return -1;
		}

		INIT_LIST_HEAD(&dev->free_raum_chunks);
		INIT_LIST_HEAD(&dev->in_use_raum_chunks);
		INIT_LIST_HEAD(&dev->full_raum_chunks);
		INIT_LIST_HEAD(&dev->lru_list);
		spin_lock_init(&dev->lru_list_lock);
		xa_init(&dev->big_chunk_list);

		spin_lock_irq(&dev->lru_list_lock);
		// for (j = 0; j < dev->capacity_in_chunks; j++) {
		// 	chunk = kzalloc(sizeof(biza_free_raum_chunk_t),
		// 			GFP_KERNEL);
		// 	chunk->chunk = j;
		// 	list_add_tail(&chunk->link, &dev->free_raum_chunks);
		// }

		for (j = 0; j < bt->params->nr_raum_big_chunks_per_drive; j++) {
			big_chunk = kzalloc(sizeof(biza_raum_big_chunk_t),
					    GFP_KERNEL);
			big_chunk->drive_idx = i;
			big_chunk->chunk_count = 0;
			big_chunk->this_write_chunk_count = 0;
			big_chunk->start_sector = j * RAUM_BIG_CHUNK_PAGES *
						  bt->params->chunk_size_sector;
			big_chunk->this_write_start_sector =
				big_chunk->start_sector;
			big_chunk->start_pcn = biza_raum_idx_to_pcn(
				bt, i, j * RAUM_BIG_CHUNK_PAGES);
			big_chunk->ctx =
				kzalloc(sizeof(struct biza_chunkioctx) *
						RAUM_BIG_CHUNK_PAGES,
					GFP_KERNEL);
			big_chunk->chunkio = bio_alloc_bioset(
				GFP_NOIO, RAUM_BIG_CHUNK_PAGES, &bt->bio_set);
			big_chunk->chunkio->bi_private = big_chunk;
			big_chunk->chunkio->bi_end_io = biza_bigchunkio_endio;
			big_chunk->chunkio->bi_iter.bi_sector =
				big_chunk->this_write_start_sector;
			bio_set_dev(big_chunk->chunkio, bt->raum_devs[i].bdev);
			bio_set_op_attrs(big_chunk->chunkio, REQ_OP_WRITE, 0);
			big_chunk->bt = bt;
			atomic64_set(&big_chunk->updates_in_flight, 0);
			atomic64_set(&big_chunk->flush_in_flight, 0);
			xa_store_irq(&dev->big_chunk_list, big_chunk->start_pcn,
				     big_chunk, GFP_KERNEL);

			list_add_tail(&big_chunk->list, &dev->free_raum_chunks);
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
		element = (uint8_t *)__get_free_pages(GFP_ATOMIC, pool->order);
		// pr_err("mempool run out\n");
	}

	spin_unlock_irqrestore(&pool->lock, flags);

	return element;
}

void biza_mempool_free(struct biza_mempool *pool, uint8_t *element)
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

	bt->params->max_chunk_size_byte =
		bt->params->chunk_size_byte * RAUM_LARGER_CHUNK_PAGES;
	bt->params->max_chunk_size_sector = bt->params->max_chunk_size_byte >>
					    SECTOR_SHIFT;
	bt->params->max_chunk_size_sector_shift =
		ilog2(bt->params->max_chunk_size_sector);

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

	// Initialize bio set
	ret = bioset_init(&bt->bio_set, BIZA_BIO_POOL_SIZE, 0,
			  BIOSET_NEED_BVECS);
	if (ret) {
		ti->error = "Failed to create bio set";
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

	/** WARN: All SSD should be the same **/
	bt->params->nr_zones_per_drive = bt->devs[0].nr_zones;
	bt->params->zone_capacity_chunk = bt->devs[0].zones[0].capacity >>
					  bt->params->chunk_size_sector_shift;
	bt->params->nr_chunks = bt->params->k * bt->params->nr_zones_per_drive *
				bt->params->zone_capacity_chunk;
	bt->params->nr_internal_chunks = bt->params->nr_drives *
					 bt->params->nr_zones_per_drive *
					 bt->params->zone_capacity_chunk;

	pr_err("Init RAUM devs\n");
	bt->params->nr_total_raum_chunks = 0;
	bt->params->nr_raum_chunks_per_drive = 0;
	// Initialize RAUM drives
	ret = biza_init_raum_devs(ti);
	if (ret) {
		ti->error = "Cannot init RAUM drives";
		goto err_raum_dev;
	}

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

	bt->end_iowq = alloc_workqueue("biza_end_iowq",
				       WQ_MEM_RECLAIM | WQ_UNBOUND,
				       NUM_SUBMIT_WORKER);
	if (!bt->end_iowq) {
		ti->error = "Failed to create end io workqueue";
		ret = -ENOMEM;
		goto err_map;
	}

	bt->end_bigchunk_iowq = alloc_workqueue("biza_end_bigchunk_iowq",
						WQ_MEM_RECLAIM | WQ_UNBOUND,
						NUM_SUBMIT_WORKER);
	if (!bt->end_bigchunk_iowq) {
		ti->error = "Failed to create end bigchunk io workqueue";
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
	// xa_init(&bt->raum_data);
	// hash_init(bt->raum_data);

	// Initialize parity lcn tracker in RAUM;
	// xa_init(&bt->raum_parity);
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

	ret = biza_ctr_mempool(&bt->largepcpool64, BIZA_PARITY_CACHE_SIZE,
			       bt->params->max_chunk_size_sector_shift - 0 -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create large parity cache";
		ret = -ENOMEM;
		goto err_dc;
	}

	ret = biza_ctr_mempool(&bt->largepcpool32, BIZA_PARITY_CACHE_SIZE,
			       bt->params->max_chunk_size_sector_shift - 1 -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create large parity cache";
		ret = -ENOMEM;
		goto err_dc;
	}

	ret = biza_ctr_mempool(&bt->largepcpool16, BIZA_PARITY_CACHE_SIZE,
			       bt->params->max_chunk_size_sector_shift - 2 -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create large parity cache";
		ret = -ENOMEM;
		goto err_dc;
	}

	ret = biza_ctr_mempool(&bt->largepcpool8, BIZA_PARITY_CACHE_SIZE,
			       bt->params->max_chunk_size_sector_shift - 3 -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create large parity cache";
		ret = -ENOMEM;
		goto err_dc;
	}

	ret = biza_ctr_mempool(&bt->largepcpool4, BIZA_PARITY_CACHE_SIZE,
			       bt->params->max_chunk_size_sector_shift - 4 -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create large parity cache";
		ret = -ENOMEM;
		goto err_dc;
	}

	ret = biza_ctr_mempool(&bt->largepcpool2, BIZA_PARITY_CACHE_SIZE,
			       bt->params->max_chunk_size_sector_shift - 5 -
				       PAGE_SECTORS_SHIFT);
	if (ret) {
		ti->error = "Failed to create large parity cache";
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

	ret = biza_init_devs_open_zones(ti);
	if (ret) {
		goto err_lru;
	}

	// statistics for write amplification
	atomic64_set(&bt->user_send, 0);
	atomic64_set(&bt->user_read, 0);
	atomic64_set(&bt->data_write, 0);
	atomic64_set(&bt->parity_write, 0);
	atomic64_set(&bt->oop_parity_write, 0);
	atomic64_set(&bt->data_in_place_update, 0);
	atomic64_set(&bt->parity_in_place_update, 0);

	atomic64_set(&bt->previous_print_time, ktime_get_boottime_ns());

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

struct biza_mempool *biza_find_pcpool(struct biza_target *bt,
				      uint64_t chunks_per_shard)
{
	switch (chunks_per_shard) {
	case 64:
		return &bt->largepcpool64;
	case 32:
		return &bt->largepcpool32;
	case 16:
		return &bt->largepcpool16;
	case 8:
		return &bt->largepcpool8;
	case 4:
		return &bt->largepcpool4;
	case 2:
		return &bt->largepcpool2;
	case 1:
		return &bt->pcpool;

	default:
		pr_err("Error chunks per shard: %llu\n", chunks_per_shard);
		BUG_ON(1);
		break;
	}
}

// Free stripe head
void biza_free_stripe_head(struct biza_target *bt, biza_stripe_head_t *sh)
{
	// kvfree(sh->parity_cache);
	biza_mempool_free(biza_find_pcpool(bt, sh->chunks_in_shard),
			  sh->parity_cache);
	kfree(sh);
}

// Allocate an empty stripe head & init
static biza_stripe_head_t *
biza_alloc_empty_stripe_head(struct biza_target *bt, uint64_t chunks_per_shard)
{
	biza_stripe_head_t *sh =
		kzalloc(sizeof(biza_stripe_head_t), GFP_KERNEL);

	if (sh) {
		sh->no = atomic64_inc_return(
			&bt->strip_no_cnt); // Started with 0
		sh->nr_data_written = 0;

		// sh->parity_cache = kvzalloc(bt->params->m * bt->params->chunk_size_byte, GFP_KERNEL);

		sh->parity_cache = biza_mempool_alloc(
			biza_find_pcpool(bt, chunks_per_shard));

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
	unsigned long flags;

	spin_lock_irqsave(&bt->pshl_lock, flags);
	// sh = list_first_or_null_rcu(&bt->pshl, biza_stripe_head_t, link);
	sh = list_first_entry_or_null(&bt->pshl, biza_stripe_head_t, link);
	if (sh) {
		list_del(&sh->link);
		spin_unlock_irqrestore(&bt->pshl_lock, flags);
	} else {
		spin_unlock_irqrestore(&bt->pshl_lock, flags);
		sh = biza_alloc_empty_stripe_head(bt, 1);
	}

	return sh;
}

// Get a stripe head from cache using stripe no
biza_stripe_head_t *biza_get_stripe_head_with_no(struct biza_target *bt,
						 uint64_t no)
{
	biza_stripe_head_t *sh = NULL, *cur = NULL, *tmp = NULL;
	unsigned long flags;

	// Try to get from pshl
	spin_lock_irqsave(&bt->pshl_lock, flags);
	list_for_each_entry_safe(cur, tmp, &bt->pshl, link) {
		if (cur->no == no) {
			log("load shno 0x%llx from pshl\n", no);
			sh = cur;
			list_del(&cur->link);
		}
	}
	spin_unlock_irqrestore(&bt->pshl_lock, flags);

	// Try to get from fshc
	if (!sh) {
		sh = xa_load(&bt->fshc, no);
		if (sh) {
			log("load shno 0x%llx from fshc\n", no);
			xa_erase_irq(&bt->fshc, no);
		}
	}

	return sh;
}

// Compute parities
static int biza_compute_parity(struct biza_target *bt, struct bio *bio,
			       biza_stripe_head_t *sh, uint8_t chunk_cnt,
			       uint64_t chunks_per_shard)
{
	void **chunks = kzalloc(
		(chunk_cnt + bt->params->m) * sizeof(uint64_t *), GFP_KERNEL);
	uint8_t *bvec_start, *data_start;
	struct bvec_iter iter;
	struct bio_vec bvec;
	uint64_t chunk_size = chunks_per_shard * bt->params->chunk_size_byte;
	int i = 0, this_cnt = 0, src_off = 0;

	if (bt->params->m != 1) {
		pr_err("dm-biza: io error: only support RAID 5 now");
		return -EDOM;
	}

	// bvec_start = bvec_kmap_local(&bio->bi_io_vec[0]);
	// data_start = bvec_start + bio->bi_iter.bi_bvec_done;

	iter = bio->bi_iter;
	bvec = bio_iter_iovec(bio, iter);
	data_start = bvec_start = bvec_kmap_local(&bvec);

	for (i = 0; i < chunk_cnt; ++i) {
		chunks[i] = data_start + i * chunk_size;
	}
	for (i = 0; i < bt->params->m; ++i) {
		chunks[chunk_cnt + i] = sh->parity_cache + i * chunk_size;
	}

	for (i = 0; i < bt->params->m; ++i) {
		src_off = 0;
		while (chunk_cnt > 0) {
			this_cnt = min(chunk_cnt, (uint8_t)MAX_XOR_BLOCKS);

			xor_blocks(this_cnt, chunk_size, chunks[chunk_cnt + i],
				   chunks + src_off);

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

// Allocate write location, size in sectors
static inline bool biza_allocate_wp(struct biza_target *bt, uint8_t drive_idx,
				    uint8_t oz_idx, uint32_t zone_idx,
				    sector_t size, unsigned long *flags)
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
	zone->wp += size;
	// pr_err("drive_idx %u, zone_idx %u, zone_wp add, now: %llu\n", drive_idx, zone_idx, zone->wp);
	// atomic64_add(bt->params->chunk_size_sector, &zone->wp);

	if (zone->wp >= zone->start + zone->capacity) { // 这个zone使用完了
		zone->cond = BLK_ZONE_COND_FULL;
		// pr_err("drive_idx %u, zone_idx %u full\n", drive_idx, dev->open_zones[oz_idx]);
		spin_unlock_irqrestore(&zone->zlock, *flags);

		up_read(&dev->ozlock);
		down_write(&dev->ozlock);
		while (atomic64_read(&zone->in_flight_ios) !=
		       atomic64_read(&zone->finished_ios)) {
			pr_err("Drive %u zone %u Waiting for in flight io (%llu/%llu finished)\n",
			       drive_idx, zone_idx,
			       atomic64_read(&zone->finished_ios),
			       atomic64_read(&zone->in_flight_ios));
			cpu_relax();
			cond_resched();
		}
		ret = biza_finish_zone(bt, dev, zone_idx);
		pr_err("Drive %u finish zone %u\n", drive_idx, zone_idx);
		dev->open_zones[oz_idx] = biza_open_empty_zone(
			bt, dev, true,
			biza_oz_idx_to_aware_type(bt, drive_idx, oz_idx));
		pr_err("drive_idx %u, oz_idx %u open new zone %u/%u\n",
		       drive_idx, oz_idx, dev->open_zones[oz_idx],
		       dev->nr_zones);

		if (dev->open_zones[oz_idx] == dev->nr_zones)
			BUG_ON(1);
		downgrade_write(&dev->ozlock);

		zone_idx = dev->open_zones[oz_idx];
		zone = &dev->zones[zone_idx];
		zone->wp += size;
		spin_lock_irqsave(&zone->zlock, *flags);
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

// Which zone to write? size in sectors.
// For parallel write
static bool biza_get_zone_write_location(struct biza_target *bt,
					 uint8_t drive_idx, uint8_t oz_idx,
					 uint32_t *zone_idx, uint64_t *offset,
					 sector_t size)
{
	struct biza_dev *dev = &bt->devs[drive_idx];
	struct biza_zone *zone;
	uint64_t wp_off;
	unsigned long flags;
	while (1) {
		down_read(&dev->ozlock);
		*zone_idx = dev->open_zones[oz_idx];
		zone = &dev->zones[*zone_idx];
		while (zone->cond == BLK_ZONE_COND_FULL) {
			up_read(&dev->ozlock);
			udelay(1);
			pr_err("Drive %u Zone %u is full\n", drive_idx,
			       *zone_idx);
			cpu_relax();
			cond_resched();
			continue;
		}

		spin_lock_irqsave(&zone->zlock, flags);

		wp_off = (zone->wp - zone->start) >>
			 bt->params->chunk_size_sector_shift;

		biza_allocate_wp(bt, drive_idx, oz_idx, *zone_idx, size,
				 &flags);
		*zone_idx = dev->open_zones[oz_idx];
		zone = &dev->zones[*zone_idx];
		atomic64_inc(&zone->in_flight_ios);

		// wp_off = (atomic64_read(&zone->wp) - zone->start) >> bt->params->chunk_size_sector_shift;
		spin_unlock_irqrestore(&zone->zlock, flags);
		up_read(&dev->ozlock);
		*offset = wp_off;

		return true;
	}
	return false;
}

// Get a write location in a zone, size in sectors
inline void biza_get_write_location(struct biza_target *bt, uint64_t hint,
				    uint8_t drive_idx, uint32_t *zone_idx,
				    uint64_t *offset, sector_t size)
{
	uint8_t oz_idx;
	int ret;

	oz_idx = biza_choose_open_zone_to_write(bt, drive_idx, hint);
	ret = biza_get_zone_write_location(bt, drive_idx, oz_idx, zone_idx,
					   offset, size);
	BUG_ON(!ret);
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

biza_raum_big_chunk_t *biza_find_big_chunk_by_pcn(struct biza_target *bt,
						  uint64_t pcn)
{
	uint8_t drive_idx;
	sector_t offset;
	struct biza_raum_dev *raum_dev;
	biza_raum_pcn_to_idx(bt, pcn, &drive_idx, &offset);
	offset = biza_round_chunk_no(offset);
	pcn = biza_raum_idx_to_pcn(bt, drive_idx, offset);
	raum_dev = &bt->raum_devs[drive_idx];
	return xa_load(&raum_dev->big_chunk_list, pcn);
}

// Prepare for in-place updates
// Set up in-flight status
static bool biza_prep_in_place_pcn_update(struct biza_target *bt, uint64_t pcn,
					  bool parity)
{
	uint8_t drive_idx;
	sector_t offset;
	biza_raum_big_chunk_t *big_chunk;
	unsigned long flags;
	struct biza_raum_dev *raum_dev;

	if (biza_check_pcn_in_raum(bt, pcn)) {
		biza_raum_pcn_to_idx(bt, pcn, &drive_idx, &offset);
		raum_dev = &bt->raum_devs[drive_idx];
		big_chunk = biza_find_big_chunk_by_pcn(bt, pcn);
		spin_lock_irqsave(&raum_dev->lru_list_lock, flags);
		if (atomic64_read(&big_chunk->flush_in_flight)) {
			spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
			return false;
		}
		atomic64_inc(&big_chunk->updates_in_flight);
		log("big_chunk 0x%px pcn 0x%llx in flight++, now 0x%llx, parity %s\n",
		    big_chunk, pcn,
		    atomic64_read(&big_chunk->updates_in_flight),
		    parity ? "true" : "false");
		spin_unlock_irqrestore(&raum_dev->lru_list_lock, flags);
		return true;
	}
	return false;
}

static bool biza_is_data_in_raum(struct biza_target *bt, uint64_t lcn)
{
	sector_t pcn = bt->map->l2p[lcn].chunk_no;
	if (biza_check_pcn_in_raum(bt, pcn)) {
		return true;
	}
	return false;
}

static bool biza_is_parity_in_raum(struct biza_target *bt, uint64_t no,
				   uint8_t slot)
{
	struct biza_stripe *stripe = xa_load(&bt->map->stripe_table, no);
	sector_t pcn;
	if (!stripe)
		return false;
	pcn = stripe->parity_pcns[slot];
	if (biza_check_pcn_in_raum(bt, pcn)) {
		return true;
	}
	return false;
}

// can the data be updated in place?
static bool biza_can_raum_data_update_in_place(struct biza_target *bt,
					       uint64_t lcn)
{
	sector_t pcn = bt->map->l2p[lcn].chunk_no;
	return biza_prep_in_place_pcn_update(bt, pcn, false);
}

// can the parity be updated in place?
static bool biza_can_raum_parity_update_in_place(struct biza_target *bt,
						 uint64_t no, uint8_t slot)
{
	struct biza_stripe *stripe = xa_load(&bt->map->stripe_table, no);
	sector_t pcn;
	if (!stripe)
		return false;
	pcn = stripe->parity_pcns[slot];
	return biza_prep_in_place_pcn_update(bt, pcn, true);
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
	log("lcn 0x%llx shno 0x%llx\n", lcn, stripe_no);
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

	if (status != BLK_STS_OK && bio->bi_status == BLK_STS_OK) {
		log("bio endio status = %u, bio 0x%px\n", status, bio);
		bio->bi_status = status;
	}

	if (refcount_dec_and_test(&bioctx->ref)) {
		bio_endio(bio);
	}
}

void stripe_head_endio_work(struct work_struct *work)
{
	unsigned long flags;
	biza_stripe_head_t *sh = container_of(work, biza_stripe_head_t, work);
	struct biza_target *bt = sh->bt;
	spin_lock_irqsave(&bt->pshl_lock, flags);
	sh->ioctx = NULL;
	list_add_tail(&sh->link, &bt->pshl);
	spin_unlock_irqrestore(&bt->pshl_lock, flags);
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
		log("store shno 0x%llx to fshc\n", sh->no);
		xa_store(&bt->fshc, sh->no, sh, GFP_ATOMIC);
	} else {
		// May deadlock without _IRQ?
		log("store shno 0x%llx to pshl\n", sh->no);
		sh->bt = bt;
		INIT_WORK(&sh->work, stripe_head_endio_work);
		queue_work(bt->end_iowq, &sh->work);
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
	struct biza_target *bt;
	biza_raum_big_chunk_t *big_chunk;
	int ret;
	int i;

	bt = chunkioctx->bt;

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

		log("dm-biza: start small io lcn: 0x%llx, io_type %d, bi_status %d, drive_idx %u, offset 0x%llx, chunks in shard %llu\n",
		    chunkioctx->lcn, bio_op(chunkio), status,
		    chunkioctx->drive_idx, chunkio->bi_iter.bi_sector,
		    sh->chunks_in_shard);

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

		// TODO: add subslot numbers (i) to keep track of
		// normal chunks in larger chunks
		if (chunkioctx->type == BIZA_DATA_WRITE) {
			biza_map_update_data_wrt(
				chunkioctx->bt, chunkioctx->lcn,
				chunkioctx->pcn, sh->no, chunkioctx->slot,
				chunkioctx->in_raum, sh->chunks_in_shard);
		} else if (chunkioctx->type == BIZA_PARITY_WRITE) {
			biza_map_update_parity_wrt(chunkioctx->bt,
						   chunkioctx->pcn, sh->no,
						   chunkioctx->slot,
						   chunkioctx->in_raum,
						   sh->chunks_in_shard);
		}

		// log("dm-biza: mid small io lcn: 0x%llx, io_type %d, bi_status %d, drive_idx %u, offset 0x%llx, sectors %u, shref %d\n",
		//     chunkioctx->lcn, bio_op(chunkio), status,
		//     chunkioctx->drive_idx, chunkio->bi_iter.bi_sector,
		//     bio_sectors(chunkio), refcount_read(&sh->ioctx->ref));

		// ret = biza_test_and_clear_zrwa_bit(chunkioctx->bt,
		// 				   chunkioctx->pcn, false);
		// if (!ret) {
		// 	biza_pcn_to_idx(chunkioctx->bt, chunkioctx->pcn,
		// 			&drive_idx, &zone_idx, &offset);
		// 	BUG_ON(1);
		// }

		if (chunkioctx->in_raum) {
			big_chunk =
				xa_load(&bt->raum_devs[chunkioctx->drive_idx]
						 .big_chunk_list,
					biza_round_chunk_no(chunkioctx->pcn));
			atomic64_dec(&big_chunk->updates_in_flight);
			log("big_chunk 0x%px pcn 0x%llx in flight--, now 0x%llx, parity %s\n",
			    big_chunk, chunkioctx->pcn,
			    atomic64_read(&big_chunk->updates_in_flight),
			    chunkioctx->lcn == BIZA_MAP_PARITY ? "true" :
								 "false");
		} else {
			biza_gc_avoid_stat(chunkioctx);
			if (chunkioctx->lcn == BIZA_MAP_PARITY) {
				biza_mempool_free(
					biza_find_pcpool(bt,
							 sh->chunks_in_shard),
					sh->parity_cache);
			}
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

void biza_bigchunkio_endio_work(struct work_struct *work)
{
	biza_raum_big_chunk_t *big_chunk =
		container_of(work, biza_raum_big_chunk_t, work);
	struct biza_target *bt = big_chunk->bt;
	struct biza_raum_dev *raum_dev = &bt->raum_devs[big_chunk->drive_idx];
	big_chunk->chunkio =
		bio_alloc_bioset(GFP_NOIO, RAUM_BIG_CHUNK_PAGES, &bt->bio_set);
	big_chunk->chunkio->bi_private = big_chunk;
	big_chunk->chunkio->bi_end_io = biza_bigchunkio_endio;
	big_chunk->chunkio->bi_iter.bi_sector =
		big_chunk->this_write_start_sector;
	bio_set_dev(big_chunk->chunkio, raum_dev->bdev);
	bio_set_op_attrs(big_chunk->chunkio, REQ_OP_WRITE, 0);
	biza_put_big_chunk_back(bt, big_chunk);
}

void biza_bigchunkio_endio(struct bio *chunkio)
{
	uint32_t i, j;
	biza_raum_big_chunk_t *big_chunk = chunkio->bi_private;
	struct biza_target *bt = big_chunk->bt;
	struct biza_raum_dev *raum_dev = &bt->raum_devs[big_chunk->drive_idx];
	unsigned long flags;
	struct biza_chunkioctx *chunkioctx;

	struct bio *bio;
	biza_stripe_head_t *sh;
	blk_status_t status = chunkio->bi_status;
	uint8_t drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	sector_t bi_sector;
	sector_t lcn;
	int ret;

	// log("bigchunkio end drive_idx %u sector 0x%llx pcn 0x%llx num chunks %u size 0x%llx\n",
	//     big_chunk->drive_idx, chunkio->bi_iter.bi_sector,
	//     chunkio->bi_iter.bi_sector >> bt->params->chunk_size_sector_shift,
	//     big_chunk->this_write_chunk_count,
	//     big_chunk->this_write_chunk_count * bt->params->chunk_size_byte);

	for (j = big_chunk->this_write_start_chunk;
	     j < big_chunk->this_write_start_chunk +
			 big_chunk->this_write_chunk_count;
	     j++) {
		if (j >= RAUM_BIG_CHUNK_PAGES) {
			pr_err("OOB this_write_start_chunk 0x%x this_write_chunk_count %u total 0x%x j 0x%x\n",
			       big_chunk->this_write_start_chunk,
			       big_chunk->this_write_chunk_count,
			       big_chunk->this_write_start_chunk +
				       big_chunk->this_write_chunk_count,
			       j);
			BUG_ON(1);
		}
		chunkioctx = &big_chunk->ctx[j];
		if (chunkioctx->type == BIZA_DATA_WRITE ||
		    chunkioctx->type == BIZA_PARITY_WRITE ||
		    chunkioctx->type == BIZA_DATA_UPDATE ||
		    chunkioctx->type == BIZA_PARITY_UPDATE) {
			sh = chunkioctx->sh;

			if (unlikely(status != BLK_STS_OK)) {
				// pr_err("dm-biza: io failed! io_type %d, bi_status %d, offset %lld, sectors %u",
				//        bio_op(chunkio), status, chunkio->bi_iter.bi_sector,
				//        bio_sectors(chunkio));
				pr_err("dm-biza: big chunk io failed! bi_status %u lcn: 0x%llx, io_type %d, bi_status %d, offset 0x%llx, sectors %u\n",
				       status, chunkioctx->lcn, bio_op(chunkio),
				       status, chunkio->bi_iter.bi_sector,
				       bio_sectors(chunkio));

				BUG_ON(1);
			}

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
			}

			if (chunkioctx->type == BIZA_DATA_WRITE) {
				biza_map_update_data_wrt(
					chunkioctx->bt, chunkioctx->lcn,
					chunkioctx->pcn, sh->no,
					chunkioctx->slot, chunkioctx->in_raum,
					1);
			} else if (chunkioctx->type == BIZA_PARITY_WRITE) {
				biza_map_update_parity_wrt(
					chunkioctx->bt, chunkioctx->pcn, sh->no,
					chunkioctx->slot, chunkioctx->in_raum,
					1);
			}

			// TODO: set in_raum to false when flushing
			if (!chunkioctx->in_raum) {
				biza_gc_avoid_stat(chunkioctx);
			}

			biza_end_stripe_head_io(sh, status);
		}
	}
	bio_put(chunkio);

	big_chunk->this_write_start_chunk += big_chunk->this_write_chunk_count;
	big_chunk->this_write_start_sector +=
		big_chunk->this_write_chunk_count *
		bt->params->chunk_size_sector;

	big_chunk->this_write_chunk_count = 0;

	INIT_WORK(&big_chunk->work, biza_bigchunkio_endio_work);
	queue_work(bt->end_bigchunk_iowq, &big_chunk->work);
}

// Send stripe I/O to SSDs
static int biza_submit_stripe_head_write(struct biza_target *bt,
					 struct bio *bio,
					 biza_stripe_head_t *sh,
					 uint8_t chunk_cnt,
					 biza_raum_big_chunk_t **big_chunks,
					 uint64_t chunks_in_shard)
{
	// pr_err("enter: type:%u chunks:%u bio=0x%px bt=0x%px bi_size=%u vcnt=%u idx=%u done=%u\n",
	//    type, chunk_cnt, bio, bt, bio->bi_iter.bi_size, bio->bi_vcnt,
	//    bio->bi_iter.bi_idx, bio->bi_iter.bi_bvec_done);
	struct biza_stripe_head_ioctx *shioctx = sh->ioctx;
	struct bio *chunkio;
	struct biza_chunkioctx *chunkioctx;
	uint8_t drive_idx, new_drive_idx;
	uint32_t zone_idx;
	uint64_t offset;
	sector_t lcn, pcn;
	uint8_t *data_buffer, *bvec_start;
	struct biza_raum_dev *raum_dev;
	int i, ret;
	struct bvec_iter iter;
	struct bio_vec bvec;
	unsigned long flags;
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	struct biza_stripe *stripe;

	struct block_device *bdev;
	biza_raum_big_chunk_t *big_chunk, *old_big_chunk;
	sector_t bi_sector;

	uint64_t num_chunks = chunks_in_shard;
	sector_t sectors_in_shard = num_chunks * bt->params->chunk_size_sector;
	sector_t bytes_in_shard = num_chunks * bt->params->chunk_size_byte;

	bool data_in_raum, parity_oop;

	enum biza_aware_type aware_type;

	BUG_ON(shioctx == NULL);

	// send data chunk I/O
	for (i = 0; i < chunk_cnt; ++i) {
		lcn = sh->ioctx->lcn_start + (i * num_chunks);
		// pr_err("data chunk lcn 0x%llx, size %d\n", lcn, num_chunks);

		// Data update in place
		if (shioctx->type == BIZA_SH_IN_PLACE_UPDATE) {
			// pcn = sh->ioctx->data_pcns[0];
			// BUG_ON(pcn == BIZA_MAP_UNMAPPED ||
			//        pcn == BIZA_MAP_INVALID);
			// biza_pcn_to_idx(bt, pcn, &drive_idx, &zone_idx,
			// 		&offset);
			pcn = bt->map->l2p[lcn].chunk_no;
			biza_update_pred(bt, pcn);
			biza_raum_pcn_to_idx(bt, pcn, &drive_idx, &offset);
			raum_dev = &bt->raum_devs[drive_idx];

			bi_sector = biza_raum_idx_to_sector(bt, offset);
			chunkio = bio_clone_fast(bio, GFP_NOIO, &bt->bio_set);
			bio_set_dev(chunkio, raum_dev->bdev);
			chunkio->bi_iter.bi_sector = bi_sector;
			chunkio->bi_iter.bi_size = bt->params->chunk_size_byte;
			chunkio->bi_end_io = biza_chunkio_endio;

			chunkioctx = kzalloc(sizeof(struct biza_chunkioctx),
					     GFP_NOIO);
			if (!chunkioctx) {
				BUG_ON(1);
				return -ENOMEM;
			}

			chunkioctx->sh = sh;
			chunkioctx->type = BIZA_DATA_UPDATE;
			chunkioctx->stime = jiffies;
			chunkioctx->bt = bt;
			chunkioctx->lcn = lcn;
			chunkioctx->pcn = pcn;
			chunkioctx->drive_idx = drive_idx;
			chunkioctx->slot = bt->map->l2p[lcn].slot;
			chunkioctx->in_raum = true;
			chunkioctx->raum_flush_to_zone = false;
			chunkio->bi_private = chunkioctx;
			shioctx->data_pcns[i] = pcn;

			log("Writing RAUM in-place lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx size 0x%x shno 0x%llx slot %d shref %u\n",
			    lcn, pcn, drive_idx, offset, bi_sector,
			    bio_sectors(chunkio), sh->no, chunkioctx->slot,
			    refcount_read(&shioctx->ref));

			refcount_inc(&shioctx->ref);
			submit_bio_noacct(chunkio);

			if (WRITE_AMP_STAT)
				atomic64_add(bt->params->chunk_size_sector,
					     &bt->data_in_place_update);
			bio_advance(bio, bt->params->chunk_size_byte);
		} else {
			drive_idx = (sh->nr_data_written + i + sh->no +
				     bt->params->m) %
				    bt->params->nr_drives;

			if (chunk_cnt == bt->params->k) {
				// Full stripes
				data_in_raum = false;
				bdev = bt->devs[drive_idx].dev->bdev;
				biza_get_write_location(bt, lcn, drive_idx,
							&zone_idx, &offset,
							sectors_in_shard);
				pcn = biza_idx_to_pcn(bt, drive_idx, zone_idx,
						      offset);
				bi_sector = biza_idx_to_sector(
					bt, drive_idx, zone_idx, offset, true);
				log("Writing large_chunk zone lcn 0x%llx pcn 0x%llx drive %u zone %u offset 0x%llx sector 0x%llx shno 0x%llx\n",
				    lcn, pcn, drive_idx, zone_idx, offset,
				    bi_sector, sh->no);

				if (WRITE_AMP_STAT)
					atomic64_add(sectors_in_shard,
						     &bt->data_write);

				chunkio = bio_clone_fast(bio, GFP_NOIO,
							 &bt->bio_set);
				if (!chunkio)
					return -ENOMEM;

				bio_set_dev(chunkio, bdev);
				chunkio->bi_opf = REQ_OP_ZONE_APPEND |
						  (bio->bi_opf & ~REQ_OP_MASK);

				chunkio->bi_iter.bi_sector = bi_sector;
				chunkio->bi_iter.bi_size = bytes_in_shard;
				chunkio->bi_end_io = biza_chunkio_endio;

				chunkioctx =
					kzalloc(sizeof(struct biza_chunkioctx),
						GFP_NOIO);
				if (!chunkioctx)
					return -ENOMEM;
				chunkioctx->sh = sh;
				chunkioctx->type = BIZA_DATA_WRITE;
				chunkioctx->stime = jiffies;
				chunkioctx->bt = bt;
				chunkioctx->lcn = lcn;
				chunkioctx->pcn = pcn;
				chunkioctx->drive_idx = drive_idx;
				chunkioctx->slot = sh->nr_data_written + i;
				chunkioctx->in_raum = false;
				chunkioctx->raum_flush_to_zone = false;

				chunkio->bi_private = chunkioctx;

				shioctx->data_pcns[i] = pcn;
				refcount_inc(&shioctx->ref);

				submit_bio_noacct(chunkio);

				bio_advance(bio, bytes_in_shard);
			} else {
				big_chunk = big_chunks[drive_idx];
				biza_update_pred(bt, big_chunk->start_pcn);

				raum_dev = &bt->raum_devs[drive_idx];
				bdev = raum_dev->bdev;

				pcn = big_chunk->start_pcn +
				      big_chunk->chunk_count;
				biza_raum_pcn_to_idx(bt, pcn, &new_drive_idx,
						     &offset);
				BUG_ON(new_drive_idx != drive_idx);
				BUG_ON(big_chunk->drive_idx != drive_idx);
				bi_sector = biza_raum_idx_to_sector(bt, offset);

				log("Writing RAUM oop lcn 0x%llx pcn 0x%llx drive %u start_pcn 0x%llx chunk_count %u offset 0x%llx sector 0x%llx shno 0x%llx\n",
				    lcn, pcn, drive_idx, big_chunk->start_pcn,
				    big_chunk->chunk_count, offset, bi_sector,
				    sh->no);

				if (WRITE_AMP_STAT)
					atomic64_add(
						bt->params->chunk_size_sector,
						&bt->data_write);
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
				xa_store_irq(&bt->dc, pcn, data_buffer,
					     GFP_KERNEL);

				chunkio = big_chunk->chunkio;

				if (!chunkio) {
					BUG_ON(1);
					return -EIO;
				}

				ret = bio_add_page(chunkio,
						   virt_to_page(data_buffer),
						   bt->params->chunk_size_byte,
						   0);
				if (ret != bt->params->chunk_size_byte) {
					pr_err("ERROR: drive_idx %u big_chunk->start_pcn 0x%llx chunk_count %u this start chunk 0x%x this start sector 0x%x\n",
					       drive_idx, big_chunk->start_pcn,
					       big_chunk->chunk_count,
					       big_chunk->this_write_start_chunk,
					       big_chunk
						       ->this_write_start_sector);
					BUG_ON(1);
					return -EIO;
				}
				chunkioctx =
					&big_chunk
						 ->ctx[big_chunk->chunk_count++];
				if (!chunkioctx) {
					BUG_ON(1);
					return -ENOMEM;
				}
				chunkioctx->sh = sh;
				chunkioctx->type = BIZA_DATA_WRITE;
				chunkioctx->stime = jiffies;
				chunkioctx->bt = bt;
				chunkioctx->lcn = lcn;
				chunkioctx->pcn = pcn;
				chunkioctx->drive_idx = drive_idx;
				chunkioctx->slot = sh->nr_data_written + i;
				chunkioctx->in_raum = true;
				chunkioctx->raum_flush_to_zone = false;
				big_chunk->this_write_chunk_count++;
				shioctx->data_pcns[i] = pcn;
				refcount_inc(&shioctx->ref);

				if (big_chunk->chunk_count ==
				    RAUM_BIG_CHUNK_PAGES) {
					log("data Just hit RAUM_BIG_CHUNK_PAGES!!!\n");
					submit_bio_noacct(chunkio);
					big_chunks[drive_idx] =
						biza_get_big_chunk(bt,
								   drive_idx);
				}
				bio_advance(bio, bt->params->chunk_size_byte);
			}
		}
	}

	log("starting on parity io\n");
	// send parity chunk I/O
	for (i = 0; i < bt->params->m; ++i) {
		if (chunk_cnt != bt->params->k) {
			if (sh->nr_data_written > 0) { // try in place update
				// log("parity if io\n");
				if (biza_can_raum_parity_update_in_place(
					    bt, sh->no, i)) {
					// log("update in place\n");
					stripe = xa_load(&bt->map->stripe_table,
							 sh->no);
					pcn = stripe->parity_pcns[i];
					biza_raum_pcn_to_idx(
						bt, pcn, &drive_idx, &offset);
					parity_oop = false;

					log("Writing RAUM parity in-place lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
					    lcn, pcn, drive_idx, offset,
					    bi_sector);

					if (WRITE_AMP_STAT)
						atomic64_add(
							bt->params
								->chunk_size_sector,
							&bt->parity_in_place_update);
				} else {
					// log("out of place paritial parity update 1\n");
					drive_idx = (sh->no + i) %
						    bt->params->nr_drives;
					big_chunk = big_chunks[drive_idx];
					pcn = big_chunk->start_pcn +
					      big_chunk->chunk_count;
					parity_oop = true;
					log("Writing RAUM parity OOP lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
					    lcn, pcn, drive_idx, offset,
					    bi_sector);

					if (WRITE_AMP_STAT)
						atomic64_add(
							bt->params
								->chunk_size_sector,
							&bt->oop_parity_write);
				}
			} else {
				log("other paritial parity update\n");
				drive_idx =
					(sh->no + i) % bt->params->nr_drives;
				big_chunk = big_chunks[drive_idx];
				pcn = big_chunk->start_pcn +
				      big_chunk->chunk_count;
				parity_oop = true;
				log("Writing RAUM parity else lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
				    lcn, pcn, drive_idx, offset, bi_sector);

				if (WRITE_AMP_STAT)
					atomic64_add(
						bt->params->chunk_size_sector,
						&bt->parity_write);
			}
			log("parity drive_idx=%u, offset=0x%llx, bi_sector=0x%llx, pcn=0x%llx\n",
			    drive_idx, offset, bi_sector, pcn);
			if (parity_oop) {
				chunkio = big_chunk->chunkio;
				if (!chunkio) {
					BUG_ON(1);
					return -ENOMEM;
				}

				bio_set_op_attrs(chunkio, REQ_OP_WRITE,
						 bio->bi_opf);
				ret = bio_add_page(
					chunkio,
					virt_to_page(
						sh->parity_cache +
						i * bt->params->chunk_size_byte),
					bt->params->chunk_size_byte, 0);
				if (ret != bt->params->chunk_size_byte) {
					log("ERROR: drive_idx %u big_chunk->start_pcn 0x%llx chunk_count %u this start chunk 0x%x this start sector 0x%x\n",
					    drive_idx, big_chunk->start_pcn,
					    big_chunk->chunk_count,
					    big_chunk->this_write_start_chunk,
					    big_chunk->this_write_start_sector);
					BUG_ON(1);
					return -EIO;
				}

				chunkioctx =
					&big_chunk
						 ->ctx[big_chunk->chunk_count++];
				if (!chunkioctx) {
					BUG_ON(1);
					return -ENOMEM;
				}
				chunkioctx->sh = sh;
				chunkioctx->type =
					shioctx->type ==
							BIZA_SH_IN_PLACE_UPDATE ?
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
				big_chunk->this_write_chunk_count++;
				shioctx->parity_pcns[i] = pcn;
				refcount_inc(&shioctx->ref);

				if (big_chunk->chunk_count ==
				    RAUM_BIG_CHUNK_PAGES) {
					log("parity Just hit RAUM_BIG_CHUNK_PAGES!!!\n");
					submit_bio_noacct(chunkio);
					big_chunks[drive_idx] =
						biza_get_big_chunk(bt,
								   drive_idx);
				}
			} else {
				// log("parity in-place 1\n");
				chunkio = bio_alloc_bioset(GFP_NOIO, 1,
							   &bt->bio_set);
				ret = bio_add_page(
					chunkio,
					virt_to_page(
						sh->parity_cache +
						i * bt->params->chunk_size_byte),
					bt->params->chunk_size_byte, 0);
				// log("parity in-place 2\n");
				if (ret != bt->params->chunk_size_byte) {
					log("ERROR: drive_idx %u big_chunk->start_pcn 0x%llx chunk_count %u this start chunk 0x%x this start sector 0x%x\n",
					    drive_idx, big_chunk->start_pcn,
					    big_chunk->chunk_count,
					    big_chunk->this_write_start_chunk,
					    big_chunk->this_write_start_sector);
					BUG_ON(1);
					return -EIO;
				}

				// log("parity in-place 3\n");
				bio_set_dev(chunkio,
					    bt->raum_devs[drive_idx].bdev);
				chunkio->bi_iter.bi_sector = bi_sector;
				chunkio->bi_iter.bi_size =
					bt->params->chunk_size_byte;
				chunkio->bi_end_io = biza_chunkio_endio;
				// log("parity in-place 4\n");
				chunkioctx =
					kzalloc(sizeof(struct biza_chunkioctx),
						GFP_NOIO);
				if (!chunkioctx) {
					BUG_ON(1);
					return -ENOMEM;
				}
				// log("parity in-place 5\n");
				chunkioctx->sh = sh;
				chunkioctx->type =
					shioctx->type ==
							BIZA_SH_IN_PLACE_UPDATE ?
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
				// log("parity in-place 6\n");
				shioctx->parity_pcns[i] = pcn;
				// log("parity in-place 7\n");
				refcount_inc(&shioctx->ref);
				// log("parity in-place 8\n");
				submit_bio_noacct(chunkio);
			}
		} else {
			log("full stripe parity write\n");
			drive_idx = (sh->no + i) % bt->params->nr_drives;
			big_chunk = big_chunks[drive_idx];
			biza_get_write_location(bt, lcn, drive_idx, &zone_idx,
						&offset, sectors_in_shard);
			pcn = biza_idx_to_pcn(bt, drive_idx, zone_idx, offset);
			bi_sector = biza_idx_to_sector(bt, drive_idx, zone_idx,
						       offset, true);
			parity_oop = true;
			log("Writing RAUM full stripe parity lcn 0x%llx pcn 0x%llx drive %u offset 0x%llx sector 0x%llx\n",
			    lcn, pcn, drive_idx, offset, bi_sector);

			if (WRITE_AMP_STAT)
				atomic64_add(sectors_in_shard,
					     &bt->parity_write);

			chunkio = bio_alloc_bioset(GFP_NOIO, 1, &bt->bio_set);
			ret = bio_add_page(chunkio,
					   virt_to_page(sh->parity_cache +
							i * bytes_in_shard),
					   bytes_in_shard, 0);
			// log("parity in-place 2\n");
			if (ret != bytes_in_shard) {
				BUG_ON(1);
				return -EIO;
			}

			// log("parity in-place 3\n");
			bio_set_dev(chunkio, bt->devs[drive_idx].dev->bdev);
			chunkio->bi_opf = REQ_OP_ZONE_APPEND |
					  (bio->bi_opf & ~REQ_OP_MASK);
			chunkio->bi_iter.bi_sector = bi_sector;
			chunkio->bi_iter.bi_size = bytes_in_shard;
			chunkio->bi_end_io = biza_chunkio_endio;
			// log("parity in-place 4\n");
			chunkioctx = kzalloc(sizeof(struct biza_chunkioctx),
					     GFP_NOIO);
			if (!chunkioctx) {
				BUG_ON(1);
				return -ENOMEM;
			}
			// log("parity in-place 5\n");
			chunkioctx->sh = sh;
			chunkioctx->type =
				shioctx->type == BIZA_SH_IN_PLACE_UPDATE ?
					BIZA_PARITY_UPDATE :
					BIZA_PARITY_WRITE;
			;
			chunkioctx->bt = bt;
			chunkioctx->lcn = BIZA_MAP_PARITY;
			chunkioctx->parity_start_data_lcn =
				sh->ioctx->lcn_start;
			chunkioctx->pcn = pcn;
			chunkioctx->drive_idx = drive_idx;
			chunkioctx->slot = i;
			// We always write parity chunks to RAUM
			// Even if it has been previously evicted to flash
			chunkioctx->in_raum = false;
			chunkioctx->raum_flush_to_zone = false;
			chunkio->bi_private = chunkioctx;
			// log("parity in-place 6\n");
			shioctx->parity_pcns[i] = pcn;
			// log("parity in-place 7\n");
			refcount_inc(&shioctx->ref);
			// log("parity in-place 8\n");
			submit_bio_noacct(chunkio);
		}
	}

	biza_end_stripe_head_io(sh, BLK_STS_OK);

	return 0;
}

// Process write request of a full stripe
static int biza_handle_full_stripe_write(struct biza_target *bt,
					 struct bio *bio,
					 biza_raum_big_chunk_t **big_chunks,
					 uint64_t chunks_in_shard)
{
	biza_stripe_head_t *sh;
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	int ret;

	sh = biza_alloc_empty_stripe_head(bt, chunks_in_shard);
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
	sh->chunks_in_shard = chunks_in_shard;
	refcount_inc(&bioctx->ref);
	// refcount_add(bt->params->k, &bioctx->ref);

	ret = biza_compute_parity(bt, bio, sh, bt->params->k, chunks_in_shard);
	if (ret) {
		pr_err("dm-biza: io error: compute parity error");
		return -EIO;
	}

	ret = biza_submit_stripe_head_write(bt, bio, sh, bt->params->k,
					    big_chunks, chunks_in_shard);
	if (ret) {
		pr_err("dm-biza: io error: cannot submit full stripe write, err %d",
		       ret);
		return -EIO;
	}

	return 0;
}

// Process write request of a partial stripe
static int biza_handle_partial_stripe_write(struct biza_target *bt,
					    struct bio *bio, uint8_t chunk_cnt,
					    biza_raum_big_chunk_t **big_chunks)
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
		sh->chunks_in_shard = 1;
		refcount_inc(&bioctx->ref);
		// refcount_add(nr_data_write, &bioctx->ref);

		ret = biza_compute_parity(bt, bio, sh, nr_data_write, 1);
		if (ret) {
			pr_err("dm-biza: io error: compute parity error");
			return -ENOMEM;
		}

		ret = biza_submit_stripe_head_write(bt, bio, sh, nr_data_write,
						    big_chunks, 1);
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
					    biza_stripe_head_t *sh,
					    biza_raum_big_chunk_t **big_chunks)
{
	void **srcs = kzalloc(sizeof(uint64_t *), GFP_KERNEL);
	struct biza_bioctx *bioctx =
		dm_per_bio_data(bio, sizeof(struct biza_bioctx));
	sector_t lcn, pcn;
	uint8_t *org_data, *bvec_start;
	int ret;
	struct bvec_iter iter;
	struct bio_vec bvec;

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
	iter = bio->bi_iter;
	bvec = bio_iter_iovec(bio, iter);
	bvec_start = bvec_kmap_local(&bvec);
	srcs[0] = bvec_start;
	xor_blocks(1, bt->params->chunk_size_byte, sh->parity_cache, srcs);
	// bvec_start = bvec_kmap_local(&bio->bi_io_vec[0]);
	// srcs[0] = bvec_start + bio->bi_iter.bi_bvec_done;
	// xor_blocks(1, bt->params->chunk_size_byte, sh->parity_cache, srcs);

	// update data_buffer
	memcpy(org_data, bvec_start, bt->params->chunk_size_byte);
	kunmap_local(bvec_start);
	// memcpy(org_data, srcs[0], bt->params->chunk_size_byte);
	// kunmap_local(bvec_start);

	kfree(srcs);

	/** submit stripe head **/
	ret = biza_submit_stripe_head_write(bt, bio, sh, 1, big_chunks, 1);
	if (ret) {
		pr_err("dm-biza: io error: cannot submit data update in place");
		return -EIO;
	}

	return 0;
}

// Process a write request
static int biza_handle_write(struct biza_target *bt, struct bio *bio)
{
	sector_t left, cur_lcn, original_left;
	uint8_t chunk_cnt;
	biza_stripe_head_t *sh;
	biza_raum_big_chunk_t **big_chunks =
		kzalloc(sizeof(biza_raum_big_chunk_t *) * bt->params->nr_drives,
			GFP_NOIO);
	biza_raum_big_chunk_t *big_chunk;
	sector_t biza_flush_lcn = BIZA_MAP_INVALID;
	sector_t pcn = BIZA_MAP_INVALID;
	uint8_t shift;
	uint64_t chunks;
	int ret, i;

	for (i = 0; i < bt->params->nr_drives; i++) {
		big_chunks[i] = biza_get_big_chunk(bt, i);
	}

	// Try getting a big chunk that is currently in-use

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

	if (WRITE_AMP_STAT)
		atomic64_add(bio_sectors(bio), &bt->user_send);

	// Try to get some larger chunks directly written to Zones

	for (shift = bt->params->max_chunk_size_sector_shift;
	     shift > bt->params->chunk_size_sector_shift; shift--) {
		original_left = left = bio_sectors(bio) >> shift;
		chunks = RAUM_LARGER_CHUNK_PAGES >>
			 (bt->params->max_chunk_size_sector_shift - shift);
		log("Left: %llu large chunks, trying %llu chunks\n", left,
		    chunks);

		while (left >= bt->params->k) {
			// cur_lcn = bio->bi_iter.bi_sector >>
			// 	  bt->params->chunk_size_sector_shift;
			// for (i = 0; i < bt->params->k * RAUM_LARGER_CHUNK_PAGES; i++) {
			// 	pcn = bt->map->l2p[cur_lcn].chunk_no;
			// 	if (biza_check_pcn_in_raum(bt, pcn)) {
			// 		goto out;
			// 	}
			// }

			ret = biza_handle_full_stripe_write(bt, bio, big_chunks,
							    chunks);
			if (ret)
				return -EIO;

			left = bio_sectors(bio) >> shift;
		}
	}

	left = bio_sectors(bio) >> bt->params->chunk_size_sector_shift;
	log("Left: %llu small chunks\n", left);

	while (left > 0) {
		cur_lcn = bio->bi_iter.bi_sector >>
			  bt->params->chunk_size_sector_shift;

		if (biza_is_data_in_raum(bt, cur_lcn)) {
			pr_err("2 performing In-place update lcn 0x%llx, pcn 0x%llx\n",
			       cur_lcn, bt->map->l2p[cur_lcn].chunk_no);
			sh = biza_data_update_get_sh(bt, cur_lcn);
			if (sh &&
			    biza_can_raum_data_update_in_place(bt, cur_lcn)) {
				pr_err("Found sh!!!\n");
				ret = biza_handle_data_in_place_update(
					bt, bio, sh, big_chunks);
				if (ret)
					return -EIO;
				left = bio_sectors(bio) >>
				       bt->params->chunk_size_sector_shift;
				continue;
			} else {
				pr_err("2 sh not found\n");
			}
		}
		chunk_cnt = 0;

		// while (chunk_cnt < left && chunk_cnt < bt->params->k) {
		while (chunk_cnt < left && chunk_cnt < bt->params->k &&
		       !biza_is_data_in_raum(bt, cur_lcn)) {
			cur_lcn++;
			chunk_cnt++;
		}

		if (chunk_cnt == bt->params->k) {
			ret = biza_handle_full_stripe_write(bt, bio, big_chunks,
							    1);
			if (ret)
				return -EIO;
		} else if (chunk_cnt > 0 && chunk_cnt < bt->params->k) {
			ret = biza_handle_partial_stripe_write(
				bt, bio, chunk_cnt, big_chunks);
			if (ret)
				return -EIO;
		} else
			BUG_ON(chunk_cnt != 0);
		left = bio_sectors(bio) >> bt->params->chunk_size_sector_shift;
	}

	for (i = 0; i < bt->params->nr_drives; i++) {
		big_chunk = big_chunks[i];
		if (big_chunk->this_write_chunk_count > 0) {
			// Submit bio
			submit_bio_noacct(big_chunk->chunkio);
		} else {
			// Put unused big chunks back
			biza_put_big_chunk_back(bt, big_chunk);
		}
	}

	// bio_advance_iter(bio, &iter, iter.bi_size);

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
	sector_t lcn, pcn, temp_lcn, temp_pcn;
	int ret;
	int i;

	left = bio_sectors(bio);
	// pr_err("Got read: 0x%llx %llu sectors\n", left, left);

	if (WRITE_AMP_STAT)
		atomic64_add(left, &bt->user_read);

	while (left > 0) {
		cur_sec = bio->bi_iter.bi_sector;
		// e.g., chunk = 128 sec, 0->128, 32->128
		nxt_sec = min(round_up(cur_sec + 1,
				       bt->params->chunk_size_sector),
			      bio_end_sector(bio));
		size = (nxt_sec - cur_sec) << SECTOR_SHIFT;

		lcn = cur_sec >> bt->params->chunk_size_sector_shift;
		pcn = biza_map_lcn_lookup_pcn(bt, lcn);

		for (i = 1;; i++) {
			temp_lcn = lcn + i;
			if (temp_lcn >= bt->params->nr_chunks) {
				// pr_err("temp_lcn OOB!\n");
				break;
			}
			temp_pcn = biza_map_lcn_lookup_pcn(bt, temp_lcn);
			if (pcn + i == temp_pcn ||
			    ((pcn == BIZA_MAP_UNMAPPED ||
			      pcn == BIZA_MAP_INVALID) &&
			     (temp_pcn == BIZA_MAP_UNMAPPED ||
			      temp_pcn == BIZA_MAP_INVALID))) {
				// if (pcn + i == temp_pcn) {
				// pr_err("size: 0x%llx, %llu, left_size: 0x%llx, %llu\n",
				//        size, size, left << SECTOR_SHIFT,
				//        left << SECTOR_SHIFT);
				if (size + (bt->params->chunk_size_byte) >
				    left << SECTOR_SHIFT) {
					break;
				}
				// pr_err("Got coalesced!\n");
				size += bt->params->chunk_size_byte;
			} else {
				break;
			}
		}

		ret = biza_submit_chunk_read(bt, bio, lcn, pcn, size);
		if (ret) {
			pr_err("dm-biza: io error: cannot submit chunk read");
			return -EIO;
		}

		left = bio_sectors(bio);
	}

	return 0;
}

static int biza_handle_discard(struct biza_target *bt, struct bio *bio)
{
	return 0;
}

// Entry of I/O handling
static void biza_handle_bio(struct biza_target *bt, struct bio *bio)
{
	enum req_opf op;
	int ret;

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
	case REQ_OP_DISCARD:
		ret = biza_handle_discard(bt, bio);
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
	// /* Enable discards */
	// limits->discard_granularity = 4096; // Usually 1 sector or page size
	// limits->max_discard_sectors = UINT_MAX; // Max size of a single discard
	// limits->max_hw_discard_sectors = UINT_MAX;

	// /* This tells the DM core that this target supports discards */
	// ti->discards_supported = true;
	// ti->num_discard_bios = 1;

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
