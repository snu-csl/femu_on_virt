#include "nvmev.h"
#include "ftl.h"
#include "zns.h"

#if SUPPORT_ZNS 
__u32 __proc_zmgmt_send_close_zone(struct zns_ssd *zns_ssd, __u64 zid)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;	
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{	
			change_zone_state(zns_ssd, zid, ZONE_STATE_CLOSED);

			release_zone_resource(zns_ssd, OPEN_ZONE);
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

__u32 __proc_zmgmt_send_finish_zone(struct zns_ssd *zns_ssd, __u64 zid, __u32 select_all)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;
	bool is_zrwa_zone = zone_descs[zid].zrwav;		
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{			
			release_zone_resource(zns_ssd, OPEN_ZONE);
			//go through
		}
		case ZONE_STATE_CLOSED:
		{
			release_zone_resource(zns_ssd, ACTIVE_ZONE);

			if (is_zrwa_zone)
				release_zone_resource(zns_ssd, ZRWA_ZONE);

			change_zone_state(zns_ssd, zid, ZONE_STATE_FULL);
			break;
		}
		
		case ZONE_STATE_EMPTY:
		{
			if (select_all)
				status = NVME_SC_ZNS_INVALID_TRANSITION;
			else
				change_zone_state(zns_ssd, zid, ZONE_STATE_FULL);
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

__u32 __zmgmt_send_open_zone(struct zns_ssd *zns_ssd, __u64 zid, __u32 select_all, __u32 zrwa)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;		
	__u32 status = NVME_SC_SUCCESS;

	if (select_all && cur_state != ZONE_STATE_CLOSED)
		return NVME_SC_ZNS_INVALID_TRANSITION;

	switch(cur_state) {

		case ZONE_STATE_EMPTY:
		{
			if (is_zone_resource_full(zns_ssd, ACTIVE_ZONE))
				return NVME_SC_ZNS_NO_ACTIVE_ZONE;

			if (is_zone_resource_full(zns_ssd, OPEN_ZONE))
				return NVME_SC_ZNS_NO_OPEN_ZONE;
			
			if (zrwa)
			{
				if (is_zone_resource_full(zns_ssd, ZRWA_ZONE))
					return NVME_SC_ZNS_ZRWA_RSRC_UNAVAIL;

				acquire_zone_resource(zns_ssd, ZRWA_ZONE);
				zone_descs[zid].zrwav = 1;
			}

			acquire_zone_resource(zns_ssd, ACTIVE_ZONE);
			// go through
		}

		case ZONE_STATE_CLOSED:
		{
			if (acquire_zone_resource(zns_ssd, OPEN_ZONE) == false)
				return NVME_SC_ZNS_NO_OPEN_ZONE;

 			//go through
		}
		case ZONE_STATE_OPENED_IMPL:
		{
			change_zone_state(zns_ssd, zid, ZONE_STATE_OPENED_EXPL);
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

void __reset_zone(struct zns_ssd * zns_ssd, __u64 zid)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	__u32 zone_size = zns_ssd->zone_size;
	__u8 * zone_start_addr = (__u8 *)get_storage_addr_from_zid(zns_ssd, zid);
	
	NVMEV_ZNS_DEBUG("%s ns %d zid %lu  0x%llx, start addres 0x%llx zone_size %x \n", 
			__FUNCTION__, zns_ssd->ns, zid, (void*)vdev->ns_mapped[zns_ssd->ns], (__u64)zone_start_addr, zone_size);

	//memset(zone_start_addr, 0, zone_size);

	zone_descs[zid].wp = zone_descs[zid].zslba;
	zone_descs[zid].zrwav = 0;
/*
	zns_ssd->zone_descs_durable[zid].wp = zone_descs[zid].zslba;
	memset(zns_ssd->wl_state[zid], 0, sizeof(__u16)*zns_ssd->wl_per_zone);
	zns_ssd->zone_descs_durable[zid].zrwav = 0;
*/
	zns_reset_desc_durable(zid);

	buffer_refill(&zns_ssd->zwra_buffer[zid]);
}

__u32 __proc_zmgmt_send_reset_zone(struct zns_ssd *zns_ssd, __u64 zid)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	enum zone_state cur_state = zone_descs[zid].state;
	bool is_zrwa_zone = zone_descs[zid].zrwav;	
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_OPENED_EXPL:
		{
			release_zone_resource(zns_ssd, OPEN_ZONE);
			// go through
		}
		case ZONE_STATE_CLOSED:
		{
			release_zone_resource(zns_ssd, ACTIVE_ZONE);

			if (is_zrwa_zone)
				release_zone_resource(zns_ssd, ZRWA_ZONE);
			// go through
		}
		case ZONE_STATE_FULL:
		case ZONE_STATE_EMPTY:
		{
			change_zone_state(zns_ssd, zid, ZONE_STATE_EMPTY);
			__reset_zone(zns_ssd, zid);
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

__u32 __proc_zmgmt_send_offline_zone(struct zns_ssd *zns_ssd, __u64 zid)
{
	enum zone_state cur_state = zns_ssd->zone_descs[zid].state;	
	__u32 status = NVME_SC_SUCCESS;

	switch(cur_state) {
		case ZONE_STATE_READ_ONLY:
		{
			change_zone_state(zns_ssd, zid, ZONE_STATE_OFFLINE);
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

__u32 __proc_zmgmt_send_flush_explicit_zrwa(struct zns_ssd *zns_ssd, __u64 slba)
{
	struct zone_descriptor *zone_descs = zns_ssd->zone_descs;
	__u64 zid = lba_to_zone(zns_ssd, slba);
	__u64 wp = zone_descs[zid].wp;
	__u32 status = NVME_SC_SUCCESS;
	enum zone_state cur_state = zone_descs[zid].state;	
	__u64 zone_capacity = zone_descs[zid].zone_capacity;

	const __u32 lbas_per_zrwafg = zns_ssd->lbas_per_zrwafg;
	const __u32 lbas_per_zrwa = zns_ssd->lbas_per_zrwa;

	__u64 zrwa_start = wp;
	__u64 zrwa_end = min(zrwa_start + lbas_per_zrwa - 1, (size_t)zone_to_slba(zns_ssd, zid) + zone_capacity - 1); 
	__u64 nr_lbas_flush = slba - wp + 1;
	
	NVMEV_ZNS_DEBUG("%s slba 0x%llx zrwa_start 0x%llx zrwa_end 0x%llx zone_descs[zid].zrwav %d\n", 
			__FUNCTION__, slba, zrwa_start, zrwa_end, zone_descs[zid].zrwav);

	if (zone_descs[zid].zrwav == 0)
		return NVME_SC_ZNS_INVALID_ZONE_OPERATION;

	if (!(slba >= zrwa_start && slba <= zrwa_end))
		return NVME_SC_ZNS_INVALID_ZONE_OPERATION; 

	if ((nr_lbas_flush % lbas_per_zrwafg) != 0)
		return NVME_SC_INVALID_FIELD;

	switch(cur_state) {
		case ZONE_STATE_OPENED_EXPL:
		case ZONE_STATE_OPENED_IMPL:
		case ZONE_STATE_CLOSED:
		{	
			zone_descs[zid].wp = slba + 1;

			if (zone_descs[zid].wp == (zone_to_slba(zns_ssd, zid) + zone_capacity)) {
				//change state to ZSF
				if (cur_state != ZONE_STATE_CLOSED)
					release_zone_resource(zns_ssd, OPEN_ZONE);
				release_zone_resource(zns_ssd, ACTIVE_ZONE);
				release_zone_resource(zns_ssd, ZRWA_ZONE);
				change_zone_state(zns_ssd, zid, ZONE_STATE_FULL);
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

__u32 __proc_zmgmt_send(struct zns_ssd *zns_ssd, __u64 slba, __u32 action, __u32 select_all, __u32 option)
{	
	__u32 status;
	__u64 zid = lba_to_zone(zns_ssd, slba);

	switch(action) {
		case ZSA_CLOSE_ZONE:
		{
			status = __proc_zmgmt_send_close_zone(zns_ssd, zid);
			break;
		}
		case ZSA_FINISH_ZONE:
		{
			status = __proc_zmgmt_send_finish_zone(zns_ssd, zid, select_all);
			break;
		}
		case ZSA_OPEN_ZONE:
		{
			status = __zmgmt_send_open_zone(zns_ssd, zid, select_all, option);
			break;
		}
		case ZSA_RESET_ZONE:
		{
			status = __proc_zmgmt_send_reset_zone(zns_ssd, zid);
			break;
		}
		case ZSA_OFFLINE_ZONE:
		{
			status = __proc_zmgmt_send_offline_zone(zns_ssd, zid);
			break;
		}
		case ZSA_FLUSH_EXPL_ZRWA:
		{
			status = __proc_zmgmt_send_flush_explicit_zrwa(zns_ssd, slba);
			break;
		}
	}

	return status;
}

extern struct buffer zns_write_buffer;
void zns_zmgmt_send(struct nvme_request * req, struct nvme_result * ret)
{
	struct nvme_zone_mgmt_send * cmd = (struct nvme_zone_mgmt_send *)req->cmd;
	struct zns_ssd *zns_ssd= get_zns_ssd_instance();
	struct ssdparams *spp = &(zns_ssd->ssd.sp);
	__u32 select_all = cmd->select_all;
	__u32 status = NVME_SC_SUCCESS;
	
	__u32 action = cmd->zsa;
	__u32 option = cmd->zsaso;
	__u64 slba = cmd->slba;
	__u64 zid = lba_to_zone(zns_ssd, slba);
	int i,j;
	struct ppa ppa;
	struct nand_cmd swr;
	uint64_t nsecs_latest = 0, nsecs_completed;
	if (select_all) {
		buffer_refill(&zns_write_buffer);
		for (zid = 0; zid < zns_ssd->nr_zones; zid++)
			__proc_zmgmt_send(zns_ssd, zone_to_slba(zns_ssd, zid), action, true, option);
	} else {
		status = __proc_zmgmt_send(zns_ssd, slba, action, false, option);
	}
	
	NVMEV_ZNS_DEBUG("%s slba %llx zid %lu select_all %lu action %u status %lu option %lu\n", 
			__FUNCTION__, cmd->slba, zid,  select_all, cmd->zsa, status, option);
	
	if (action == ZSA_RESET_ZONE) {

		swr.type = USER_IO;
		swr.cmd = NAND_ERASE;
		swr.stime = req->nsecs_start;

		for (i = 0; i < spp->nchs; i++) {
			ppa.g.ch = i;

			for (j = 0; j < spp->luns_per_ch; j++) {
				ppa.g.lun = j;
			
				nsecs_completed = ssd_advance_status(&zns_ssd->ssd, &ppa, &swr);
				nsecs_latest = (nsecs_completed > nsecs_latest) ? nsecs_completed : nsecs_latest;
			}
    	}
		ret->nsecs_target = nsecs_latest;	
	} else {
		ret->nsecs_target = req->nsecs_start ; // no delay
	}
	ret->status = status;
	return;
}
#endif
