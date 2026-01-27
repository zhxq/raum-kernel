#include "dm-biza.h"

// alloc and init a stripe (not stripe head!!!)
static struct biza_stripe *biza_alloc_stripe(struct biza_target *bt)
{
	struct biza_stripe *stripe = NULL;
	int i;

	stripe = kzalloc(sizeof(struct biza_stripe), GFP_ATOMIC);
	if (!stripe)
		goto err;

	stripe->parity_pcns =
		kvzalloc(bt->params->m * sizeof(sector_t), GFP_ATOMIC);
	if (!stripe->parity_pcns)
		goto err_stripe;
	for (i = 0; i < bt->params->m; ++i) {
		stripe->parity_pcns[i] = BIZA_MAP_UNMAPPED;
	}

	stripe->data_lcns =
		kvzalloc(bt->params->k * sizeof(sector_t), GFP_ATOMIC);
	if (!stripe->data_lcns)
		goto err_parity;
	for (i = 0; i < bt->params->k; ++i) {
		stripe->data_lcns[i] = BIZA_MAP_UNMAPPED;
	}

	stripe->used = 0;
	stripe->valid = 0;

	return stripe;

err_parity:
	kvfree(stripe->parity_pcns);
err_stripe:
	kfree(stripe);
err:
	return NULL;
}

// free a stripe (not stripe head!!!)
static void biza_free_stripe(struct biza_stripe *stripe)
{
	kvfree(stripe->data_lcns);
	kvfree(stripe->parity_pcns);
	kfree(stripe);
}

/**
 * idx to sector (in drive)
 */
inline sector_t biza_raum_idx_to_sector(struct biza_target *bt, uint64_t offset)
{
	return (offset << bt->params->chunk_size_sector_shift);
}

/**
 * idx to sector (in drive)
 */
inline sector_t biza_idx_to_sector(struct biza_target *bt, uint8_t drive_idx,
				   uint32_t zone_idx, uint64_t offset,
				   bool ignore_offset)
{
	BUG_ON(zone_idx > bt->params->nr_zones_per_drive);

	// ZONE APPEND ONLY ACCEPTS ZONE START ADDR!
	if (ignore_offset) {
		offset = 0;
	} // But we still need a real offset when reading

	return (zone_idx * bt->devs[drive_idx].zones[0].len) +
	       (offset << bt->params->chunk_size_sector_shift);
}

inline sector_t biza_sector_to_pcn(struct biza_target *bt, uint8_t drive_idx,
				   sector_t sector)
{
	u64 zone_len = bt->devs[drive_idx].zones[0].len;
	u64 rem;
	u32 zone_idx;
	u64 chunk_off;

	zone_idx = div64_u64_rem(sector, zone_len, &rem);

	BUG_ON(zone_idx >= bt->params->nr_zones_per_drive);

	chunk_off = rem >> bt->params->chunk_size_sector_shift;

	return biza_idx_to_pcn(bt, drive_idx, zone_idx, chunk_off);
}

/**
 * RAUM idx to pcn
 */
inline sector_t biza_raum_idx_to_pcn(struct biza_target *bt, uint8_t drive_idx,
				     uint64_t offset)
{
	sector_t pcn = bt->params->nr_internal_chunks +
		       drive_idx * bt->params->nr_raum_chunks_per_drive +
		       offset;

	if (drive_idx >= bt->params->nr_drives) {
		BUG_ON(1);
	}
	if (pcn >=
	    bt->params->nr_internal_chunks + bt->params->nr_total_raum_chunks) {
		BUG_ON(1);
	}
	if (pcn < bt->params->nr_internal_chunks) {
		BUG_ON(1);
	}
	if (offset >= bt->params->nr_raum_chunks_per_drive) {
		BUG_ON(1);
	}

	return pcn;
}

/**
 * idx to pcn
 */
inline sector_t biza_idx_to_pcn(struct biza_target *bt, uint8_t drive_idx,
				uint32_t zone_idx, uint64_t offset)
{
	sector_t pcn = (drive_idx * bt->params->nr_zones_per_drive + zone_idx) *
			       bt->params->zone_capacity_chunk +
		       offset;

	if (drive_idx >= bt->params->nr_drives) {
		BUG_ON(1);
	}
	BUG_ON(zone_idx >= bt->params->nr_zones_per_drive);
	if (offset >= bt->params->zone_capacity_chunk) {
		BUG_ON(1);
	}

	return pcn;
}

/**
 * RAUM pcn to idx
 */
inline void biza_raum_pcn_to_idx(struct biza_target *bt, sector_t pcn,
				 uint8_t *drive_idx, uint64_t *offset)
{
	if (pcn < bt->params->nr_internal_chunks ||
	    pcn >= bt->params->nr_internal_chunks +
			    bt->params->nr_total_raum_chunks) {
		pr_err("illegal raum pcn: 0x%llx\n", pcn);
		BUG_ON(1);
	}
	pcn -= bt->params->nr_internal_chunks;
	*offset = pcn % bt->params->nr_raum_chunks_per_drive;
	*drive_idx = (pcn - *offset) / bt->params->nr_raum_chunks_per_drive;

	if (*drive_idx >= bt->params->nr_drives) {
		BUG_ON(1);
	}
	if (*offset >= bt->params->nr_raum_chunks_per_drive) {
		BUG_ON(1);
	}
}

/**
 * pcn to idx
 */
inline void biza_pcn_to_idx(struct biza_target *bt, sector_t pcn,
			    uint8_t *drive_idx, uint32_t *zone_idx,
			    uint64_t *offset)
{
	*drive_idx = pcn / (bt->params->nr_zones_per_drive *
			    bt->params->zone_capacity_chunk);
	*zone_idx = (pcn % (bt->params->nr_zones_per_drive *
			    bt->params->zone_capacity_chunk)) /
		    bt->params->zone_capacity_chunk;
	*offset = pcn % bt->params->zone_capacity_chunk;

	if (*drive_idx >= bt->params->nr_drives) {
		pr_err("drive_idx: %u, zone_idx: %u, offset: 0x%llx\n",
		       *drive_idx, *zone_idx, *offset);
		BUG_ON(1);
	}
	BUG_ON(*zone_idx >= bt->params->nr_zones_per_drive);
	if (*offset >= bt->params->zone_capacity_chunk) {
		BUG_ON(1);
	}
}

inline bool biza_check_pcn_in_raum(struct biza_target *bt, sector_t pcn)
{
	return pcn >= bt->params->nr_internal_chunks &&
	       pcn < bt->params->nr_internal_chunks +
			       bt->params->nr_total_raum_chunks;
}

// Lookup physical chunk number of chunk from lcn
inline sector_t biza_map_lcn_lookup_pcn(struct biza_target *bt, sector_t lcn)
{
	sector_t pcn;
	// struct biza_stripe *stripe;

	BUG_ON(lcn == BIZA_MAP_UNMAPPED || lcn == BIZA_MAP_INVALID ||
	       lcn == BIZA_MAP_PARITY);

	pcn = bt->map->l2p[lcn].chunk_no;

	return pcn;
}

// Lookup logical chunk number of chunk from pcn
inline sector_t biza_map_pcn_lookup_lcn(struct biza_target *bt, sector_t pcn)
{
	sector_t lcn;

	BUG_ON(pcn == BIZA_MAP_UNMAPPED || pcn == BIZA_MAP_INVALID ||
	       pcn == BIZA_MAP_PARITY);
	BUG_ON(pcn >= bt->params->nr_internal_chunks);

	lcn = bt->map->p2l[pcn].chunk_no;

	return lcn;
}

// Lookup physical chunk number of parity
inline sector_t biza_map_parity_lookup_pcn(struct biza_target *bt, uint64_t no,
					   uint8_t slot)
{
	sector_t pcn;
	struct biza_stripe *stripe;

	stripe = xa_load(&bt->map->stripe_table, no);
	if (!stripe)
		pcn = BIZA_MAP_UNMAPPED;
	else
		pcn = stripe->parity_pcns[slot];

	return pcn;
}

// Lookup stripe no
inline sector_t biza_map_lcn_lookup_stripe_no(struct biza_target *bt,
					      sector_t lcn)
{
	uint64_t stripe_no;

	stripe_no = bt->map->l2p[lcn].stripe_no;

	return stripe_no;
}

// Lookup stripe
inline struct biza_stripe *biza_map_lcn_lookup_stripe(struct biza_target *bt,
						      sector_t lcn)
{
	uint64_t stripe_no;
	struct biza_stripe *stripe;

	stripe_no = bt->map->l2p[lcn].stripe_no;
	stripe = xa_load(&bt->map->stripe_table, stripe_no);

	return stripe;
}

// Lookup stripe
inline uint64_t biza_map_pcn_lookup_stripe_no(struct biza_target *bt,
					      sector_t pcn)
{
	uint64_t stripe_no;

	stripe_no = bt->map->p2l[pcn].stripe_no;

	return stripe_no;
}

// Is the pcn storing valid data?
inline bool biza_map_is_data_in_pcn_useful(struct biza_target *bt, sector_t pcn)
{
	struct biza_stripe *stripe;
	bool ret;

	if (bt->map->p2l[pcn].chunk_no == BIZA_MAP_UNMAPPED ||
	    bt->map->p2l[pcn].chunk_no == BIZA_MAP_INVALID) {
		/**
         * WARN: Need modify: can only recycle stirpes with all invalid data now
         */
		if (bt->map->p2l[pcn].stripe_no == BIZA_MAP_UNMAPPED ||
		    bt->map->p2l[pcn].stripe_no == BIZA_MAP_INVALID)
			ret = false;
		else {
			stripe = xa_load(&bt->map->stripe_table,
					 bt->map->p2l[pcn].stripe_no);
			if (stripe)
				ret = true;
			else
				ret = false;
		}
	} else
		ret = true;

	return ret;
}

// update mapping tables because of data write/out-of-place update
void biza_map_update_data_wrt(struct biza_target *bt, sector_t lcn,
			      sector_t pcn, uint64_t no, uint8_t slot,
			      bool in_raum)
{
	sector_t org_pcn = BIZA_MAP_UNMAPPED;
	uint64_t org_stripe_no = BIZA_MAP_UNMAPPED;
	uint8_t org_slot = (uint8_t)BIZA_MAP_UNMAPPED;
	struct biza_stripe *stripe = NULL, *org_stripe = NULL;
	uint8_t org_drive_idx;
	uint32_t org_zone_idx;
	uint64_t org_offset;
	sector_t org_parity_pcn = BIZA_MAP_UNMAPPED;
	biza_free_raum_chunk_t *free_chunk;
	struct biza_raum_location_entry *org_raum_stripe_data;
	struct biza_raum_dev *dev;
	// unsigned long flags;
	int i;

	log("Data update lcn: 0x%llx, pcn: 0x%llx, stripe_no: 0x%llx, slot: %u, in_raum: %d\n",
	    lcn, pcn, no, slot, in_raum);

	org_pcn = bt->map->l2p[lcn].chunk_no;
	org_stripe_no = bt->map->l2p[lcn].stripe_no;
	org_slot = bt->map->l2p[lcn].slot;

	bt->map->l2p[lcn].chunk_no = pcn;
	bt->map->l2p[lcn].stripe_no = no;
	bt->map->l2p[lcn].slot = slot;
	bt->map->l2p[lcn].in_raum = in_raum;
	bt->map->p2l[pcn].chunk_no = lcn;
	bt->map->p2l[pcn].stripe_no = no;
	bt->map->p2l[pcn].slot = slot;
	bt->map->p2l[pcn].in_raum = in_raum;

	stripe = xa_load(&bt->map->stripe_table, no);
	if (!stripe) {
		stripe = biza_alloc_stripe(bt);
		xa_store(&bt->map->stripe_table, no, stripe, GFP_ATOMIC);
		// pr_err("data creating stripe: 0x%llx\n", no);
	}
	stripe->data_lcns[slot] = lcn;
	stripe->used++;
	stripe->valid++;

	if (org_pcn != BIZA_MAP_UNMAPPED) {
		bt->map->p2l[org_pcn].chunk_no = BIZA_MAP_INVALID;
		// DO NOT set stripe_no now, because when gc, we need recompute parity
		// bt->map->p2l[org_pcn].stripe_no = BIZA_MAP_INVALID;
		bt->map->p2l[org_pcn].slot = (uint8_t)BIZA_MAP_INVALID;
		if (!bt->map->p2l[org_pcn].in_raum) {
			// pr_err("Testing in_raum 1 0x%llx\n", org_pcn);
			// pr_err("Inif Data update lcn: 0x%llx, pcn: 0x%llx, original_pcn: 0x%llx, stripe_no: 0x%llx, slot: %u, in_raum: %d\n",
			//        lcn, org_pcn, pcn, no, slot, in_raum);
			biza_pcn_to_idx(bt, org_pcn, &org_drive_idx,
					&org_zone_idx, &org_offset);
			bt->devs[org_drive_idx]
				.zones[org_zone_idx]
				.nr_invalid_chunks++;
		}

		org_stripe = xa_load(&bt->map->stripe_table, org_stripe_no);
		BUG_ON(!org_stripe);
		org_stripe->data_lcns[org_slot] = BIZA_MAP_INVALID;

		// All data in original stripe is invalid
		if (--org_stripe->valid == 0) {
			if (org_stripe->used == bt->params->k) {
				for (i = 0; i < bt->params->m; ++i) {
					org_parity_pcn =
						org_stripe->parity_pcns[i];
					if (!biza_is_valid_pcn(
						    bt, org_parity_pcn)) {
						continue;
					}
					bt->map->p2l[org_parity_pcn].chunk_no =
						BIZA_MAP_INVALID;
					bt->map->p2l[org_parity_pcn].stripe_no =
						BIZA_MAP_INVALID;
					bt->map->p2l[org_parity_pcn].slot =
						(uint8_t)BIZA_MAP_INVALID;
					if (!bt->map->p2l[org_parity_pcn]
						     .in_raum) {
						// pr_err("Inloop Data update lcn: 0x%llx, pcn: 0x%llx, original_pcn: 0x%llx, stripe_no: 0x%llx, slot: %u, in_raum: %d\n",
						//        lcn, org_pcn, pcn, no,
						//        slot, in_raum);
						biza_pcn_to_idx(bt,
								org_parity_pcn,
								&org_drive_idx,
								&org_zone_idx,
								&org_offset);
						bt->devs[org_drive_idx]
							.zones[org_zone_idx]
							.nr_invalid_chunks++;
					}
				}

				// pr_err("freeing stripe: 0x%llx\n", org_stripe_no);
				dev = &bt->raum_devs[org_drive_idx];

				org_raum_stripe_data = xa_load(&bt->raum_parity,
							       org_stripe_no);
				// org_raum_stripe_data =
				// 	biza_raum_lru_htable_find(
				// 		bt, org_stripe_no, true);
				if (org_raum_stripe_data) {
					free_chunk = kzalloc(
						sizeof(biza_free_raum_chunk_t),
						GFP_ATOMIC);
					free_chunk->chunk =
						org_raum_stripe_data->raum_chunk;
					// pr_err("Locking drive_idx %u, raum_chunk 0x%llx\n",
					//        org_drive_idx,
					//        org_raum_stripe_data->raum_chunk);
					spin_lock(&dev->lru_list_lock);
					// pr_err("Locked drive_idx %u, raum_chunk 0x%llx\n",
					//        org_drive_idx,
					//        org_raum_stripe_data->raum_chunk);

					list_add_tail(&free_chunk->link,
						      &dev->free_raum_chunks);
					list_del(&org_raum_stripe_data->link);
					xa_erase(&bt->raum_parity,
						 org_stripe_no);

					// pr_err("Unlocking drive_idx %u, raum_chunk 0x%llx\n",
					//        org_drive_idx,
					//        org_raum_stripe_data->raum_chunk);
					spin_unlock(&dev->lru_list_lock);

					// pr_err("Unlocked drive_idx %u, raum_chunk 0x%llx\n",
					//        org_drive_idx,
					//        org_raum_stripe_data->raum_chunk);
					kfree(org_raum_stripe_data);
				}
				xa_erase(&bt->map->stripe_table, org_stripe_no);
				biza_free_stripe(org_stripe);
			}
		}
	}
}

// update mapping tables because of parity write/ out-of-place update
void biza_map_update_parity_wrt(struct biza_target *bt, sector_t pcn,
				uint64_t no, uint8_t slot, bool in_raum)
{
	struct biza_stripe *stripe = NULL;
	sector_t old_pcn;

	log("Parity update pcn: 0x%llx, stripe_no: 0x%llx, slot: %u, in_raum: %d\n",
	    pcn, no, slot, in_raum);
	stripe = xa_load(&bt->map->stripe_table, no);
	if (!stripe) {
		BUG_ON(slot);
		stripe = biza_alloc_stripe(bt);
		xa_store(&bt->map->stripe_table, no, stripe, GFP_ATOMIC);
		// pr_err("parity creating stripe: 0x%llx\n", no);
	} else {
		// Invalidate old entry
		old_pcn = stripe->parity_pcns[slot];
		// pr_err("Write parity: original: 0x%llx, new: 0x%llx\n", old_pcn, pcn);
		if (old_pcn != BIZA_MAP_INVALID &&
		    old_pcn != BIZA_MAP_UNMAPPED) {
			bt->map->p2l[old_pcn].chunk_no = BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].stripe_no = BIZA_MAP_INVALID;
			bt->map->p2l[old_pcn].slot = (uint8_t)BIZA_MAP_INVALID;
		}
	}

	// stripe->parity_pcns[slot] == BIZA_MAP_UNMAPPED
	bt->map->p2l[pcn].chunk_no = BIZA_MAP_PARITY;
	bt->map->p2l[pcn].stripe_no = no;
	bt->map->p2l[pcn].slot = slot;
	bt->map->p2l[pcn].in_raum = in_raum;

	stripe->parity_pcns[slot] = pcn;
}

// update map after data/parity moving
// Note that the data/parity storing in src_pcn and dst_pcn should be the same
void biza_map_remap(struct biza_target *bt, sector_t src_pcn, sector_t dst_pcn)
{
	sector_t lcn;
	uint64_t stripe_no;
	uint8_t slot;
	struct biza_stripe *stripe = NULL;

	// Debug
	uint8_t read_drive_idx = 0, passed_drive_idx = 0;
	uint32_t read_zone_idx = 0, passed_zone_idx = 0;
	uint64_t read_offset = 0, passed_offset = 0;

	lcn = bt->map->p2l[src_pcn].chunk_no;
	// be modified while GC
	if (lcn == BIZA_MAP_INVALID || lcn == BIZA_MAP_UNMAPPED)
		return;

	if (lcn == BIZA_MAP_PARITY) { // parity chunk
		stripe_no = bt->map->p2l[src_pcn].stripe_no;
		slot = bt->map->p2l[src_pcn].slot;
		stripe = xa_load(&bt->map->stripe_table, stripe_no);

		// BUG_ON(!stripe);
		// BUG_ON(stripe->parity_pcns[slot] != src_pcn);

		if (!stripe) {
			pr_err("!stripe lcn: 0x%llx, src pcn: 0x%llx, dst pcn: 0x%llx, stripe_no: 0x%llx, slot: 0x%x\n",
			       lcn, src_pcn, dst_pcn, stripe_no, slot);
			biza_pcn_to_idx(bt, src_pcn, &passed_drive_idx,
					&passed_zone_idx, &passed_offset);
			pr_err("read: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx; passed: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx\n",
			       BIZA_MAP_INVALID, read_drive_idx, read_zone_idx,
			       read_offset, src_pcn, passed_drive_idx,
			       passed_zone_idx, passed_offset);
		} else {
			if (stripe->parity_pcns[slot] != src_pcn) {
				pr_err("mismatch src_pcn -- lcn: 0x%llx, src pcn: 0x%llx, current slot pcn: 0x%llx, dst pcn: 0x%llx, stripe_no: 0x%llx, slot: 0x%x\n",
				       lcn, src_pcn, stripe->parity_pcns[slot],
				       dst_pcn, stripe_no, slot);
				biza_pcn_to_idx(bt, stripe->parity_pcns[slot],
						&read_drive_idx, &read_zone_idx,
						&read_offset);
				biza_pcn_to_idx(bt, src_pcn, &passed_drive_idx,
						&passed_zone_idx,
						&passed_offset);
				pr_err("read: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx; passed: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx\n",
				       stripe->parity_pcns[slot],
				       read_drive_idx, read_zone_idx,
				       read_offset, src_pcn, passed_drive_idx,
				       passed_zone_idx, passed_offset);
			}
			stripe->parity_pcns[slot] = dst_pcn;
			// pr_err("Remap parity: original: 0x%llx, new: 0x%llx\n", src_pcn, dst_pcn);
		}

	} else { // data chunk
		// BUG_ON(bt->map->l2p[lcn].chunk_no != src_pcn);
		if (bt->map->l2p[lcn].chunk_no != src_pcn) {
			pr_err("mismatch src_pcn -- lcn: 0x%llx, src pcn: 0x%llx, current lcn -> pcn: 0x%llx, dst pcn: 0x%llx, stripe_no: 0x%llx, slot: 0x%u\n",
			       lcn, src_pcn, bt->map->l2p[lcn].chunk_no,
			       dst_pcn, stripe_no, slot);
			biza_pcn_to_idx(bt, bt->map->l2p[lcn].chunk_no,
					&read_drive_idx, &read_zone_idx,
					&read_offset);
			biza_pcn_to_idx(bt, src_pcn, &passed_drive_idx,
					&passed_zone_idx, &passed_offset);
			pr_err("read: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx; passed: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx\n",
			       bt->map->l2p[lcn].chunk_no, read_drive_idx,
			       read_zone_idx, read_offset, src_pcn,
			       passed_drive_idx, passed_zone_idx,
			       passed_offset);
		}
		bt->map->l2p[lcn].chunk_no = dst_pcn;
	}
	bt->map->p2l[dst_pcn].chunk_no = bt->map->p2l[src_pcn].chunk_no;
	bt->map->p2l[dst_pcn].stripe_no = bt->map->p2l[src_pcn].stripe_no;
	bt->map->p2l[dst_pcn].slot = bt->map->p2l[src_pcn].slot;

	bt->map->p2l[src_pcn].chunk_no = BIZA_MAP_INVALID;
	bt->map->p2l[src_pcn].stripe_no = BIZA_MAP_INVALID;
	bt->map->p2l[src_pcn].slot = (uint8_t)BIZA_MAP_INVALID;
}

// Initialize map context
int biza_ctr_map(struct biza_target *bt)
{
	int ret;

	bt->map = kzalloc(sizeof(struct biza_map), GFP_KERNEL);
	if (!bt->map) {
		pr_err("dm-biza: Failed to allocate biza map\n");
		ret = -ENOMEM;
	}

	bt->map->l2p = kvmalloc_array(bt->params->nr_chunks,
				      sizeof(biza_addr_t), GFP_KERNEL);
	if (!bt->map->l2p) {
		pr_err("dm-biza: Failed to allocate l2p map\n");
		ret = -ENOMEM;
		goto err_map;
	}
	memset(bt->map->l2p, (uint8_t)BIZA_MAP_UNMAPPED,
	       bt->params->nr_chunks * sizeof(biza_addr_t));

	bt->map->p2l = kvmalloc_array(bt->params->nr_internal_chunks +
					      bt->params->nr_total_raum_chunks,
				      sizeof(biza_addr_t), GFP_KERNEL);
	if (!bt->map->p2l) {
		pr_err("dm-biza: Failed to allocate p2l map\n");
		ret = -ENOMEM;
		goto err_l2p;
	}
	memset(bt->map->p2l, (uint8_t)BIZA_MAP_UNMAPPED,
	       (bt->params->nr_internal_chunks +
		bt->params->nr_total_raum_chunks) *
		       sizeof(biza_addr_t));

	xa_init(&bt->map->stripe_table);

	return 0;

err_l2p:
	kvfree(bt->map->l2p);
err_map:
	kfree(bt->map);
	return ret;
}

// Destory map context
void biza_dtr_map(struct biza_target *bt)
{
	xa_destroy(&bt->map->stripe_table);
	kvfree(bt->map->p2l);
	kvfree(bt->map->l2p);
	kfree(bt->map);
}