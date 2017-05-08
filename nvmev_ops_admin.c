#include <linux/nvme.h>
#include "nvmev.h"

#define num_sq_per_page	(PAGE_SIZE / sizeof(struct nvme_command))
#define num_cq_per_page (PAGE_SIZE / sizeof(struct nvme_completion))

#define entry_sq_page_num(entry_id) (entry_id / num_sq_per_page)
#define entry_cq_page_num(entry_id) (entry_id / num_cq_per_page)

#define entry_sq_page_offs(entry_id) (entry_id % num_sq_per_page)
#define entry_cq_page_offs(entry_id) (entry_id % num_cq_per_page)


#define sq_entry(entry_id) \
	queue->nvme_sq[entry_sq_page_num(entry_id)][entry_sq_page_offs(entry_id)]
#define cq_entry(entry_id) \
	queue->nvme_cq[entry_cq_page_num(entry_id)][entry_cq_page_offs(entry_id)]

extern struct nvmev_dev *vdev;

void nvmev_admin_create_cq(int eid, int cq_head) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_completion_queue *cq;
	unsigned int num_pages, i;

	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(eid).create_cq.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	
	cq = kzalloc(sizeof(struct nvmev_completion_queue), GFP_KERNEL);
	
	/* Todo : Physically dis-contiguous prp list */

	cq->qid = sq_entry(eid).create_cq.cqid;
	cq->irq_vector = sq_entry(eid).create_cq.irq_vector;
	cq->irq = readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF;
	
	pr_err("%s: IRQ Vector: %d -> %d\n", __func__, cq->irq, cq->irq_vector);

	cq->interrupt_enabled = \
		(sq_entry(eid).create_cq.cq_flags & NVME_CQ_IRQ_ENABLED)? true: false;
	cq->phys_contig = \
		(sq_entry(eid).create_cq.cq_flags & NVME_QUEUE_PHYS_CONTIG)? true: false;
	cq->queue_size = sq_entry(eid).create_cq.qsize + 1;
	cq->phase = 1;
	//if queue size = 0 > vdev->bar->cap.mqes!!
	//num_queue_pages?
	
	cq->cq_head = 0;
	cq->cq_tail = -1;

	//multiple pages
	num_pages = (cq->queue_size * sizeof(struct nvme_completion)) / PAGE_SIZE;
	cq->cq = kzalloc(sizeof(struct nvme_completion*) * num_pages, GFP_KERNEL);

	for(i=0; i<num_pages; i++) {
		cq->cq[i] = page_address(pfn_to_page(sq_entry(eid).create_cq.prp1 >> PAGE_SHIFT) + i);
	}
	vdev->cqes[cq->qid-1] = cq;
}

void nvmev_admin_create_sq(int eid, int cq_head) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_submission_queue *sq;
	unsigned int num_pages, i;

	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(eid).create_sq.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;

	sq = kzalloc(sizeof(struct nvmev_submission_queue), GFP_KERNEL);

	/* Todo : Physically dis-contiguous prp list */

	sq->qid = sq_entry(eid).create_sq.sqid;
	sq->cqid = sq_entry(eid).create_sq.cqid - 1;

	sq->sq_priority = sq_entry(eid).create_sq.sq_flags & 0xFFFE;
	sq->phys_contig = \
		(sq_entry(eid).create_sq.sq_flags & NVME_QUEUE_PHYS_CONTIG)? true: false;
	sq->queue_size = sq_entry(eid).create_sq.qsize + 1;
	
	//multiple pages
	num_pages = (sq->queue_size * sizeof(struct nvme_command)) / PAGE_SIZE;
	sq->sq = kzalloc(sizeof(struct nvme_command*) * num_pages, GFP_KERNEL);

	for(i=0; i<num_pages; i++) {
		sq->sq[i] = page_address(pfn_to_page(sq_entry(eid).create_sq.prp1 >> PAGE_SHIFT) + i);
	}
	vdev->sqes[sq->qid-1] = sq;
}

void nvmev_admin_identify_ctrl(int eid, int cq_head) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvme_id_ctrl* ctrl;

	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;

	ctrl = page_address(pfn_to_page(sq_entry(eid).identify.prp1 >> PAGE_SHIFT));
	
	ctrl->nn = 1;
	ctrl->oncs = 0; //optional command
	ctrl->acl = 3; //minimum 4 required, 0's based value
	ctrl->vwc = 0;
	snprintf(ctrl->sn, 20, "csl_nvme_emulator_%02d", 1);
	snprintf(ctrl->mn, 40, "csl_nvme_emulator_model_%16d", 1);
	snprintf(ctrl->fr, 8, "csl_%04d", 1);
	ctrl->mdts = 0;
}

void nvmev_admin_identify_namespace(int eid, int cq_head) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvme_id_ns* ns;

	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;

	ns = page_address(pfn_to_page(sq_entry(eid).identify.prp1 >> PAGE_SHIFT));
	
	ns->nsze = (1 * 1024 * 1024 * 1024) / 512;
	ns->ncap = ns->nsze;
	ns->nlbaf = 6;
	ns->flbas = NVME_NS_FLBAS_LBA_MASK;
	ns->dps = 0;

	ns->lbaf[0].ms = 0;
	ns->lbaf[0].ds = 9;
	ns->lbaf[0].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[1].ms = 8;
	ns->lbaf[1].ds = 9;
	ns->lbaf[1].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[2].ms = 16;
	ns->lbaf[2].ds = 9;
	ns->lbaf[2].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[3].ms = 0;
	ns->lbaf[3].ds = 12;
	ns->lbaf[3].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[4].ms = 8;
	ns->lbaf[4].ds = 12;
	ns->lbaf[4].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[5].ms = 64;
	ns->lbaf[5].ds = 12;
	ns->lbaf[5].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[6].ms = 128;
	ns->lbaf[6].ds = 12;
	ns->lbaf[6].rp = NVME_LBAF_RP_BEST;
}

void nvmev_admin_set_features(int eid, int cq_head) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	
	int num_queue;

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
			//SQ
			num_queue = sq_entry(eid).features.dword11 & 0xFFFF;
			vdev->nr_sq = num_queue + 1;
			
			//CQ
			num_queue = (sq_entry(eid).features.dword11 >> 16) & 0xFFFF;
			vdev->nr_cq = num_queue + 1;

			cq_entry(cq_head).result = sq_entry(eid).features.dword11;
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
	pr_info("%s: %x %d\n", __func__, sq_entry(entry_id).identify.opcode, cq_head);

	switch(sq_entry(entry_id).common.opcode) {
		case nvme_admin_delete_sq:
		case nvme_admin_create_sq:
			nvmev_admin_create_sq(entry_id, cq_head);
		case nvme_admin_get_log_page:
		case nvme_admin_delete_cq:
			break;
		case nvme_admin_create_cq:
			nvmev_admin_create_cq(entry_id, cq_head);
			break;
		case nvme_admin_identify:
			if(sq_entry(entry_id).identify.cns == 0x01)
				nvmev_admin_identify_ctrl(entry_id, cq_head);
			else
				nvmev_admin_identify_namespace(entry_id, cq_head);
			break;
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

	queue->cq_head = cq_head;
}



void nvmev_proc_sq_admin(int new_db, int old_db) {
	struct nvmev_admin_queue *queue = vdev->admin_q;
	int num_proc = new_db - old_db;
	int seq;
	int cur_entry = old_db;
	struct irq_desc *desc;
	
	for(seq = 0; seq < num_proc; seq++) {
		nvmev_proc_admin(cur_entry);

		if(++cur_entry == queue->sq_depth) {
			cur_entry = 0;
		}
	}

	if(vdev->msix_enabled) {
		if(unlikely(!vdev->admin_q->affinity_settings)) {
			vdev->admin_q->desc = first_msi_entry(&vdev->pdev->dev);
			
			desc = irq_to_desc(vdev->admin_q->desc->irq);
			if(desc->affinity_hint) {
				queue->irq = vdev->admin_q->desc->irq;
				vdev->admin_q->affinity_settings = true;
				vdev->admin_q->cpu_mask = desc->affinity_hint;

				pr_err("===============================================\n");
			}
		}

		if(vdev->admin_q->affinity_settings)
			apic->send_IPI_mask(vdev->admin_q->cpu_mask, queue->irq_vector);
		else
			apic->send_IPI_self(queue->irq_vector);
	}
	else {
		generateInterrupt(queue->irq_vector);
	}
}

void nvmev_proc_cq_admin(int new_db, int old_db) {
	struct nvmev_admin_queue *queue = vdev->admin_q;

	queue->cq_tail = new_db-1;
}