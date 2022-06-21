#include "zns.h"

#if SUPPORT_ZNS 

__u32 __mgmt_send_close_zone(__u64 zid)
{
	enum zone_state cur_state = zone_descs[zid].state;	
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{	
			__change_zone_state(zid, ZONE_STATE_CLOSED);

			__release_zone_resource(OPEN_ZONE);
			break;
		}
		
		case ZONE_STATE_CLOSED:
		{
			break;
		}
		default:
		{
			status = NVME_SC_ZNS_INVALID_TRANSITION;
			break;
		}
	}

	return status;
}

__u32 __mgmt_send_finish_zone(__u64 zid, __u32 select_all)
{
	enum zone_state cur_state = zone_descs[zid].state;
	bool is_zrwa_zone = zone_descs[zid].zrwav;		
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{			
			__release_zone_resource(OPEN_ZONE);
			//go through
		}
		case ZONE_STATE_CLOSED:
		{
			__release_zone_resource(ACTIVE_ZONE);

			if (is_zrwa_zone)
				__release_zone_resource(ZRWA_ZONE);

			__change_zone_state(zid, ZONE_STATE_FULL);
			break;
		}
		
		case ZONE_STATE_EMPTY:
		{
			if (select_all)
				status = NVME_SC_ZNS_INVALID_TRANSITION;
			else
				__change_zone_state(zid, ZONE_STATE_FULL);
			break;		
		}
		case ZONE_STATE_FULL:
			break;

		default:
		{
			status = NVME_SC_ZNS_INVALID_TRANSITION;
			break;
		}
	}


	return status;
}

__u32 __mgmt_send_open_zone(__u64 zid, __u32 select_all, __u32 zrwa)
{
	enum zone_state cur_state = zone_descs[zid].state;		
	__u32 status = NVME_SC_SUCCESS;

	if (select_all && cur_state != ZONE_STATE_CLOSED)
		return NVME_SC_ZNS_INVALID_TRANSITION;

	switch(cur_state) {

		case ZONE_STATE_EMPTY:
		{
			if (IS_ZONE_RESOURCE_FULL(ACTIVE_ZONE))
				return NVME_SC_ZNS_NO_ACTIVE_ZONE;

			if (IS_ZONE_RESOURCE_FULL(OPEN_ZONE))
				return NVME_SC_ZNS_NO_OPEN_ZONE;
			
			if (zrwa)
			{
				if (IS_ZONE_RESOURCE_FULL(ZRWA_ZONE))
					return NVME_SC_ZNS_ZRWA_RSRC_UNAVAIL;

				__acquire_zone_resource(ZRWA_ZONE);
				zone_descs[zid].zrwav = 1;
			}

			__acquire_zone_resource(ACTIVE_ZONE);
			// go through
		}

		case ZONE_STATE_CLOSED:
		{
			if (__acquire_zone_resource(OPEN_ZONE) == false)
				return NVME_SC_ZNS_NO_OPEN_ZONE;

 			//go through
		}
		case ZONE_STATE_OPENED_IMPL:
		{
			__change_zone_state(zid, ZONE_STATE_OPENED_EXPL);
			break;
		}
		
		case ZONE_STATE_OPENED_EXPL:
			break;		
		
		default:
		{
			status = NVME_SC_ZNS_INVALID_TRANSITION;
			break;
		}
	}

	return status;
}

void __reset_zone(__u64 zid)
{
	__u32 zone_size = BYTES_PER_ZONE;

	__u8 * zone_start_addr = (__u8 *)__get_zns_media_addr_from_zid(zid);
	
	NVMEV_ZNS_DEBUG("%s zid %lu start addres 0x%llx zone_size %x \n", 
			__FUNCTION__, zid,  (__u64)zone_start_addr, zone_size);

	memset(zone_start_addr, 0, zone_size);

	zone_descs[zid].wp = zone_descs[zid].zslba;
	zone_descs[zid].zrwav = 0;
}

__u32 __mgmt_send_reset_zone(__u64 zid)
{
	enum zone_state cur_state = zone_descs[zid].state;
	bool is_zrwa_zone = zone_descs[zid].zrwav;	
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{
			__release_zone_resource(OPEN_ZONE);
			// go through
		}
		case ZONE_STATE_CLOSED:
		{
			__release_zone_resource(ACTIVE_ZONE);

			if (is_zrwa_zone)
				__release_zone_resource(ZRWA_ZONE);
			// go through
		}
		case ZONE_STATE_FULL:
		case ZONE_STATE_EMPTY:
		{
			__change_zone_state(zid, ZONE_STATE_EMPTY);
			__reset_zone(zid);
			break;
		}
		
		default:
		{
			status = NVME_SC_ZNS_INVALID_TRANSITION;
			break;
		}
	}

	return status;
}

__u32 __mgmt_send_offline_zone(__u64 zid)
{

	enum zone_state cur_state = zone_descs[zid].state;	
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		case ZONE_STATE_READ_ONLY:
		{
			__change_zone_state(zid, ZONE_STATE_OFFLINE);
			break;
		}
		
		case ZONE_STATE_OFFLINE:
			break;		
		
		default:
		{
			status = NVME_SC_ZNS_INVALID_TRANSITION;
			break;
		}
	}

	return status;
}

__u32 __mgmt_send_flush_explicit_zrwa(__u64 slba)
{
	__u64 zid = LBA_TO_ZONE(slba);
	__u64 wp = zone_descs[zid].wp;
	__u32 status = NVME_SC_SUCCESS;
	enum zone_state cur_state = zone_descs[zid].state;	
	
	__u64 zrwa_start = wp;
	__u64 zrwa_end = min(zrwa_start + LBAS_PER_ZRWA - 1, (size_t)ZONE_TO_SLBA(zid) + ZONE_CAPACITY(zid) - 1); 
	__u64 nr_lbas_flush = slba - wp + 1;

	NVMEV_ZNS_DEBUG("%s slba 0x%llx zrwa_start 0x%llx zrwa_end 0x%llx zone_descs[zid].zrwav %d\n", 
			__FUNCTION__, slba, zrwa_start, zrwa_end, zone_descs[zid].zrwav);

	if (zone_descs[zid].zrwav == 0)
		return NVME_SC_ZNS_INVALID_ZONE_OPERATION;

	if (!(slba >= zrwa_start && slba <= zrwa_end))
		return NVME_SC_ZNS_INVALID_ZONE_OPERATION; 

	if ((nr_lbas_flush % LBAS_PER_ZRWAFG) != 0)
		return NVME_SC_INVALID_FIELD;

	switch(cur_state) {
		case ZONE_STATE_OPENED_EXPL:
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_CLOSED:
		{	
		
			zone_descs[zid].wp = slba + 1;

			if (zone_descs[zid].wp == (ZONE_TO_SLBA(zid) + ZONE_CAPACITY(zid))) {
				//change state to ZSF
				if (cur_state != ZONE_STATE_CLOSED)
					__release_zone_resource(OPEN_ZONE);
				__release_zone_resource(ACTIVE_ZONE);
				__release_zone_resource(ZRWA_ZONE);
				__change_zone_state(zid, ZONE_STATE_FULL);
			}
			break;
		}
		default:
		{
			status = NVME_SC_ZNS_INVALID_ZONE_OPERATION;
			break;
		}
	}

	return status;
}

__u32 __process_mgmt_send(__u64 slba, __u32 action, __u32 select_all, __u32 option)
{	
	__u32 status;
	__u64 zid = LBA_TO_ZONE(slba);

	switch(action) {
		case ZSA_CLOSE_ZONE:
		{
			status = __mgmt_send_close_zone(zid);
			break;
		}
		case ZSA_FINISH_ZONE:
		{
			status = __mgmt_send_finish_zone(zid, select_all);
			break;
		}
		case ZSA_OPEN_ZONE:
		{
			status = __mgmt_send_open_zone(zid, select_all, option);
			break;
		}
		case ZSA_RESET_ZONE:
		{
			status = __mgmt_send_reset_zone(zid);
			break;
		}
		case ZSA_OFFLINE_ZONE:
		{
			status = __mgmt_send_offline_zone(zid);
			break;
		}
		case ZSA_FLUSH_EXPL_ZRWA:
		{
			status = __mgmt_send_flush_explicit_zrwa(slba);
			break;
		}
	}

	return status;
}

__u32 zns_proc_mgmt_send(struct nvme_zone_mgmt_send * cmd)
{
	__u32 select_all = cmd->select_all;
	__u32 status = NVME_SC_SUCCESS;
	
	__u32 action = cmd->zsa;
	__u32 option = cmd->zsaso;
	__u64 slba = cmd->slba;
	__u64 zid = LBA_TO_ZONE(slba);
	if (select_all) {
		for (zid = 0; zid < NR_MAX_ZONE; zid++)
			__process_mgmt_send(ZONE_TO_SLBA(zid), action, true, option);

	}
	else {
		status = __process_mgmt_send(slba, action, false, option);
	}
	
	NVMEV_ZNS_DEBUG("%s slba %llx zid %lu select_all %lu action %u status %lu option %lu\n", 
			__FUNCTION__, cmd->slba, zid,  select_all, cmd->zsa, status, option);
	
	return status;
}
#endif