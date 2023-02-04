#include "nvmev.h"
#include "ssd.h"
#include "zns.h"

#if SUPPORT_ZNS 
void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target, struct buffer * write_buffer, unsigned int buffs_to_release);

static inline  __u32 __nr_lbas_from_rw_cmd(struct nvme_rw_command * cmd)
{
	return cmd->length + 1;
}

static inline __u64 __lba_to_lpn(struct zns_ssd *zns_ssd, __u64 lba) 
{
	return lba / zns_ssd->sp.secs_per_pg;
}

static inline  __u64 __zone_end_lba(struct zns_ssd *zns_ssd, __u32 zid) 
{
	struct zone_descriptor *zone_descs = &(zns_ssd->zone_descs[zid]);

	return zone_descs->zslba + zone_descs->zone_capacity - 1; 
}

bool __check_boundary_error(struct zns_ssd *zns_ssd, __u64 slba, __u32 nr_lba)
{	
	return lba_to_zone(zns_ssd, slba) == lba_to_zone(zns_ssd, slba + nr_lba - 1);
}

void __increase_write_ptr(struct zns_ssd *zns_ssd, __u32 zid, __u32 nr_lba)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	__u64 cur_write_ptr = zone_descs[zid].wp;
	__u64 zone_capacity = zone_descs[zid].zone_capacity;
	
	cur_write_ptr += nr_lba;
	
	zone_descs[zid].wp = cur_write_ptr;

	if (cur_write_ptr == (zone_to_slba(zns_ssd, zid) + zone_capacity)) {
		//change state to ZSF
		release_zone_resource(zns_ssd, OPEN_ZONE);
		release_zone_resource(zns_ssd, ACTIVE_ZONE);

		if (zone_descs[zid].zrwav)
			ASSERT(0);

		change_zone_state(zns_ssd, zid, ZONE_STATE_FULL);
	}
	else if (cur_write_ptr > (zone_to_slba(zns_ssd, zid) +  zone_capacity)) {
		NVMEV_ERROR("[%s] Write Boundary error!!\n", __FUNCTION__);
	}
}

static inline struct ppa __lpn_to_ppa(struct zns_ssd *zns_ssd, uint64_t lpn) 
{
	struct ssdparams *spp = &(zns_ssd->sp);
	struct znsparams *zpp = &(zns_ssd->zp);
	__u64 zone = lpn_to_zone(zns_ssd, lpn); // find corresponding zone
	__u64 off = lpn - zone_to_slpn(zns_ssd, zone); 
	
	__u32 sdie = (zone * zpp->dies_per_zone) % spp->tt_luns;
	__u32 die = sdie + ((off / spp->pgs_per_oneshotpg) % zpp->dies_per_zone);

	__u32 channel = die_to_channel(zns_ssd, die);
	__u32 lun = die_to_lun(zns_ssd, die);
	struct ppa ppa = {0};

	ppa.g.lun = lun;
	ppa.g.ch = channel;
	ppa.g.pg = off % spp->pgs_per_oneshotpg;

    return ppa;
}

bool __zns_write(struct zns_ssd *zns_ssd, struct nvme_request * req, struct nvme_result * ret)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct ssdparams *spp = &(zns_ssd->sp);
	struct nvme_rw_command * cmd = &(req->cmd->rw);

	__u64 slba = cmd->slba;
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	
	__u64 slpn = __lba_to_lpn(zns_ssd, slba);
	__u64 elpn = __lba_to_lpn(zns_ssd, slba + nr_lba - 1);
	__u64 lpn;

	// get zone from start_lbai
	__u32 zid = lba_to_zone(zns_ssd, slba);
	enum zone_state state = zone_descs[zid].state;	

	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = nsecs_start, nsecs_latest;
	__u64 nsecs_xfer_completed = nsecs_start;
	__u32 status = NVME_SC_SUCCESS;
	
	struct ppa ppa;
	struct nand_cmd swr;

	__u64 pgs = 0, pg_off;

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n",__FUNCTION__, slba, nr_lba, zid, state);
	
	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0) {
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}

	if (__check_boundary_error(zns_ssd, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY;
		goto out;
	}
	
	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s WP error slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d\n",__FUNCTION__, slba, nr_lba, zid,  zns_ssd->zone_descs[zid].wp,  state);
		status = NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}
	
	switch (state) {
		case ZONE_STATE_EMPTY:
		{
			// check if slba == start lba in zone
			if (slba != zone_descs[zid].zslba) {
				status =  NVME_SC_ZNS_INVALID_WRITE;
				goto out;
			}	
			
			if (is_zone_resource_full(zns_ssd, ACTIVE_ZONE)) {
				status =  NVME_SC_ZNS_NO_ACTIVE_ZONE;
				goto out;
			}
			if (is_zone_resource_full(zns_ssd, OPEN_ZONE)) {
				status =  NVME_SC_ZNS_NO_OPEN_ZONE;
				goto out;
			}
			acquire_zone_resource(zns_ssd, ACTIVE_ZONE);
			// go through
		}
		case ZONE_STATE_CLOSED:
		{
			if (acquire_zone_resource(zns_ssd, OPEN_ZONE) == false) {
				status = NVME_SC_ZNS_NO_OPEN_ZONE;
				goto out;
			}
			
			// change to ZSIO
			change_zone_state(zns_ssd, zid, ZONE_STATE_OPENED_IMPL);
			break;
		}
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{
			break;
		
		}
		case ZONE_STATE_FULL :
			status = NVME_SC_ZNS_ERR_FULL;
		case ZONE_STATE_READ_ONLY :
			status = NVME_SC_ZNS_ERR_READ_ONLY;
		case ZONE_STATE_OFFLINE : 
			status = NVME_SC_ZNS_ERR_OFFLINE;
			goto out;
	}
	
	__increase_write_ptr(zns_ssd, zid, nr_lba);

	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest += spp->fw_wr0_lat;
	nsecs_latest += spp->fw_wr1_lat * (elpn - slpn + 1);
	nsecs_latest = ssd_advance_pcie(&zns_ssd->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	for (lpn = slpn; lpn <= elpn; lpn+=pgs) {
		ppa = __lpn_to_ppa(zns_ssd, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(elpn - lpn + 1, (__u64)(spp->pgs_per_oneshotpg - pg_off));

		/* Aggregate write io in flash page */
		if ((pg_off + pgs) == spp->pgs_per_oneshotpg) {	
			swr.type = USER_IO;
			swr.cmd = NAND_WRITE;
			swr.stime = nsecs_xfer_completed;
			swr.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz;
			swr.interleave_pci_dma = false;

			nsecs_completed = ssd_advance_status((struct ssd*)zns_ssd, &ppa, &swr);
			nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
			NVMEV_ZNS_DEBUG("%s Flush slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n",__FUNCTION__, slba, nr_lba, zid, state);
	
			enqueue_writeback_io_req(req->sq_id, nsecs_completed, zns_ssd->write_buffer, spp->pgs_per_oneshotpg * spp->pgsz);
		} 
	}

out :
	ret->status = status;
	if (cmd->control & NVME_RW_FUA) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;

	return true;
}

bool __zns_write_zrwa(struct zns_ssd *zns_ssd, struct nvme_request * req, struct nvme_result * ret)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct ssdparams *spp = &(zns_ssd->sp);
	struct znsparams *zpp = &(zns_ssd->zp);
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	__u64 slba = cmd->slba;
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	__u64 elba = cmd->slba + nr_lba -1;
		
	// get zone from start_lbai
	__u32 zid = lba_to_zone(zns_ssd, slba);
	enum zone_state state = zone_descs[zid].state;	
	
	__u64 prev_wp = zone_descs[zid].wp;
	const __u32 lbas_per_zrwa = zpp->lbas_per_zrwa;
	const __u32 lbas_per_zrwafg = zpp->lbas_per_zrwafg;
	__u64 zrwa_impl_start = prev_wp + lbas_per_zrwa;
	__u64 zrwa_impl_end = prev_wp + (2*lbas_per_zrwa) - 1;
	
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest;
	__u64 nsecs_xfer_completed;
	__u32 status = NVME_SC_SUCCESS;

	struct ppa ppa;
	struct nand_cmd swr;

	__u64 nr_lbas_flush = 0, lpn, remaining, pgs = 0, pg_off;
	
	NVMEV_DEBUG("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx zrwa_impl_start 0x%llx zrwa_impl_end 0x%llx  buffer %d\n",
														__FUNCTION__, slba, nr_lba, zid, state, prev_wp, zrwa_impl_start, zrwa_impl_end, zns_ssd->zwra_buffer[zid].remaining);
	

	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0) {
		status =  NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}
	
	if (__check_boundary_error(zns_ssd, slba, nr_lba) == false) {
		// return boundary error
		status =  NVME_SC_ZNS_ERR_BOUNDARY;
		goto out;
	}
	
	// valid range : wp <=  <= wp + 2*(size of zwra) -1
	if (slba < zone_descs[zid].wp || elba > zrwa_impl_end) {
		NVMEV_ERROR("%s slba 0x%llx nr_lba 0x%llx zone_id %d wp 0x%llx state %d\n",__FUNCTION__, slba, nr_lba, zid,  zone_descs[zid].wp,  state);
		status =  NVME_SC_ZNS_INVALID_WRITE;
		goto out;
	}
	
	switch (state) {
		case ZONE_STATE_CLOSED:
		case ZONE_STATE_EMPTY : 
		{
			if (acquire_zone_resource(zns_ssd, OPEN_ZONE) == false) {
				status =  NVME_SC_ZNS_NO_OPEN_ZONE;
				goto out;
			}
			
			if (!buffer_allocate(&zns_ssd->zwra_buffer[zid], zpp->zrwa_size)) 
				NVMEV_ASSERT(0);

			// change to ZSIO
			change_zone_state(zns_ssd, zid, ZONE_STATE_OPENED_IMPL);
			break;
		}
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{
			break;
		
		}
		case ZONE_STATE_FULL :
			status =  NVME_SC_ZNS_ERR_FULL;
			goto out;
		case ZONE_STATE_READ_ONLY :
			status =  NVME_SC_ZNS_ERR_READ_ONLY;
			goto out;
		case ZONE_STATE_OFFLINE : 
			status =  NVME_SC_ZNS_ERR_OFFLINE;
			goto out;
	#if 0
		case ZONE_STATE_EMPTY : 
			return NVME_SC_ZNS_INVALID_ZONE_OPERATION;
	#endif
	}
	
	if (elba >= zrwa_impl_start) {
		nr_lbas_flush = DIV_ROUND_UP((elba - zrwa_impl_start + 1), lbas_per_zrwafg) * lbas_per_zrwafg;

		NVMEV_DEBUG("%s implicitly flush zid %d wp before 0x%llx after 0x%llx buffer %d",
														__FUNCTION__, zid,  prev_wp, zone_descs[zid].wp + nr_lbas_flush, zns_ssd->zwra_buffer[zid].remaining);
	} else if (elba == __zone_end_lba(zns_ssd, zid)) { 
		// Workaround. move wp to end of the zone and make state full implicitly
		nr_lbas_flush = elba - prev_wp + 1;

		NVMEV_DEBUG("%s end of zone zid %d wp before 0x%llx after 0x%llx buffer %d",
														__FUNCTION__, zid, prev_wp, zone_descs[zid].wp + nr_lbas_flush, zns_ssd->zwra_buffer[zid].remaining);
	} 

	if (nr_lbas_flush > 0) {
		if (!buffer_allocate(&zns_ssd->zwra_buffer[zid], LBA_TO_BYTE(nr_lbas_flush))) 
				return false;

		__increase_write_ptr(zns_ssd, zid, nr_lbas_flush);
	}
	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest += spp->fw_wr0_lat;
	nsecs_latest += spp->fw_wr1_lat * DIV_ROUND_UP(elba - slba + 1, spp->secs_per_pg);
	nsecs_latest = ssd_advance_pcie(&zns_ssd->ssd, nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	lpn = __lba_to_lpn(zns_ssd, prev_wp);
	remaining = nr_lbas_flush / spp->secs_per_pg;
	/* Aggregate write io in flash page */
	while (remaining > 0) {
		ppa = __lpn_to_ppa(zns_ssd, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_oneshotpg;
		pgs = min(remaining, (__u64)(spp->pgs_per_oneshotpg - pg_off));

		if ((pg_off + pgs) == spp->pgs_per_oneshotpg) {	
			swr.type = USER_IO;
			swr.cmd = NAND_WRITE;
			swr.stime = nsecs_xfer_completed;
			swr.xfer_size = spp->pgs_per_oneshotpg * spp->pgsz;
			swr.interleave_pci_dma = false;

			nsecs_completed = ssd_advance_status((struct ssd*)zns_ssd, &ppa, &swr);
			nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;

			enqueue_writeback_io_req(req->sq_id, nsecs_completed, &zns_ssd->zwra_buffer[zid], spp->pgs_per_oneshotpg * spp->pgsz);
		} 

		lpn+=pgs;
		remaining-=pgs;
	}

out : 
	ret->status = status;

	if (cmd->control & NVME_RW_FUA) /*Wait all flash operations*/
		ret->nsecs_target = nsecs_latest;
	else /*Early completion*/
		ret->nsecs_target = nsecs_xfer_completed;

	return true;
}

bool zns_write(struct nvme_request * req, struct nvme_result * ret)
{
	struct zns_ssd *zns_ssd = zns_ssd_instance();
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	__u64 slpn = __lba_to_lpn(zns_ssd, cmd->slba);

	// get zone from start_lba
	__u32 zid = lpn_to_zone(zns_ssd, slpn);

	NVMEV_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d \n",__FUNCTION__, cmd->slba, nr_lba, zid);
	
	if (buffer_allocate(zns_ssd->write_buffer, LBA_TO_BYTE(nr_lba)) < LBA_TO_BYTE(nr_lba))
			return false;

	if (zone_descs[zid].zrwav == 0) 
		return __zns_write(zns_ssd, req, ret);
	else 
		return __zns_write_zrwa(zns_ssd, req, ret);
}

bool zns_read(struct nvme_request * req, struct nvme_result * ret)
{
	struct zns_ssd *zns_ssd= zns_ssd_instance();
	struct ssdparams *spp = &(zns_ssd->sp);
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	
	__u64 slba = cmd->slba;
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	
	__u64 slpn = __lba_to_lpn(zns_ssd, slba);
	__u64 elpn = __lba_to_lpn(zns_ssd, slba + nr_lba - 1);
	__u64 lpn;

	// get zone from start_lba
	__u32 zid = lpn_to_zone(zns_ssd, slpn);
	__u32 status = NVME_SC_SUCCESS;
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = nsecs_start, nsecs_latest = 0;	
	__u64 pgs = 0, pg_off;
	struct ppa ppa;
	struct nand_cmd swr;

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d wp 0x%llx last lba 0x%llx\n",
	__FUNCTION__, slba, nr_lba, zid, zone_descs[zid].state,  zone_descs[zid].wp,  (slba + nr_lba - 1));

	if (zone_descs[zid].state == ZONE_STATE_OFFLINE) {
		status = NVME_SC_ZNS_ERR_OFFLINE; 
	}
	else if (__check_boundary_error(zns_ssd, slba, nr_lba) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY; 
	}

	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest += spp->fw_rd0_lat;
	swr.type = USER_IO;
	swr.cmd = NAND_READ;
	swr.stime = nsecs_latest;
	swr.interleave_pci_dma = false;

	for (lpn = slpn; lpn <= elpn; lpn+=pgs) {
		ppa = __lpn_to_ppa(zns_ssd, lpn);
		pg_off = ppa.g.pg % spp->pgs_per_flashpg;
		pgs = min(elpn - lpn + 1, (__u64)(spp->pgs_per_flashpg - pg_off));
		swr.xfer_size = pgs * spp->pgsz;
		nsecs_completed = ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	} 

	if (swr.interleave_pci_dma == false) {
		nsecs_completed = ssd_advance_pcie(&zns_ssd->ssd, nsecs_latest, nr_lba * spp->secsz);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	}

	ret->status = status;
	ret->nsecs_target = nsecs_latest; 
	return true;
}
#endif