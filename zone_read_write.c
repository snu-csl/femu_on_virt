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

bool __check_boundary_error(__u64 slba, __u32 nr_lba)
{	
	return LBA_TO_ZONE(slba) == LBA_TO_ZONE(slba + nr_lba - 1);
}

void __increase_write_ptr(__u32 zid, __u32 nr_lba)
{
	__u64 cur_write_ptr = zone_descs[zid].wp;

	cur_write_ptr += nr_lba;
	
	zone_descs[zid].wp = cur_write_ptr;

	if (cur_write_ptr == (ZONE_TO_SLBA(zid) + ZONE_CAPACITY(zid))) {
		//change state to ZSF
		__release_zone_resource(OPEN_ZONE);
		__release_zone_resource(ACTIVE_ZONE);

		if (zone_descs[zid].zrwav)
			ASSERT(0);

		__change_zone_state(zid, ZONE_STATE_FULL);
	}
	else if (cur_write_ptr > (ZONE_TO_SLBA(zid) +  ZONE_CAPACITY(zid))) {
		NVMEV_ERROR("[%s] Write Boundary error!!\n", __FUNCTION__);
	}
}

static inline struct ppa __lba_to_ppa(uint64_t lba) 
{
	__u64 zone = LBA_TO_ZONE(lba); // find corresponding zone
	__u64 off = LBA_TO_BYTE(lba - ZONE_TO_SLBA(zone)); 
	
	__u32 die = ZONE_TO_DIE(zone, off);
	__u32 channel = DIE_TO_CHANNEL(die);
	__u32 lun = DIE_TO_LUN(die);
	struct ppa ppa = {0};

	ppa.g.lun = lun;
	ppa.g.ch = channel;

    return ppa;
}

static inline  __u64 __get_firmware_read_latency(void)
{
	return ssd.sp.fw_rd_lat;
}

static inline  __u64 __get_firmware_write_latency(void)
{
	return ssd.sp.fw_wr_lat;
}

static inline  __u64 __get_firmware_transfer_latency(__u64 size)
{
	return ssd.sp.fw_xfer_lat * DIV_ROUND_UP(size, UNIT_XFER_SIZE);
}

__u32 __proc_nvme_write(struct nvme_rw_command * cmd)
{
	__u64 slba = cmd->slba;
	__u64 nr_lba = __get_nr_lba_from_rw_cmd(cmd);
	
	// get zone from start_lbai
	__u32 zid = LBA_TO_ZONE(slba);
	enum zone_state state = zone_descs[zid].state;	

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n",__FUNCTION__, slba, nr_lba, zid, state);
	
	if ((LBA_TO_BYTE(nr_lba) % WRITE_UNIT_SIZE) != 0)
		return NVME_SC_ZNS_INVALID_WRITE;

	if (__check_boundary_error(slba, nr_lba) == false) {
		// return boundary error
		return NVME_SC_ZNS_ERR_BOUNDARY;
	}
	
	// check if slba == current write pointer
	if (slba != zone_descs[zid].wp) {
		NVMEV_ERROR("%s slba 0x%llx nr_lba 0x%llx zone_id %d wp %llx state %d\n",__FUNCTION__, slba, nr_lba, zid,  zone_descs[zid].wp,  state);
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
			
			if (IS_ZONE_RESOURCE_FULL(ACTIVE_ZONE))
				return NVME_SC_ZNS_NO_ACTIVE_ZONE;

			if (IS_ZONE_RESOURCE_FULL(OPEN_ZONE))
				return NVME_SC_ZNS_NO_OPEN_ZONE;

			__acquire_zone_resource(ACTIVE_ZONE);
			// go through
		}
		case ZONE_STATE_CLOSED:
		{
			if (__acquire_zone_resource(OPEN_ZONE) == false)
			{
				return NVME_SC_ZNS_NO_OPEN_ZONE;
			}
			
			// change to ZSIO
			__change_zone_state(zid, ZONE_STATE_OPENED_IMPL);
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
	
	__increase_write_ptr(zid, nr_lba);

	return NVME_SC_SUCCESS;
}

__u32 __proc_nvme_write_zrwa(struct nvme_rw_command * cmd)
{
	__u64 slba = cmd->slba;
	__u64 nr_lba = __get_nr_lba_from_rw_cmd(cmd);
	__u64 elba = cmd->slba + nr_lba -1;

	// get zone from start_lbai
	__u32 zid = LBA_TO_ZONE(slba);
	enum zone_state state = zone_descs[zid].state;	
	
	__u64 wp = zone_descs[zid].wp;
	__u64 zrwa_impl_start = wp + LBAS_PER_ZRWA;
	__u64 zrwa_impl_end = wp + (2*LBAS_PER_ZRWA) - 1;
	
	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx zrwa_impl_start 0x%llx zrwa_impl_end 0x%llx\n",
														__FUNCTION__, slba, nr_lba, zid, state, wp, zrwa_impl_start, zrwa_impl_end);
	
	if ((LBA_TO_BYTE(nr_lba) % WRITE_UNIT_SIZE) != 0)
		return NVME_SC_ZNS_INVALID_WRITE;

	if (__check_boundary_error(slba, nr_lba) == false) {
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
			if (__acquire_zone_resource(OPEN_ZONE) == false)
			{
				return NVME_SC_ZNS_NO_OPEN_ZONE;
			}
			
			// change to ZSIO
			__change_zone_state(zid, ZONE_STATE_OPENED_IMPL);
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
		__increase_write_ptr(zid, nr_lbas_flush);
		NVMEV_ZNS_DEBUG("%s implicitly flush wp before 0x%llx after 0x%llx \n",
														__FUNCTION__, wp, zone_descs[zid].wp);
	}
	return NVME_SC_SUCCESS;
}

void zns_write(struct nvme_request * req, struct nvme_result * ret)
{
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	__u32 status;
	__u64 lba, slba = cmd->slba;
	__u64 lbas_to_write = __get_nr_lba_from_rw_cmd(cmd);
	__u64 bytes_to_write = LBA_TO_BYTE(lbas_to_write);
	__u64 remaining = bytes_to_write;
	// get zone from start_lba
	__u32 zid = LBA_TO_ZONE(slba);
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest = 0;
	struct ppa ppa;
	struct nand_cmd swr;
	
	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d\n",__FUNCTION__, slba, lbas_to_write, zid);
	
	if (zone_descs[zid].zrwav == 0) {
		status = __proc_nvme_write(cmd);

		// get delay from nand model
		swr.type = USER_IO;
		swr.cmd = NAND_WRITE;
		swr.stime = nsecs_start + 
					__get_firmware_write_latency() + 
					__get_firmware_transfer_latency(bytes_to_write);
		lba = slba;
		
		while (remaining) {
			swr.xfer_size = min(remaining, PGM_PAGE_SIZE);
			ppa = __lba_to_ppa(lba); 
			nsecs_completed = ssd_advance_status(&ssd, &ppa, &swr);
			nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
			remaining -= swr.xfer_size;
			lba += lbas_to_write;
		} 
		
		// get delay from pcie model 
		nsecs_latest = ssd_advance_pcie(&ssd, nsecs_latest, bytes_to_write);
	}
	else {
		status = __proc_nvme_write_zrwa(cmd);
		nsecs_latest = nsecs_start; // TODO : it will make perf model for zrwa 
	}

	ret->status = status;
	ret->nsecs_target = nsecs_latest;
	return;
}

void zns_read(struct nvme_request * req, struct nvme_result * ret)
{
	struct nvme_rw_command * cmd = &(req->cmd->rw);
	__u64 lba, slba = cmd->slba;
	__u64 lbas_to_read = __get_nr_lba_from_rw_cmd(cmd);
	__u64 bytes_to_read = LBA_TO_BYTE(lbas_to_read);
	__u64 remaining = bytes_to_read;
		
	// get zone from start_lba
	__u32 zid = LBA_TO_ZONE(slba);
	__u32 status = NVME_SC_SUCCESS;
	__u64 nsecs_start = req->nsecs_start;
	__u64 nsecs_completed = 0, nsecs_latest = 0;
	enum zone_state state = zone_descs[zid].state;	
	struct ppa ppa;
	struct nand_cmd swr;

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d wp 0x%llx last lba 0x%llx %d\n",
	__FUNCTION__, slba, lbas_to_read, zid, state,  zone_descs[zid].wp,  (slba + lbas_to_read - 1), LBAS_PER_ZONE);

	if (state == ZONE_STATE_OFFLINE) {
		status = NVME_SC_ZNS_ERR_OFFLINE; 
	}
	else if (__check_boundary_error(slba, lbas_to_read) == false) {
		// return boundary error
		status = NVME_SC_ZNS_ERR_BOUNDARY; 
	}

	// get delay from nand model
	swr.type = USER_IO;
	swr.cmd = NAND_READ;
	swr.stime = nsecs_start + __get_firmware_read_latency();
				
	lba = slba;

	while (remaining)
	{
		swr.xfer_size = min(remaining, READ_PAGE_SIZE);
		ppa = __lba_to_ppa(lba); 
		nsecs_completed = ssd_advance_status(&ssd, &ppa, &swr);
		nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
		remaining -= swr.xfer_size;
		lba += lbas_to_read;
	} 

	// get delay from pcie model 
	nsecs_latest = ssd_advance_pcie(&ssd, nsecs_latest, bytes_to_read);
	
	ret->status = status;
	ret->nsecs_target = nsecs_latest; 
	return;
}
#endif