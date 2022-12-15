#include "nvmev.h"
#include "ftl.h"
#include "zns.h"
#include "channel.h"

#if SUPPORT_ZNS 

extern struct buffer global_write_buffer;
void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target, struct buffer * write_buffer, unsigned int buffs_to_release);

inline __u32 __nr_lbas_from_rw_cmd(struct nvme_rw_command * cmd)
{
	return cmd->length + 1;
}

inline __u64 __zone_end_lba(struct zns_ssd *zns_ssd, __u32 zid) 
{
	struct zone_descriptor *zone_descs = &(zns_ssd->zone_descs[zid]);

	return zone_descs->zslba + BYTE_TO_LBA(zone_descs->zone_capacity) -1; 
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
	__u64 zone = lpn_to_zone(zns_ssd, lpn); // find corresponding zone
	__u64 off = lpn - zone_to_slpn(zns_ssd, zone); 
	
	__u32 sdie = (zone * zns_ssd->dies_per_zone) % zns_ssd->ssd.sp.tt_luns;
	__u32 die = sdie + ((off / zns_ssd->ssd.sp.chunks_per_pgm_pg) % zns_ssd->dies_per_zone);

	__u32 channel = die_to_channel(zns_ssd, die);
	__u32 lun = die_to_lun(zns_ssd, die);
	struct ppa ppa = {0};

	ppa.g.lun = lun;
	ppa.g.ch = channel;
	ppa.h.chunk_offs = off % zns_ssd->ssd.sp.chunks_per_pgm_pg;

    return ppa;
}

__u32 __proc_zns_write(struct zns_ssd *zns_ssd, struct nvme_rw_command * cmd)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	__u64 slba = cmd->slba;
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	
	// get zone from start_lbai
	__u32 zid = lba_to_zone(zns_ssd, slba);
	enum zone_state state = zone_descs[zid].state;	

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n",__FUNCTION__, slba, nr_lba, zid, state);
	
	if ((LBA_TO_BYTE(nr_lba) % spp->write_unit_size) != 0)
		return NVME_SC_ZNS_INVALID_WRITE;

	if (__check_boundary_error(zns_ssd, slba, nr_lba) == false) {
		// return boundary error
		return NVME_SC_ZNS_ERR_BOUNDARY;
	}
	
	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s WP error slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d\n",__FUNCTION__, slba, nr_lba, zid,  zns_ssd->zone_descs[zid].wp,  state);
		return NVME_SC_ZNS_INVALID_WRITE;
	}
	
	switch (state) {
		case ZONE_STATE_EMPTY:
		{
			// check if slba == start lba in zone
			if (slba != zone_descs[zid].zslba)
			{
				return NVME_SC_ZNS_INVALID_WRITE;
			}	
			
			if (is_zone_resource_full(zns_ssd, ACTIVE_ZONE))
				return NVME_SC_ZNS_NO_ACTIVE_ZONE;

			if (is_zone_resource_full(zns_ssd, OPEN_ZONE))
				return NVME_SC_ZNS_NO_OPEN_ZONE;

			acquire_zone_resource(zns_ssd, ACTIVE_ZONE);
			// go through
		}
		case ZONE_STATE_CLOSED:
		{
			if (acquire_zone_resource(zns_ssd, OPEN_ZONE) == false)
			{
				return NVME_SC_ZNS_NO_OPEN_ZONE;
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
			return NVME_SC_ZNS_ERR_FULL;
		case ZONE_STATE_READ_ONLY :
			return NVME_SC_ZNS_ERR_READ_ONLY;
		case ZONE_STATE_OFFLINE : 
			return NVME_SC_ZNS_ERR_OFFLINE;
	}
	
	__increase_write_ptr(zns_ssd, zid, nr_lba);

	return NVME_SC_SUCCESS;
}

__u32 __proc_zns_write_zrwa(struct zns_ssd *zns_ssd, struct nvme_request * req, struct nvme_result * ret)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	__u64 slba = cmd->slba;
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	__u64 elba = cmd->slba + nr_lba -1;
		
	// get zone from start_lbai
	__u32 zid = lba_to_zone(zns_ssd, slba);
	enum zone_state state = zone_descs[zid].state;	
	
	__u64 lba, chunks; 
	__u64 cur_wp, prev_wp = zone_descs[zid].wp;
	const __u32 lbas_per_zrwa = zns_ssd->lbas_per_zrwa;
	const __u32 lbas_per_zrwafg = zns_ssd->lbas_per_zrwafg;
	__u64 zrwa_impl_start = prev_wp + lbas_per_zrwa;
	__u64 zrwa_impl_end = prev_wp + (2*lbas_per_zrwa) - 1;
	
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest;
	__u64 nsecs_xfer_completed;
	__u32 status = NVME_SC_SUCCESS;

	struct ppa ppa;
	struct nand_cmd swr;

	NVMEV_DEBUG("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx zrwa_impl_start 0x%llx zrwa_impl_end 0x%llx\n",
														__FUNCTION__, slba, nr_lba, zid, state, prev_wp, zrwa_impl_start, zrwa_impl_end);
	
	if (!buffer_allocate(&zns_ssd->zwra_buffer[zid], LBA_TO_BYTE(nr_lba))) 
		return false;

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
		__u64 nr_lbas_flush = DIV_ROUND_UP((elba - zrwa_impl_start + 1), lbas_per_zrwafg) * lbas_per_zrwafg;
		__increase_write_ptr(zns_ssd, zid, nr_lbas_flush);
		NVMEV_DEBUG("%s implicitly flush wp before 0x%llx after 0x%llx \n",
														__FUNCTION__, prev_wp, zone_descs[zid].wp);
	} else if (elba == __zone_end_lba(zns_ssd, zid)) { 
		// Workaround. move wp to end of the zone and make state full implicitly
		__increase_write_ptr(zns_ssd, zid, elba - prev_wp + 1);
	}

	// get delay from nand model
	nsecs_latest = nsecs_start;
	nsecs_latest += spp->fw_wr0_lat;
	nsecs_latest += spp->fw_wr1_lat * DIV_ROUND_UP(elba - slba + 1, spp->secs_per_chunk);
	nsecs_latest = ssd_advance_pcie(&(zns_ssd->ssd), nsecs_latest, LBA_TO_BYTE(nr_lba));
	nsecs_xfer_completed = nsecs_latest;

	cur_wp = zone_descs[zid].wp;
	
	/* Aggregate write io in flash page */
	for (lba = prev_wp; lba <= cur_wp; lba+=(chunks*spp->secs_per_chunk)) {
		__u64 lpn, end_lpn;
		lpn = lba / spp->secs_per_chunk;
		end_lpn = cur_wp / spp->secs_per_chunk;

		ppa = __lpn_to_ppa(zns_ssd, lpn);
		chunks = min(end_lpn - lpn + 1, (__u64)(spp->chunks_per_pgm_pg - ppa.h.chunk_offs));

		if ((ppa.h.chunk_offs + chunks) == spp->chunks_per_pgm_pg) {	
			swr.type = USER_IO;
			swr.cmd = NAND_WRITE;
			swr.stime = nsecs_xfer_completed;
			swr.xfer_size = spp->chunks_per_pgm_pg * spp->chunksz;

			nsecs_completed = ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);
			nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;

			enqueue_writeback_io_req(req->sq_id, nsecs_completed, &zns_ssd->zwra_buffer[zid], spp->chunks_per_pgm_pg * spp->chunksz);
		} 
	}

out : 
	ret->status = status;
	ret->early_completion = true;
	ret->nsecs_target_early = nsecs_latest;
	ret->nsecs_target = nsecs_latest;

	return true;
}

bool zns_write(struct nvme_request * req, struct nvme_result * ret)
{
	struct zns_ssd *zns_ssd = get_zns_ssd_instance();
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	__u64 lpn, start_lpn = cmd->slba / spp->secs_per_chunk;
	__u64 end_lpn = (cmd->slba + nr_lba - 1) / spp->secs_per_chunk;
	
	// get zone from start_lba
	__u32 zid = lpn_to_zone(zns_ssd, start_lpn);
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest;
	__u64 nsecs_xfer_completed;
	__u64 chunks;
	__u32 status;

	struct ppa ppa;
	struct nand_cmd swr;
	
	NVMEV_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d \n",__FUNCTION__, cmd->slba, nr_lba, zid);
	
	if (zone_descs[zid].zrwav == 0) {
		if (buffer_allocate(&global_write_buffer, LBA_TO_BYTE(nr_lba)) < LBA_TO_BYTE(nr_lba))
			return false;
		
		if (zid != 0)
			status = __proc_zns_write(zns_ssd, cmd);
		else
			status = NVME_SC_SUCCESS;

		// get delay from nand model
		nsecs_latest = nsecs_start;
		nsecs_latest += spp->fw_wr0_lat;
		nsecs_latest += spp->fw_wr1_lat * (end_lpn - start_lpn + 1);
		nsecs_latest = ssd_advance_pcie(&(zns_ssd->ssd), nsecs_latest, LBA_TO_BYTE(nr_lba));
		nsecs_xfer_completed = nsecs_latest;

		for (lpn = start_lpn; lpn <= end_lpn; lpn+=chunks) {
			ppa = __lpn_to_ppa(zns_ssd, lpn);
			chunks = min(end_lpn - lpn + 1, (__u64)(spp->chunks_per_pgm_pg - ppa.h.chunk_offs));

			/* Aggregate write io in flash page */
			if ((ppa.h.chunk_offs + chunks) == spp->chunks_per_pgm_pg) {	
				swr.type = USER_IO;
				swr.cmd = NAND_WRITE;
				swr.stime = nsecs_xfer_completed;
				swr.xfer_size = spp->chunks_per_pgm_pg * spp->chunksz;

				nsecs_completed = ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);
				nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;

				enqueue_writeback_io_req(req->sq_id, nsecs_completed, &global_write_buffer, spp->chunks_per_pgm_pg * spp->chunksz);
			} 
		}

		ret->status = status;
		ret->early_completion = true;
		ret->nsecs_target_early = nsecs_xfer_completed;
		ret->nsecs_target = nsecs_latest;
		
		 
	} else {
		if (!__proc_zns_write_zrwa(zns_ssd, req, ret))
			return false;
	}

	NVMEV_ASSERT(ret->nsecs_target_early <= ret->nsecs_target); 

	return true;
}

bool zns_read(struct nvme_request * req, struct nvme_result * ret)
{
	struct zns_ssd *zns_ssd= get_zns_ssd_instance();
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	__u64 nr_lba = __nr_lbas_from_rw_cmd(cmd);
	__u64 slba = cmd->slba;
	__u64 lpn, start_lpn = slba / spp->secs_per_chunk;
	__u64 end_lpn = (slba + nr_lba - 1) / spp->secs_per_chunk;

	// get zone from start_lba
	__u32 zid = lpn_to_zone(zns_ssd, start_lpn);
	__u32 status = NVME_SC_SUCCESS;
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest = 0;	
	__u64 chunks;
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

	for (lpn = start_lpn; lpn <= end_lpn; lpn+=chunks) {
		ppa = __lpn_to_ppa(zns_ssd, lpn);
		chunks = min(end_lpn - lpn + 1, (__u64)(spp->chunks_per_read_pg - ppa.h.chunk_offs));
		swr.xfer_size = chunks * spp->chunksz;
		nsecs_completed = ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
	} 

	ret->status = status;
	ret->early_completion = false;
	ret->nsecs_target = nsecs_latest; 
	return true;
}
#endif