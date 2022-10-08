#include "nvmev.h"
#include "ftl.h"
#include "zns.h"
#include "channel.h"

#if SUPPORT_ZNS 

#define WRITE_UNIT_SIZE 512

inline __u32 __get_nr_lba_from_rw_cmd(struct nvme_rw_command * cmd)
{
	return cmd->length + 1;
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

static inline struct ppa __lba_to_ppa(struct zns_ssd *zns_ssd, uint64_t lba) 
{
	__u64 zone = lba_to_zone(zns_ssd, lba); // find corresponding zone
	__u64 off = LBA_TO_LPN(lba - zone_to_slba(zns_ssd, zone)); 
	
	__u32 sdie = (zone * zns_ssd->dies_per_zone) % zns_ssd->ssd.sp.tt_luns;
	__u32 die = sdie + ((off / zns_ssd->ssd.sp.chunks_per_pgm_pg) % zns_ssd->dies_per_zone);

	__u32 channel = die_to_channel(zns_ssd, die);
	__u32 lun = die_to_lun(zns_ssd, die);
	struct ppa ppa = {0};

	ppa.g.lun = lun;
	ppa.g.ch = channel;

    return ppa;
}

__u32 __proc_zns_write(struct zns_ssd *zns_ssd, struct nvme_rw_command * cmd)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	__u64 slba = cmd->slba;
	__u64 nr_lba = __get_nr_lba_from_rw_cmd(cmd);
	
	// get zone from start_lbai
	__u32 zid = lba_to_zone(zns_ssd, slba);
	enum zone_state state = zone_descs[zid].state;	

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n",__FUNCTION__, slba, nr_lba, zid, state);
	
	if ((LBA_TO_BYTE(nr_lba) % WRITE_UNIT_SIZE) != 0)
		return NVME_SC_ZNS_INVALID_WRITE;

	if (__check_boundary_error(zns_ssd, slba, nr_lba) == false) {
		// return boundary error
		return NVME_SC_ZNS_ERR_BOUNDARY;
	}
	
	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d\n",__FUNCTION__, slba, nr_lba, zid,  zns_ssd->zone_descs[zid].wp,  state);
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

__u32 __proc_zns_write_zrwa(struct zns_ssd *zns_ssd, struct nvme_rw_command * cmd)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	__u64 slba = cmd->slba;
	__u64 nr_lba = __get_nr_lba_from_rw_cmd(cmd);
	__u64 elba = cmd->slba + nr_lba -1;

	// get zone from start_lbai
	__u32 zid = lba_to_zone(zns_ssd, slba);
	enum zone_state state = zone_descs[zid].state;	
	
	__u64 wp = zone_descs[zid].wp;
	__u64 zrwa_impl_start = wp + LBAS_PER_ZRWA;
	__u64 zrwa_impl_end = wp + (2*LBAS_PER_ZRWA) - 1;
	
	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx zrwa_impl_start 0x%llx zrwa_impl_end 0x%llx\n",
														__FUNCTION__, slba, nr_lba, zid, state, wp, zrwa_impl_start, zrwa_impl_end);
	
	if ((LBA_TO_BYTE(nr_lba) % WRITE_UNIT_SIZE) != 0)
		return NVME_SC_ZNS_INVALID_WRITE;

	if (__check_boundary_error(zns_ssd, slba, nr_lba) == false) {
		// return boundary error
		return NVME_SC_ZNS_ERR_BOUNDARY;
	}
	
	// valid range : wp <=  <= wp + 2*(size of zwra) -1
	if (slba < zone_descs[zid].wp || elba > zrwa_impl_end) {
		NVMEV_ERROR("%s slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d\n",__FUNCTION__, slba, nr_lba, zid,  zone_descs[zid].wp,  state);
		return NVME_SC_ZNS_INVALID_WRITE;
	}
	
	switch (state) {
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
		case ZONE_STATE_EMPTY : 
			return NVME_SC_ZNS_INVALID_ZONE_OPERATION;
	}
	
	if (elba >= zrwa_impl_start) {	
		__u64 nr_lbas_flush = DIV_ROUND_UP((elba - zrwa_impl_start + 1), LBAS_PER_ZRWAFG) * LBAS_PER_ZRWAFG;
		__increase_write_ptr(zns_ssd, zid, nr_lbas_flush);
		NVMEV_ZNS_DEBUG("%s implicitly flush wp before 0x%llx after 0x%llx \n",
														__FUNCTION__, wp, zone_descs[zid].wp);
	}
	return NVME_SC_SUCCESS;
}

void enqueue_writeback_io_req(int sqid, unsigned long long nsecs_target, unsigned int buffs_to_release);
bool zns_write(struct nvme_request * req, struct nvme_result * ret)
{
	struct zns_ssd *zns_ssd = get_zns_ssd_instance();
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	__u32 status;
	__u64 lba, slba = cmd->slba;
	__u64 lbas_to_write = __get_nr_lba_from_rw_cmd(cmd);
	__u64 bytes_to_write = LBA_TO_BYTE(lbas_to_write);
	__u64 remaining = bytes_to_write;
	// get zone from start_lba
	
	__u32 zid = lba_to_zone(zns_ssd, slba);
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest = 0;
	__u64 buffers_to_write = DIV_ROUND_UP(bytes_to_write, WRITE_BUFFER_SIZE);
	struct ppa ppa;
	struct nand_cmd swr;
	
	NVMEV_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d\n",__FUNCTION__, slba, lbas_to_write, zid);
	
	if (zone_descs[zid].zrwav == 0) {
#if 0
		if (allocate_write_buffer(buffers_to_write) < buffers_to_write)
			return false;
#endif
		status = __proc_zns_write(zns_ssd, cmd);

		// get delay from nand model
		swr.type = USER_IO;
		swr.cmd = NAND_WRITE;
		swr.stime = nsecs_start;
		swr.stime += spp->fw_wr0_lat;
		swr.stime += spp->fw_wr1_lat * buffers_to_write;
		swr.stime = ssd_advance_pcie(&(zns_ssd->ssd), swr.stime, bytes_to_write);
		lba = slba;
		
		while (remaining) {
			ppa = __lba_to_ppa(zns_ssd, lba); 
			swr.xfer_size = min(remaining, spp->chunks_per_pgm_pg * spp->chunksz);
			nsecs_completed = ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);
			nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
			remaining -= swr.xfer_size;
			lba += BYTE_TO_LBA(swr.xfer_size);
		} 
	}
	else {
		status = __proc_zns_write_zrwa(zns_ssd, cmd);
		nsecs_latest = nsecs_start; // TODO : it will make perf model for zrwa 
	}

	ret->status = status;
	ret->early_completion = false;
	ret->nsecs_target_early = swr.stime;
	ret->nsecs_target = nsecs_latest;
	
	NVMEV_ASSERT(ret->nsecs_target_early <= ret->nsecs_target); 
	return true;
}

bool zns_read(struct nvme_request * req, struct nvme_result * ret)
{
	struct zns_ssd *zns_ssd= get_zns_ssd_instance();
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	__u64 lba, slba = cmd->slba;
	__u64 lbas_to_read = __get_nr_lba_from_rw_cmd(cmd);
	__u64 bytes_to_read = LBA_TO_BYTE(lbas_to_read);
	__u64 remaining = bytes_to_read;
		
	// get zone from start_lba
	__u32 zid = lba_to_zone(zns_ssd, slba);
	__u32 status = NVME_SC_SUCCESS;
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest = 0;	
	struct ppa ppa;
	struct nand_cmd swr;

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d wp 0x%llx last lba 0x%llx\n",
	__FUNCTION__, slba, lbas_to_read, zid, zone_descs[zid].state,  zone_descs[zid].wp,  (slba + lbas_to_read - 1));

	if (zone_descs[zid].state == ZONE_STATE_OFFLINE) {
		status = NVME_SC_ZNS_ERR_OFFLINE; 
	}
	else if (__check_boundary_error(zns_ssd, slba, lbas_to_read) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY; 
	}

	// get delay from nand model
	swr.type = USER_IO;
	swr.cmd = NAND_READ;
	swr.stime = nsecs_start;
	swr.stime += spp->fw_rd0_lat;
				
	lba = slba;

	while (remaining)
	{
		swr.xfer_size = min(remaining, spp->chunks_per_read_pg * spp->chunksz);
		ppa = __lba_to_ppa(zns_ssd, lba); 
		nsecs_completed = ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
		remaining -= swr.xfer_size;
		lba += BYTE_TO_LBA(swr.xfer_size);
	} 

	// get delay from pcie model 
	nsecs_latest = ssd_advance_pcie(&zns_ssd->ssd, nsecs_latest, bytes_to_read);

	ret->status = status;
	ret->nsecs_target = nsecs_latest; 
	return true;
}
#endif