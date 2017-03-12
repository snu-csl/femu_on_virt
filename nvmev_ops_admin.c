#include <linux/nvme.h>
#include "nvmev.h"

#define sq_entry(entry_id) queue->nvme_sq[entry_id]
#define cq_entry(entry_id) queue->nvme_cq[entry_id]

extern struct nvmev_dev *vdev;

void nvmev_admin_create_cq(int eid, int cq_head) {
	struct nvmev_admin_queue *queue = vdev->admin_q;

	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;

	//nvme_alloc_sq_cmds 851b7d000 ffff880851b7d000
	
}
void nvmev_admin_set_features(int eid, int cq_head) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	
	int tmp;

	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	
	pr_info("%s: %x\n", __func__, sq_entry(eid).features.fid);
	switch(sq_entry(eid).features.fid) {
		case NVME_FEAT_ARBITRATION:
		case NVME_FEAT_POWER_MGMT:
		case NVME_FEAT_LBA_RANGE:
		case NVME_FEAT_TEMP_THRESH:
		case NVME_FEAT_ERR_RECOVERY:
		case NVME_FEAT_VOLATILE_WC:
			break;
		case NVME_FEAT_NUM_QUEUES:
			tmp = sq_entry(eid).features.dword11 & 0xFFFF;
			vdev->nr_max_ioq = tmp+1;
			cq_entry(cq_head).result = ((tmp) << 16) | (tmp);
			break;
		case NVME_FEAT_IRQ_COALESCE:
		case NVME_FEAT_IRQ_CONFIG:
		case NVME_FEAT_WRITE_ATOMIC:
		case NVME_FEAT_ASYNC_EVENT:
		case NVME_FEAT_AUTO_PST:
		case NVME_FEAT_SW_PROGRESS:
		case NVME_FEAT_HOST_ID:
		case NVME_FEAT_RESV_MASK:
		case NVME_FEAT_RESV_PERSIST:
		default:
			break;
	}

}

void nvmev_proc_admin(int entry_id) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	int cq_head = queue->cq_head;
	pr_info("%s: %x\n", __func__, sq_entry(entry_id).identify.opcode);

	switch(sq_entry(entry_id).identify.opcode) {
		case nvme_admin_delete_sq:
		case nvme_admin_create_sq:
		case nvme_admin_get_log_page:
		case nvme_admin_delete_cq:
			break;
		case nvme_admin_create_cq:
			nvmev_admin_create_cq(entry_id, cq_head);
			break;
		case nvme_admin_identify:
		case nvme_admin_abort_cmd:
		case nvme_admin_set_features:
			nvmev_admin_set_features(entry_id, cq_head);
			break;
		case nvme_admin_get_features:
		case nvme_admin_async_event:
		case nvme_admin_activate_fw:
		case nvme_admin_download_fw:
		case nvme_admin_format_nvm:
		case nvme_admin_security_send:
		case nvme_admin_security_recv:
		default:
			break;
	}

	if(++cq_head == queue->cq_depth) {
		cq_head = 0;
		queue->phase = !queue->phase;
	}
}

void nvmev_proc_sq_admin(int new_db, int old_db) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	int num_proc = new_db - old_db;
	int seq;
	int cur_entry = old_db;

	for(seq = 0; seq < num_proc; seq++) {
		nvmev_proc_admin(cur_entry);

		if(++cur_entry == queue->sq_depth) {
			cur_entry = 0;
		}
	}

	generateInterrupt(queue->irq_vector);
}

void nvmev_proc_cq_admin(int new_db, int old_db) {
	struct nvmev_admin_queue *queue = vdev->admin_q;

	queue->cq_tail = new_db-1;
}
