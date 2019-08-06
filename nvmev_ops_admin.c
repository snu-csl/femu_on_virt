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

void nvmev_admin_create_cq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_completion_queue *cq;
	unsigned int num_pages, i;
	int dbs_idx;

	cq = kzalloc(sizeof(struct nvmev_completion_queue), GFP_KERNEL);

	/* Todo : Physically dis-contiguous prp list */

	cq->qid = sq_entry(eid).create_cq.cqid;

	cq->interrupt_enabled =
		(sq_entry(eid).create_cq.cq_flags & NVME_CQ_IRQ_ENABLED)? true: false;
	cq->phys_contig =
		(sq_entry(eid).create_cq.cq_flags & NVME_QUEUE_PHYS_CONTIG)? true: false;
	cq->queue_size = sq_entry(eid).create_cq.qsize + 1;
	cq->phase = 1;

	cq->interrupt_ready = false;

	if (cq->interrupt_enabled) {
		cq->irq_vector = sq_entry(eid).create_cq.irq_vector;
		cq->irq = readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF;

		NVMEV_ERROR("%s: IRQ Vector: %d -> %d\n", __func__, cq->irq, cq->irq_vector);
	}

	//if queue size = 0 > vdev->bar->cap.mqes!!
	//num_queue_pages?

	cq->cq_head = 0;
	cq->cq_tail = -1;

	//multiple pages
	num_pages = (cq->queue_size * sizeof(struct nvme_completion)) / PAGE_SIZE;
	cq->cq = kzalloc(sizeof(struct nvme_completion*) * num_pages, GFP_KERNEL);

	for (i = 0; i < num_pages; i++) {
		cq->cq[i] = page_address(pfn_to_page(sq_entry(eid).create_cq.prp1 >> PAGE_SHIFT) + i);
	}
	vdev->cqes[cq->qid] = cq;
	spin_lock_init(&vdev->cq_entry_lock[cq->qid]);
	spin_lock_init(&vdev->cq_irq_lock[cq->qid]);

	dbs_idx = cq->qid * 2 + 1;
	vdev->dbs[dbs_idx] = 0;
	vdev->old_dbs[dbs_idx] = 0;

	cq_entry(cq_head).command_id = sq_entry(eid).create_cq.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

void nvmev_admin_delete_cq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_completion_queue *cq;
	unsigned int qid;

	qid = sq_entry(eid).delete_queue.qid;

	cq = vdev->cqes[qid];
	vdev->cqes[qid] = NULL;

	kfree(cq->cq);
	kfree(cq);

	cq_entry(cq_head).command_id = sq_entry(eid).delete_queue.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

void nvmev_admin_create_sq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_submission_queue *sq;
	unsigned int num_pages, i;
	int dbs_idx;

	sq = kzalloc(sizeof(struct nvmev_submission_queue), GFP_KERNEL);

	/* Todo : Physically dis-contiguous prp list */

	sq->qid = sq_entry(eid).create_sq.sqid;
	sq->cqid = sq_entry(eid).create_sq.cqid;

	sq->sq_priority = sq_entry(eid).create_sq.sq_flags & 0xFFFE;
	sq->phys_contig =
		(sq_entry(eid).create_sq.sq_flags & NVME_QUEUE_PHYS_CONTIG)? true: false;
	sq->queue_size = sq_entry(eid).create_sq.qsize + 1;

	//multiple pages
	num_pages = (sq->queue_size * sizeof(struct nvme_command)) / PAGE_SIZE;
	sq->sq = kzalloc(sizeof(struct nvme_command*) * num_pages, GFP_KERNEL);

	for (i = 0; i < num_pages; i++) {
		sq->sq[i] = page_address(pfn_to_page(sq_entry(eid).create_sq.prp1 >> PAGE_SHIFT) + i);
	}
	vdev->sqes[sq->qid] = sq;

	dbs_idx = sq->qid * 2;
	vdev->dbs[dbs_idx] = 0;
	vdev->old_dbs[dbs_idx] = 0;

	cq_entry(cq_head).command_id = sq_entry(eid).create_sq.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

void nvmev_admin_delete_sq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_submission_queue *sq;
	unsigned int qid;

	qid = sq_entry(eid).delete_queue.qid;

	sq = vdev->sqes[qid];
	vdev->sqes[qid] = NULL;

	kfree(sq->sq);
	kfree(sq);

	cq_entry(cq_head).command_id = sq_entry(eid).delete_queue.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

void nvmev_admin_identify_ctrl(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvme_id_ctrl* ctrl;

	ctrl = page_address(pfn_to_page(sq_entry(eid).identify.prp1 >> PAGE_SHIFT));

	ctrl->nn = 1;
	ctrl->oncs = 0; //optional command
	ctrl->acl = 3; //minimum 4 required, 0's based value
	ctrl->vwc = 0;
	snprintf(ctrl->sn, 20, "CSL_Virt_NVMe_SN_%02d", 1);
	snprintf(ctrl->mn, 40, "CSL_Virt_NVMe_MN_%02d", 1);
	snprintf(ctrl->fr, 8, "CSL_%04d", 1);
	ctrl->mdts = 5;
	ctrl->sqes = 0x66;
	ctrl->cqes = 0x44;


	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

void nvmev_admin_get_log_page(int eid, int cq_head)
{

}

void nvmev_admin_identify_namespace(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvme_id_ns* ns;

	ns = page_address(pfn_to_page(sq_entry(eid).identify.prp1 >> PAGE_SHIFT));
	memset(ns, 0x0, PAGE_SIZE);
	//ns->nsze = (1 * 1024 * 1024 * 1024) / 512;

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

	ns->nsze = (vdev->config.storage_size >> ns->lbaf[ns->flbas&0xF].ds);
	ns->ncap = ns->nsze;
	ns->nuse = ns->nsze;
	ns->nlbaf = 6;
	ns->flbas = 0;
	ns->dps = 0;

	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

void nvmev_admin_set_features(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;

	int num_queue;

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
            if (num_queue > NR_MAX_IO_QUEUE) {
                num_queue = NR_MAX_IO_QUEUE - 1;
                vdev->nr_sq = NR_MAX_IO_QUEUE;
            } else
                vdev->nr_sq = num_queue + 1;

            //CQ
            num_queue = (sq_entry(eid).features.dword11 >> 16) & 0xFFFF;
            if (num_queue > NR_MAX_IO_QUEUE) {
                num_queue = NR_MAX_IO_QUEUE - 1;
                vdev->nr_cq = NR_MAX_IO_QUEUE;
            } else
                vdev->nr_cq = num_queue + 1;

            cq_entry(cq_head).result = ((vdev->nr_cq-1)<<16 | (vdev->nr_sq-1));
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

	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

void nvmev_admin_get_features(int eid, int cq_head)
{

}

void nvmev_proc_admin(int entry_id)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	int cq_head = queue->cq_head;
	pr_info("%s: %x id-%d command-%d %d\n", __func__, sq_entry(entry_id).identify.opcode,
			entry_id, sq_entry(entry_id).common.command_id, cq_head);

	switch(sq_entry(entry_id).common.opcode) {
		case nvme_admin_delete_sq:
			nvmev_admin_delete_sq(entry_id, cq_head);
			break;
		case nvme_admin_create_sq:
			nvmev_admin_create_sq(entry_id, cq_head);
			break;
		case nvme_admin_get_log_page:
			nvmev_admin_get_log_page(entry_id, cq_head);
			break;
		case nvme_admin_delete_cq:
			nvmev_admin_delete_cq(entry_id, cq_head);
			break;
		case nvme_admin_create_cq:
			nvmev_admin_create_cq(entry_id, cq_head);
			break;
		case nvme_admin_identify:
			if (sq_entry(entry_id).identify.cns == 0x01)
				nvmev_admin_identify_ctrl(entry_id, cq_head);
			else
				nvmev_admin_identify_namespace(entry_id, cq_head);
			break;
		case nvme_admin_abort_cmd:
		case nvme_admin_set_features:
			nvmev_admin_set_features(entry_id, cq_head);
			break;
		case nvme_admin_get_features:
			nvmev_admin_get_features(entry_id, cq_head);
			break;
			break;
		case nvme_admin_async_event:
			cq_entry(cq_head).command_id = sq_entry(entry_id).features.command_id;
			cq_entry(cq_head).sq_id = 0;
			cq_entry(cq_head).sq_head = entry_id;
			cq_entry(cq_head).result = 0;
			cq_entry(cq_head).status = queue->phase | NVME_SC_ASYNC_LIMIT << 1;
			break;
		case nvme_admin_activate_fw:
		case nvme_admin_download_fw:
		case nvme_admin_format_nvm:
		case nvme_admin_security_send:
		case nvme_admin_security_recv:
		default:
			break;
	}

	if (++cq_head == queue->cq_depth) {
		cq_head = 0;
		queue->phase = !queue->phase;
	}

	queue->cq_head = cq_head;
}



void nvmev_proc_sq_admin(int new_db, int old_db)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	int num_proc = new_db - old_db;
	int cur_entry = old_db;
	int seq;

	if (unlikely(num_proc < 0)) num_proc += queue->sq_depth;

	for (seq = 0; seq < num_proc; seq++) {
		nvmev_proc_admin(cur_entry);

		if (++cur_entry == queue->sq_depth) {
			cur_entry = 0;
		}
	}

	if (vdev->msix_enabled) {
		if (unlikely(!queue->irq_desc)) {
			if (queue->vector == 0)
				queue->vector = first_msi_entry(&vdev->pdev->dev)->irq;

			queue->irq_desc = irq_to_desc(vdev->admin_q->vector);
		}
#ifdef CONFIG_NUMA
		NVMEV_DEBUG("smphint(latest): %*pbl\n",
				cpumask_pr_args(queue->irq_desc->irq_common_data.affinity));

		if (unlikely(!cpumask_subset(queue->irq_desc->irq_common_data.affinity,
						cpumask_of_node(vdev->pdev->dev.numa_node)))) {
			NVMEV_DEBUG("Not a member of node %d\n", vdev->pdev->dev.numa_node);

			NVMEV_DEBUG("Send to %*pbl, Vector %d\n",
					cpumask_pr_args(&vdev->first_cpu_on_node), queue->irq_vector);
			apic->send_IPI_mask(&vdev->first_cpu_on_node, queue->irq_vector);
		} else {
			if (unlikely(queue->irq_vector !=
				(readl(vdev->msix_table + PCI_MSIX_ENTRY_DATA) & 0xFF))) {
				queue->irq_vector = (readl(vdev->msix_table + PCI_MSIX_ENTRY_DATA) & 0xFF);
			}
			if (unlikely(cpumask_equal(queue->irq_desc->irq_common_data.affinity,
							cpumask_of_node(vdev->pdev->dev.numa_node)))) {
				NVMEV_DEBUG("EQ Send to %*pbl, Vector %d\n",
						cpumask_pr_args(&vdev->first_cpu_on_node), queue->irq_vector);
				apic->send_IPI_mask(&vdev->first_cpu_on_node, queue->irq_vector);
			} else {
				NVMEV_DEBUG("Best Send to %*pbl, Vector %d\n",
						cpumask_pr_args(queue->irq_desc->irq_common_data.affinity),
						queue->irq_vector);
				apic->send_IPI_mask(queue->irq_desc->irq_common_data.affinity,
						queue->irq_vector);

			}
		}
#else
		NVMEV_DEBUG("Intr -> all, Vector->%u\n", cq->irq);

		apic->send_IPI_mask(queue->irq_desc->irq_common_data.affinity,
				queue->irq_vector);
#endif
	} else {
		NVMEV_DEBUG("PIN INTR %u %d node-%d\n",
				queue->irq_vector, smp_processor_id(),
				vdev->pdev->dev.numa_node);
		NVMEV_DEBUG("Node %d CPU Mask %*pbl\n", vdev->pdev->dev.numa_node,
				cpumask_pr_args(cpumask_of_node(vdev->pdev->dev.numa_node)));
		NVMEV_DEBUG("Send to %*pbl, Vector %d\n",
				cpumask_pr_args(&vdev->first_cpu_on_node), queue->irq_vector);
		apic->send_IPI_mask(&vdev->first_cpu_on_node, queue->irq_vector);
		//generateInterrupt(queue->irq_vector);
	}
}

void nvmev_proc_cq_admin(int new_db, int old_db)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;

	queue->cq_tail = new_db-1;
}
