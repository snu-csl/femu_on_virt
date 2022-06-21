#include <linux/kthread.h>                               
#include <linux/jiffies.h>                                                                
#include <linux/ktime.h>   
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "zns.h"

#if SUPPORT_ZNS 

#define sq_entry(entry_id) \
	sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

struct zone_resource_info res_infos[RES_TYPE_COUNT];
struct zone_descriptor * zone_descs;

// WA, Namespace for zns is only one. not support multi zns namespace..
__u32 zns_nsid = 0; 

void * __get_media_addr(__u64 byte_offset, __u32 nsid)
{
	return  vdev->storage_mapped + byte_offset;
}

void * __get_zns_media_addr_from_lba(__u64 lba)
{
	return  __get_media_addr(LBA_TO_BYTE(lba), zns_nsid);
}

void * __get_zns_media_addr_from_zid(__u64 zid)
{
	return __get_media_addr(zid * BYTES_PER_ZONE, zns_nsid);
}

bool __acquire_zone_resource(__u32 type)
{
	if(IS_ZONE_RESOURCE_AVAIL(type)) {
		res_infos[type].acquired_cnt++;
		return true;
	}

	return false;
}

void __release_zone_resource(__u32 type)
{	
	ASSERT(res_infos[type].acquired_cnt > 0);

	res_infos[type].acquired_cnt--;
}

void __change_zone_state(__u32 zid, enum zone_state state)
{
	NVMEV_ZNS_DEBUG("change state zid %d from %d to %d \n",zid, zone_descs[zid].state, state);

	// check if transition is correct
	zone_descs[zid].state = state;
}

__u64 __prp_transfer_data(__u64 prp1, __u64 prp2, void * buffer, __u64 length, __u32 io)
{
	size_t offset;
	size_t remaining;
	size_t prp_offs = 0;
	size_t prp2_offs = 0;
	__u64 paddr;
	__u64 * paddr_list = NULL;
	size_t mem_offs = 0;
	
	//NVMEV_ZNS_DEBUG("[PRP Transfer] prp1 0x%llx prp2 0x%llx buffer 0x%p length 0x%lx io %ld \n", 
			//prp1, prp2, buffer, length, io); 

	offset = 0;
	remaining = length;
	
	while (remaining) {
		size_t io_size;
		void *vaddr;
		
		mem_offs = 0;
		prp_offs++;
		if (prp_offs == 1) {
			paddr = prp1;
		} else if (prp_offs == 2) {
			paddr = prp2;
			if (remaining > PAGE_SIZE) {
				paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) + (paddr & PAGE_OFFSET_MASK);
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			paddr = paddr_list[prp2_offs++];
		}

		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}
		
		if (io == 0) // output (buffer -> plp)
			memcpy(vaddr + mem_offs, buffer + offset, io_size);

		else // input (prp -> buffer)
			memcpy(buffer + offset, vaddr + mem_offs, io_size);

		kunmap_atomic(vaddr);

		remaining -= io_size;
		offset += io_size;
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length;
}

void ZNS_INIT(void)
{
	__u64 zslba = 0;
	__u32 i = 0;

	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	zone_descs = (struct zone_descriptor *) kmalloc(sizeof(struct zone_descriptor) * NR_MAX_ZONE, GFP_ATOMIC);
	report_buffer = (struct zone_report *) kmalloc(sizeof(struct zone_report) + sizeof(struct zone_descriptor) * (NR_MAX_ZONE - 1), GFP_ATOMIC);
	
	memset(zone_descs, 0, sizeof(struct zone_descriptor) * NR_MAX_ZONE);

	for (i = 0; i < NR_MAX_ZONE; i++) {
		zone_descs[i].state = ZONE_STATE_EMPTY;
		zone_descs[i].type = ZONE_TYPE_SEQ_WRITE_REQUIRED;

		zone_descs[i].zslba = zslba;
		zone_descs[i].wp = zslba;
		zslba += LBAS_PER_ZONE;
		zone_descs[i].zone_capacity = LBAS_PER_ZONE;

		NVMEV_ZNS_DEBUG("[i] zslba 0x%llx zone capacity 0x%llx\n", zone_descs[i].zslba, zone_descs[i].zone_capacity);
	}

	res_infos[ACTIVE_ZONE].total_cnt = NR_MAX_ACTIVE_ZONE;
	res_infos[ACTIVE_ZONE].acquired_cnt = 0;

	res_infos[OPEN_ZONE].total_cnt = NR_MAX_OPEN_ZONE;
	res_infos[OPEN_ZONE].acquired_cnt = 0;

	res_infos[ZRWA_ZONE].total_cnt = NR_MAX_OPEN_ZONE;
	res_infos[ZRWA_ZONE].acquired_cnt = 0;
}

void zns_exit(void)
{
	NVMEV_ZNS_DEBUG("%s \n", __FUNCTION__);
	
	kfree(zone_descs);
	kfree(report_buffer);
}

void zns_proc_io_cmd(struct nvmev_proc_table *pe)
{
	__u32 sqid = pe->sqid;
	__u32 sq_entry = pe->sq_entry;
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	__u32 op = sq_entry(sq_entry).common.opcode;
	__u32 status = NVME_SC_SUCCESS;

	void* cmd = &(sq->sq[SQ_ENTRY_TO_PAGE_NUM(sq_entry)][SQ_ENTRY_TO_PAGE_OFFSET(sq_entry)]);

	ASSERT(NS_CSI == NVME_CSI_ZNS);
	
	switch (op) {
		case nvme_cmd_write:
		{
			status =  zns_proc_nvme_write((struct nvme_rw_command *) cmd);
			break;
		}
		case nvme_cmd_read:
		{
			status =  zns_proc_nvme_read((struct nvme_rw_command*) cmd);
			break;
		}
		case nvme_cmd_zone_mgmt_recv:
		{
			status =  zns_proc_mgmt_recv( (struct nvme_zone_mgmt_recv *) cmd);
			break;
		}
		case nvme_cmd_zone_mgmt_send:
		{
			status =  zns_proc_mgmt_send( (struct nvme_zone_mgmt_send *) cmd);
			break;
		}
		case nvme_cmd_zone_append:
		{
			__u64 wp;
			status = zns_proc_append( (struct nvme_zone_append *) cmd, &wp);
			pe->result0 = wp & INVALID32;
			pe->result1 = (wp >> 32) & INVALID32;
			break;
		}

		case nvme_cmd_flush:
			break;

		default:
			NVMEV_ERROR("Unsupported Error %x\n", op);
	}

	if (op != nvme_cmd_zone_append) {
		pe->result0 = 0;
		pe->result1 = 0;
	}

	if (status != NVME_SC_SUCCESS)
		NVMEV_ERROR("%s ERROR %x\n", __FUNCTION__,  status);

	pe->status = status; 
}
#endif