/**********************************************************************
 * Copyright (c) 2020-2021
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTIABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 **********************************************************************/

#include "nvmev.h"
#include "ftl.h"
#define sq_entry(entry_id) \
	queue->nvme_sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) \
	queue->nvme_cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern struct nvmev_dev *vdev;

static void __nvmev_admin_create_cq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_completion_queue *cq;
	struct nvme_create_cq *cmd = &sq_entry(eid).create_cq;
	unsigned int num_pages, i;
	int dbs_idx;

	cq = kzalloc(sizeof(struct nvmev_completion_queue), GFP_KERNEL);

	cq->qid = cmd->cqid;

	cq->irq_enabled = cmd->cq_flags & NVME_CQ_IRQ_ENABLED ? true : false;
	if (cq->irq_enabled) {
		cq->irq_vector = cmd->irq_vector;
	}
	cq->interrupt_ready = false;

	cq->queue_size = cmd->qsize + 1;
	cq->phase = 1;

	cq->cq_head = 0;
	cq->cq_tail = -1;

	spin_lock_init(&cq->entry_lock);
	spin_lock_init(&cq->irq_lock);

	/* TODO Physically non-contiguous prp list */
	cq->phys_contig = cmd->cq_flags & NVME_QUEUE_PHYS_CONTIG ? true : false;
	WARN_ON(!cq->phys_contig);

	num_pages = DIV_ROUND_UP(cq->queue_size * sizeof(struct nvme_completion), PAGE_SIZE);
	cq->cq = kzalloc(sizeof(struct nvme_completion*) * num_pages, GFP_KERNEL);
	for (i = 0; i < num_pages; i++) {
		cq->cq[i] = page_address(pfn_to_page(cmd->prp1 >> PAGE_SHIFT) + i);
	}

	vdev->cqes[cq->qid] = cq;

	dbs_idx = cq->qid * 2 + 1;
	vdev->dbs[dbs_idx] = vdev->old_dbs[dbs_idx] = 0;

	cq_entry(cq_head).command_id = cmd->command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_delete_cq(int eid, int cq_head)
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

static void __nvmev_admin_create_sq(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvmev_submission_queue *sq;
	struct nvme_create_sq *cmd = &sq_entry(eid).create_sq;
	unsigned int num_pages, i;
	int dbs_idx;

	sq = kzalloc(sizeof(struct nvmev_submission_queue), GFP_KERNEL);

	sq->qid = cmd->sqid;
	sq->cqid = cmd->cqid;

	sq->sq_priority = cmd->sq_flags & 0xFFFE;
	sq->queue_size = cmd->qsize + 1;

	/* TODO Physically non-contiguous prp list */
	sq->phys_contig = (cmd->sq_flags & NVME_QUEUE_PHYS_CONTIG) ? true : false;
	WARN_ON(!sq->phys_contig);

	num_pages = DIV_ROUND_UP(sq->queue_size * sizeof(struct nvme_command), PAGE_SIZE);
	sq->sq = kzalloc(sizeof(struct nvme_command*) * num_pages, GFP_KERNEL);

	for (i = 0; i < num_pages; i++) {
		sq->sq[i] = page_address(pfn_to_page(cmd->prp1 >> PAGE_SHIFT) + i);
	}
	vdev->sqes[sq->qid] = sq;

	dbs_idx = sq->qid * 2;
	vdev->dbs[dbs_idx] = 0;
	vdev->old_dbs[dbs_idx] = 0;

	NVMEV_DEBUG("%s: %d\n", __func__, sq->qid);

	cq_entry(cq_head).command_id = cmd->command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_delete_sq(int eid, int cq_head)
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

static void __nvmev_admin_identify_ctrl(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvme_id_ctrl *ctrl;

	ctrl = page_address(pfn_to_page(sq_entry(eid).identify.prp1 >> PAGE_SHIFT));
	memset(ctrl, 0x00, sizeof(*ctrl));

	ctrl->nn = 1;
	ctrl->oncs = 0; //optional command
	ctrl->acl = 3; //minimum 4 required, 0's based value
	ctrl->vwc = 0;
	snprintf(ctrl->sn, sizeof(ctrl->sn), "CSL_Virt_SN_%02d", 1);
	snprintf(ctrl->mn, sizeof(ctrl->mn), "CSL_Virt_MN_%02d", 1);
	snprintf(ctrl->fr, sizeof(ctrl->fr), "CSL_%03d", 2);
	ctrl->mdts = 5;
	ctrl->sqes = 0x66;
	ctrl->cqes = 0x44;

	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_SUCCESS << 1;
}

static void __nvmev_admin_get_log_page(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;

	cq_entry(cq_head).command_id = sq_entry(eid).features.command_id;
	cq_entry(cq_head).sq_id = 0;
	cq_entry(cq_head).sq_head = eid;
	cq_entry(cq_head).status = queue->phase | NVME_SC_INVALID_FIELD << 1;
}

static void __nvmev_admin_identify_namespace(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	struct nvme_id_ns *ns;

	ns = page_address(pfn_to_page(sq_entry(eid).identify.prp1 >> PAGE_SHIFT));
	memset(ns, 0x0, PAGE_SIZE);

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

	ns->nsze = (vdev->config.storage_size >> ns->lbaf[ns->flbas & 0xF].ds);
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

static void __nvmev_admin_set_features(int eid, int cq_head)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;

	NVMEV_DEBUG("%s: %x\n", __func__, sq_entry(eid).features.fid);

	switch(sq_entry(eid).features.fid) {
		case NVME_FEAT_ARBITRATION:
		case NVME_FEAT_POWER_MGMT:
		case NVME_FEAT_LBA_RANGE:
		case NVME_FEAT_TEMP_THRESH:
		case NVME_FEAT_ERR_RECOVERY:
		case NVME_FEAT_VOLATILE_WC:
			break;
		case NVME_FEAT_NUM_QUEUES: {
			int num_queue;

            // # of sq in 0-base
            num_queue = (sq_entry(eid).features.dword11 & 0xFFFF) + 1;
			vdev->nr_sq = min(num_queue, NR_MAX_IO_QUEUE);

            // # of cq in 0-base
            num_queue = ((sq_entry(eid).features.dword11 >> 16) & 0xFFFF) + 1;
			vdev->nr_cq = min(num_queue, NR_MAX_IO_QUEUE);

            cq_entry(cq_head).result = ((vdev->nr_cq - 1) << 16 | (vdev->nr_sq - 1));
			break;
		}
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

static void __nvmev_admin_get_features(int eid, int cq_head)
{

}

static void __nvmev_proc_admin_req(int entry_id)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	int cq_head = queue->cq_head;

	NVMEV_DEBUG("%s: %x %d %d %d\n", __func__,
			sq_entry(entry_id).identify.opcode,
			entry_id, sq_entry(entry_id).common.command_id, cq_head);

	switch(sq_entry(entry_id).common.opcode) {
		case nvme_admin_delete_sq:
			__nvmev_admin_delete_sq(entry_id, cq_head);
			break;
		case nvme_admin_create_sq:
			__nvmev_admin_create_sq(entry_id, cq_head);
			break;
		case nvme_admin_get_log_page:
			__nvmev_admin_get_log_page(entry_id, cq_head);
			break;
		case nvme_admin_delete_cq:
			__nvmev_admin_delete_cq(entry_id, cq_head);
			break;
		case nvme_admin_create_cq:
			__nvmev_admin_create_cq(entry_id, cq_head);
			break;
		case nvme_admin_identify:
			if (sq_entry(entry_id).identify.cns == 0x01)
				__nvmev_admin_identify_ctrl(entry_id, cq_head);
			else
				__nvmev_admin_identify_namespace(entry_id, cq_head);
			break;
		case nvme_admin_abort_cmd:
			break;
		case nvme_admin_set_features:
			__nvmev_admin_set_features(entry_id, cq_head);
			break;
		case nvme_admin_get_features:
			__nvmev_admin_get_features(entry_id, cq_head);
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
			NVMEV_ERROR("Unhandled admin requests: %d",
					sq_entry(entry_id).common.opcode);
			break;
	}

	if (++cq_head == queue->cq_depth) {
		cq_head = 0;
		queue->phase = !queue->phase;
	}

	queue->cq_head = cq_head;
}


void nvmev_proc_admin_sq(int new_db, int old_db)
{
	struct nvmev_admin_queue *queue = vdev->admin_q;
	int num_proc = new_db - old_db;
	int curr = old_db;
	int seq;

	if (num_proc < 0) num_proc += queue->sq_depth;

	for (seq = 0; seq < num_proc; seq++) {
		__nvmev_proc_admin_req(curr++);

		if (curr == queue->sq_depth) {
			curr = 0;
		}
	}

	nvmev_signal_irq(0);
}

void nvmev_proc_admin_cq(int new_db, int old_db)
{
}
