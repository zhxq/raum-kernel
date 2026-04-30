#include "dm-biza.h"

// alloc and init a stripe (not stripe head!!!)
static struct biza_stripe *biza_alloc_stripe(struct biza_target *bt,
					     bool larger_chunk,
					     uint64_t chunks_in_shard)
{
	struct biza_stripe *stripe = NULL;
	int i, j;
	int num_chunks = chunks_in_shard;

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
	// stripe->parity_pcns =
	// 	kvzalloc(bt->params->m * sizeof(sector_t *), GFP_ATOMIC);
	// if (!stripe->parity_pcns)
	// 	goto err_stripe;
	// for (i = 0; i < bt->params->m; ++i) {
	// 	stripe->parity_pcns[i] =
	// 		kvzalloc(num_chunks * sizeof(sector_t), GFP_ATOMIC);
	// 	for (j = 0; j < num_chunks; j++) {
	// 		stripe->parity_pcns[i][j] = BIZA_MAP_UNMAPPED;
	// 	}
	// }

	stripe->data_lcns =
		kvzalloc(bt->params->k * sizeof(sector_t), GFP_ATOMIC);
	if (!stripe->data_lcns)
		goto err_parity;

	// for (i = 0; i < bt->params->k; ++i) {
	// 	stripe->data_lcns[i] =
	// 		kvzalloc(num_chunks * sizeof(sector_t), GFP_ATOMIC);
	// 	for (j = 0; j < num_chunks; j++) {
	// 		stripe->data_lcns[i][j] = BIZA_MAP_UNMAPPED;
	// 	}
	// 	if (!stripe->data_lcns[i]) {
	// 		BUG_ON(1);
	// 		goto err_parity;
	// 	}
	// }

	stripe->used = 0;
	stripe->valid = 0;
	stripe->larger_chunk = larger_chunk;
	stripe->chunks_in_shard = chunks_in_shard;

	return stripe;

err_parity:
	kvfree(stripe->parity_pcns);
err_stripe:
	kfree(stripe);
err:
	return NULL;
}

// free a stripe (not stripe head!!!)
static void biza_free_stripe(struct biza_target *bt, struct biza_stripe *stripe)
{
	int i = 0;
	// for (i = 0; i < bt->params->k; i++) {
	// 	kvfree(stripe->data_lcns[i]);
	// }
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
		if ((zone_idx * bt->devs[drive_idx].zones[0].len) % 0x100 !=
		    0) {
			pr_err("Weird! zone_idx: 0x%x, zone_len: 0x%llx, product: 0x%llx, remainder: 0x%llx\n",
			       zone_idx, bt->devs[drive_idx].zones[0].len,
			       (zone_idx * bt->devs[drive_idx].zones[0].len),
			       (zone_idx * bt->devs[drive_idx].zones[0].len) %
				       0x100);
		}
		return (zone_idx * bt->devs[drive_idx].zones[0].len);
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
			      bool in_raum, bool larger_chunk,
			      uint64_t chunks_in_shard)
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
	int i = 0, j = 0, k = 0, max_i = chunks_in_shard;
	int original_stripe_max_i = 1;

	// log("1 Data update lcn 0x%llx, pcn 0x%llx, shno 0x%llx, slot %u, in_raum %d\n",
	//     lcn, pcn, no, slot, in_raum);

	stripe = xa_load(&bt->map->stripe_table, no);
	if (!stripe) {
		stripe = biza_alloc_stripe(bt, larger_chunk, chunks_in_shard);
		xa_store_irq(&bt->map->stripe_table, no, stripe, GFP_ATOMIC);
		// pr_err("data creating stripe: 0x%llx\n", no);
	}
	stripe->larger_chunk = larger_chunk;
	stripe->chunks_in_shard = chunks_in_shard;

	for (i = 0; i < max_i; i++) {
		org_pcn = bt->map->l2p[lcn + i].chunk_no;
		org_stripe_no = bt->map->l2p[lcn + i].stripe_no;
		org_slot = bt->map->l2p[lcn + i].slot;
		bt->map->l2p[lcn + i].chunk_no = pcn + i;
		bt->map->l2p[lcn + i].stripe_no = no;
		bt->map->l2p[lcn + i].slot = slot;
		bt->map->l2p[lcn + i].in_raum = in_raum;
		bt->map->p2l[pcn + i].chunk_no = lcn + i;
		bt->map->p2l[pcn + i].stripe_no = no;
		bt->map->p2l[pcn + i].slot = slot;
		bt->map->p2l[pcn + i].in_raum = in_raum;

		stripe->data_lcns[slot] = lcn;
		stripe->valid++;
		stripe->used++;

		// log("2 Data update lcn 0x%llx, pcn 0x%llx, shno 0x%llx, slot %u, in_raum %d\n",
		//     lcn, pcn, no, slot, in_raum);

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

			org_stripe =
				xa_load(&bt->map->stripe_table, org_stripe_no);
			if (!org_stripe) {
				pr_err("no org_stripe 0x%llx with org_pcn 0x%llx -- caused by lcn 0x%llx, i %d, lcn+i 0x%llx, pcn 0x%llx, shno 0x%llx, slot %u, in_raum %d, larger_chunk %d, chunks_in_shard %d\n",
				       org_stripe_no, org_pcn, lcn, i, lcn + i,
				       pcn, no, slot, in_raum, larger_chunk,
				       chunks_in_shard);
			}
			BUG_ON(!org_stripe);
			org_stripe->data_lcns[org_slot] = BIZA_MAP_INVALID;

			// All data in original stripe is invalid
			if (--org_stripe->valid == 0) {
				if (org_stripe->used ==
				    bt->params->k *
					    org_stripe->chunks_in_shard) {
					for (j = 0; j < bt->params->m; ++j) {
						// TODO: need to invalidate several PCN mappings when dealing with larger chunks
						org_parity_pcn =
							org_stripe
								->parity_pcns[j];
						original_stripe_max_i = 1;
						if (org_stripe->larger_chunk) {
							original_stripe_max_i =
								org_stripe
									->chunks_in_shard;
						}
						log("Cleaning stripe 0x%llx (org_num_chunks %d, org_stripe->used %d, org_pcn 0x%llx) due to lcn 0x%llx, i %d, lcn+i 0x%llx, pcn 0x%llx, shno 0x%llx, slot %u, in_raum %d, larger_chunk %d, chunks_in_shard %d\n",
						    org_stripe_no,
						    org_stripe->chunks_in_shard,
						    org_stripe->used,
						    org_parity_pcn, lcn, i,
						    lcn + i, pcn, no, slot,
						    in_raum, larger_chunk,
						    chunks_in_shard);
						for (k = 0;
						     k < original_stripe_max_i;
						     k++) {
							if (!biza_is_valid_pcn(
								    bt,
								    org_parity_pcn +
									    k)) {
								continue;
							}
							if (bt->map->p2l[org_parity_pcn +
									 k]
								    .chunk_no !=
							    BIZA_MAP_PARITY) {
								pr_err("Cleaning stripe 0x%llx at incorrect location 0x%llx (org_chunks_in_shard %d, org_stripe->used %d, org_pcn 0x%llx, mapped LCN 0x%llx, k %d) due to lcn 0x%llx, i %d, lcn+i 0x%llx, pcn 0x%llx, shno 0x%llx, slot %u, in_raum %d, larger_chunk %d, chunks_in_shard %d\n",
								       org_stripe_no,
								       org_parity_pcn +
									       k,
								       org_stripe
									       ->chunks_in_shard,
								       org_stripe
									       ->used,
								       org_parity_pcn,
								       bt->map->p2l[org_parity_pcn +
										    k]
									       .chunk_no,
								       k, lcn,
								       i,
								       lcn + i,
								       pcn, no,
								       slot,
								       in_raum,
								       larger_chunk,
								       chunks_in_shard);
								;
							}
							bt->map->p2l[org_parity_pcn +
								     k]
								.chunk_no =
								BIZA_MAP_INVALID;
							bt->map->p2l[org_parity_pcn +
								     k]
								.stripe_no =
								BIZA_MAP_INVALID;
							bt->map->p2l[org_parity_pcn +
								     k]
								.slot = (uint8_t)
								BIZA_MAP_INVALID;
							if (!bt->map->p2l[org_parity_pcn +
									  k]
								     .in_raum) {
								// pr_err("Inloop Data update lcn: 0x%llx, pcn: 0x%llx, original_pcn: 0x%llx, stripe_no: 0x%llx, slot: %u, in_raum: %d\n",
								//        lcn, org_pcn, pcn, no,
								//        slot, in_raum);
								biza_pcn_to_idx(
									bt,
									org_parity_pcn +
										k,
									&org_drive_idx,
									&org_zone_idx,
									&org_offset);
								bt->devs[org_drive_idx]
									.zones[org_zone_idx]
									.nr_invalid_chunks++;
							}
						}
					}
					// xa_erase_irq(&bt->map->stripe_table,
					// 	     org_stripe_no);
					// biza_free_stripe(bt, org_stripe);
				}
			}
		}
	}
}

// update mapping tables because of parity write/ out-of-place update
void biza_map_update_parity_wrt(struct biza_target *bt, sector_t pcn,
				uint64_t no, uint8_t slot, bool in_raum,
				bool larger_chunk, uint64_t chunks_in_shard)
{
	struct biza_stripe *stripe = NULL;
	sector_t old_pcn, old_data_start_lcn;
	uint64_t i;
	int subslot;

	// TODO: need to update several PCN mappings when dealing with larger chunks

	log("Parity update pcn 0x%llx, shno 0x%llx, slot %u, in_raum %d\n", pcn,
	    no, slot, in_raum);
	stripe = xa_load(&bt->map->stripe_table, no);
	if (!stripe) {
		BUG_ON(slot);
		stripe = biza_alloc_stripe(bt, larger_chunk, chunks_in_shard);
		xa_store_irq(&bt->map->stripe_table, no, stripe, GFP_ATOMIC);
		// pr_err("parity creating stripe: 0x%llx\n", no);
	} else {
		// Invalidate old entry
		for (i = 0; i < stripe->chunks_in_shard; i++) {
			old_pcn = stripe->parity_pcns[slot] + i;
			// pr_err("Write parity: original: 0x%llx, new: 0x%llx\n", old_pcn, pcn);
			if (old_pcn != BIZA_MAP_INVALID &&
			    old_pcn != BIZA_MAP_UNMAPPED) {
				bt->map->p2l[old_pcn].chunk_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[old_pcn].stripe_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[old_pcn].slot =
					(uint8_t)BIZA_MAP_INVALID;
			}
		}
	}

	stripe->parity_pcns[slot] = pcn;
	for (i = 0; i < chunks_in_shard; i++) {
		// stripe->parity_pcns[slot] == BIZA_MAP_UNMAPPED
		bt->map->p2l[pcn + i].chunk_no = BIZA_MAP_PARITY;
		bt->map->p2l[pcn + i].stripe_no = no;
		bt->map->p2l[pcn + i].slot = slot;
		bt->map->p2l[pcn + i].in_raum = in_raum;
	}
}

// update map after data/parity moving
// Note that the data/parity storing in src_pcn and dst_pcn should be the same
void biza_map_remap(struct biza_target *bt, sector_t src_pcn, sector_t dst_pcn,
		    struct xarray *parity_pcn)
{
	sector_t lcn;
	uint64_t stripe_no;
	uint8_t slot;
	struct biza_stripe *stripe = NULL;

	// Debug
	uint8_t read_drive_idx = 0, passed_drive_idx = 0;
	uint32_t read_zone_idx = 0, passed_zone_idx = 0;
	uint64_t read_offset = 0, passed_offset = 0;
	void *entry = NULL;
	uint64_t new_parity_start = 0;

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
			if (src_pcn != BIZA_MAP_INVALID &&
			    src_pcn != BIZA_MAP_UNMAPPED) {
				biza_pcn_to_idx(bt, src_pcn, &passed_drive_idx,
						&passed_zone_idx,
						&passed_offset);
			}

			pr_err("read: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx; passed: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx\n",
			       BIZA_MAP_INVALID, read_drive_idx, read_zone_idx,
			       read_offset, src_pcn, passed_drive_idx,
			       passed_zone_idx, passed_offset);
			bt->map->p2l[dst_pcn].chunk_no = BIZA_MAP_INVALID;
			bt->map->p2l[dst_pcn].stripe_no = BIZA_MAP_INVALID;
			bt->map->p2l[dst_pcn].slot = (uint8_t)BIZA_MAP_INVALID;

			if (src_pcn != BIZA_MAP_INVALID &&
			    src_pcn != BIZA_MAP_UNMAPPED) {
				bt->map->p2l[src_pcn].chunk_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[src_pcn].stripe_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[src_pcn].slot =
					(uint8_t)BIZA_MAP_INVALID;
			}
			return;
		} else {
			entry = xa_load(parity_pcn, stripe_no);
			if (xa_is_value(entry)) {
				new_parity_start = xa_to_value(entry);
			} else {
				xa_store(parity_pcn, stripe_no,
					 xa_mk_value(dst_pcn), GFP_ATOMIC);
				new_parity_start = dst_pcn;
				if (stripe->parity_pcns[slot] != src_pcn) {
					pr_err("mismatch parity src_pcn -- lcn: 0x%llx, src pcn: 0x%llx, current slot pcn: 0x%llx, dst pcn: 0x%llx, stripe_no: 0x%llx, slot: 0x%x\n",
					       lcn, src_pcn,
					       stripe->parity_pcns[slot],
					       dst_pcn, stripe_no, slot);
					if (src_pcn != BIZA_MAP_INVALID &&
					    src_pcn != BIZA_MAP_UNMAPPED &&
					    stripe->parity_pcns[slot] !=
						    BIZA_MAP_INVALID &&
					    stripe->parity_pcns[slot] !=
						    BIZA_MAP_UNMAPPED) {
						biza_pcn_to_idx(
							bt,
							stripe->parity_pcns[slot],
							&read_drive_idx,
							&read_zone_idx,
							&read_offset);
						biza_pcn_to_idx(
							bt, src_pcn,
							&passed_drive_idx,
							&passed_zone_idx,
							&passed_offset);
						pr_err("read: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx; passed: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx\n",
						       stripe->parity_pcns[slot],
						       read_drive_idx,
						       read_zone_idx,
						       read_offset, src_pcn,
						       passed_drive_idx,
						       passed_zone_idx,
						       passed_offset);
					}
					bt->map->p2l[dst_pcn].chunk_no =
						BIZA_MAP_INVALID;
					bt->map->p2l[dst_pcn].stripe_no =
						BIZA_MAP_INVALID;
					bt->map->p2l[dst_pcn].slot =
						(uint8_t)BIZA_MAP_INVALID;

					bt->map->p2l[src_pcn].chunk_no =
						BIZA_MAP_INVALID;
					bt->map->p2l[src_pcn].stripe_no =
						BIZA_MAP_INVALID;
					bt->map->p2l[src_pcn].slot =
						(uint8_t)BIZA_MAP_INVALID;
					return;
				} else {
					stripe->parity_pcns[slot] =
						new_parity_start;
				}
			}
			// pr_err("Remap parity: original: 0x%llx, new: 0x%llx\n", src_pcn, dst_pcn);
		}

	} else { // data chunk
		// BUG_ON(bt->map->l2p[lcn].chunk_no != src_pcn);
		if (bt->map->l2p[lcn].chunk_no != src_pcn) {
			pr_err("mismatch data src_pcn -- lcn: 0x%llx, src pcn: 0x%llx, current lcn -> pcn: 0x%llx, dst pcn: 0x%llx, stripe_no: 0x%llx, slot: 0x%u\n",
			       lcn, src_pcn, bt->map->l2p[lcn].chunk_no,
			       dst_pcn, stripe_no, slot);
			if (src_pcn != BIZA_MAP_INVALID &&
			    src_pcn != BIZA_MAP_UNMAPPED) {
				biza_pcn_to_idx(bt, bt->map->l2p[lcn].chunk_no,
						&read_drive_idx, &read_zone_idx,
						&read_offset);
				biza_pcn_to_idx(bt, src_pcn, &passed_drive_idx,
						&passed_zone_idx,
						&passed_offset);
				pr_err("read: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx; passed: src_pcn: 0x%llx, drive_idx: %u, zone_idx: %u, offset: 0x%llx\n",
				       bt->map->l2p[lcn].chunk_no,
				       read_drive_idx, read_zone_idx,
				       read_offset, src_pcn, passed_drive_idx,
				       passed_zone_idx, passed_offset);

				bt->map->l2p[lcn].chunk_no = BIZA_MAP_INVALID;

				bt->map->p2l[dst_pcn].chunk_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[dst_pcn].stripe_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[dst_pcn].slot =
					(uint8_t)BIZA_MAP_INVALID;

				bt->map->p2l[src_pcn].chunk_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[src_pcn].stripe_no =
					BIZA_MAP_INVALID;
				bt->map->p2l[src_pcn].slot =
					(uint8_t)BIZA_MAP_INVALID;
			}
			return;
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

	bt->map->l2p = vmalloc(bt->params->nr_chunks * sizeof(biza_addr_t));
	if (!bt->map->l2p) {
		pr_err("dm-biza: Failed to allocate l2p map\n");
		ret = -ENOMEM;
		goto err_map;
	}
	memset(bt->map->l2p, (uint8_t)BIZA_MAP_UNMAPPED,
	       bt->params->nr_chunks * sizeof(biza_addr_t));

	bt->map->p2l = vmalloc((bt->params->nr_internal_chunks +
				bt->params->nr_total_raum_chunks) *
			       sizeof(biza_addr_t));
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