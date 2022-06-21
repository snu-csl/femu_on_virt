#include "zns.h"

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

__u32 __proc_nvme_write(struct nvme_rw_command * cmd)
{
	__u64 slba = cmd->slba;
	__u64 nr_lba = __get_nr_lba_from_rw_cmd(cmd);
	__u64 prp1 = cmd->prp1;
	__u64 prp2 = cmd->prp2;
	void * raw_storage = __get_zns_media_addr_from_lba(slba);	

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
	
	__prp_transfer_data(prp1, prp2, raw_storage, LBA_TO_BYTE(nr_lba), 1 /*prp->buffer*/); 

	__increase_write_ptr(zid, nr_lba);

	return NVME_SC_SUCCESS;
}

__u32 __proc_nvme_write_zrwa(struct nvme_rw_command * cmd)
{
	__u64 slba = cmd->slba;
	__u64 nr_lba = __get_nr_lba_from_rw_cmd(cmd);
	__u64 elba = cmd->slba + nr_lba -1;
	__u64 prp1 = cmd->prp1;
	__u64 prp2 = cmd->prp2;
	void * raw_storage = __get_zns_media_addr_from_lba(slba);	

	// get zone from start_lbai
	__u32 zid = LBA_TO_ZONE(slba);
	enum zone_state state = zone_descs[zid].state;	
	
	__u64 wp = zone_descs[zid].wp;
	__u64 zrwa_impl_start = wp + LBAS_PER_ZRWA;
	__u64 zrwa_impl_end = wp + (2*LBAS_PER_ZRWA) - 1;
	
	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%llx zone_id %d state %d wp 0x%llx zrwa_impl_start 0x%llx zrwa_impl_end 0x%llx\n",
														__FUNCTION__, slba, nr_lba, zid, state, wp, zrwa_impl_start, zrwa_impl_end);
	
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
	
	__prp_transfer_data(prp1, prp2, raw_storage, LBA_TO_BYTE(nr_lba), 1 /*prp->buffer*/); 

	if (elba >= zrwa_impl_start) {	
		__u64 nr_lbas_flush = DIV_ROUND_UP((elba - zrwa_impl_start + 1), LBAS_PER_ZRWAFG) * LBAS_PER_ZRWAFG;
		__increase_write_ptr(zid, nr_lbas_flush);
		NVMEV_ZNS_DEBUG("%s implicitly flush wp before 0x%llx after 0x%llx \n",
														__FUNCTION__, wp, zone_descs[zid].wp);
	}
	return NVME_SC_SUCCESS;
}

__u32 zns_proc_nvme_write(struct nvme_rw_command * cmd)
{
	__u32 status;
	__u64 slba = cmd->slba;
	__u64 nr_lba = __get_nr_lba_from_rw_cmd(cmd);
	// get zone from start_lbai
	__u32 zid = LBA_TO_ZONE(slba);
	
	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d\n",__FUNCTION__, slba, nr_lba, zid);
	
	if ((LBA_TO_BYTE(nr_lba) % WRITE_UNIT_SIZE) != 0)
		return NVME_SC_ZNS_INVALID_WRITE;

	if (__check_boundary_error(slba, nr_lba) == false) {
		// return boundary error
		return NVME_SC_ZNS_ERR_BOUNDARY;
	}
	
	if (zone_descs[zid].zrwav == 0)
		status = __proc_nvme_write(cmd);
	else 
		status = __proc_nvme_write_zrwa(cmd);

	return status;
}

__u32 zns_proc_append(struct nvme_zone_append * cmd, __u64 * wp)
{
	__u32 zid = LBA_TO_ZONE(cmd->slba);
	__u64 slba = zone_descs[zid].wp; 

	__u64 nr_lba = cmd->nr_lba + 1;

	__u64 prp1 = cmd->prp1;
	__u64 prp2 = cmd->prp2;

	enum zone_state state = zone_descs[zid].state;	

	void * raw_storage = __get_zns_media_addr_from_lba(slba);
	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d\n",__FUNCTION__, slba, nr_lba, zid, state);

	if (IS_ZONE_SEQUENTIAL(zid) == false) {
		return NVME_SC_INVALID_FIELD;
	}

	if (__check_boundary_error(slba, nr_lba) == false) {
		// return boundary error
		return NVME_SC_ZNS_ERR_BOUNDARY;
	}
	
	switch (state) {
		case ZONE_STATE_EMPTY:
		{		
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
	
	__prp_transfer_data(prp1, prp2, raw_storage, LBA_TO_BYTE(nr_lba), 1 /*prp->buffer*/); 

	__increase_write_ptr(zid, nr_lba);

	return NVME_SC_SUCCESS;
}

__u32 zns_proc_nvme_read(struct nvme_rw_command * cmd)
{
	__u64 slba = cmd->slba;
	__u64 nr_lba =  __get_nr_lba_from_rw_cmd(cmd);
	
	__u64 prp1 = cmd->prp1;
	__u64 prp2 = cmd->prp2;
	void * raw_storage = __get_zns_media_addr_from_lba(slba);		

	// get zone from start_lba
	__u32 zid = LBA_TO_ZONE(slba);
	enum zone_state state = zone_descs[zid].state;	

	NVMEV_ZNS_DEBUG("%s slba 0x%llx nr_lba 0x%lx zone_id %d state %d wp 0x%llx last lba 0x%llx %d\n",__FUNCTION__, slba, nr_lba, zid, state,  zone_descs[zid].wp,  (slba + nr_lba - 1), LBAS_PER_ZONE);

	switch(state) {
		case ZONE_STATE_OFFLINE:
			return NVME_SC_ZNS_ERR_OFFLINE;
		default :
			break;
	}

	if (__check_boundary_error(slba, nr_lba) == false) {
		// return boundary error
		return NVME_SC_ZNS_ERR_BOUNDARY;
	}

	__prp_transfer_data(prp1, prp2, raw_storage, LBA_TO_BYTE(nr_lba), 0 /*buffer->prp*/); 
	
	return NVME_SC_SUCCESS;
}
#endif