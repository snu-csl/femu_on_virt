#include <linux/nvme.h>
#include "nvmev.h"

#define num_sq_per_page	(PAGE_SIZE / sizeof(struct nvme_command))
#define num_cq_per_page (PAGE_SIZE / sizeof(struct nvme_completion))

#define entry_sq_page_num(entry_id) (entry_id / num_sq_per_page)
#define entry_cq_page_num(entry_id) (entry_id / num_cq_per_page)

#define entry_sq_page_offs(entry_id) (entry_id % num_sq_per_page)
#define entry_cq_page_offs(entry_id) (entry_id % num_cq_per_page)

#define sq_entry(entry_id) \
	sq->sq[entry_sq_page_num(entry_id)][entry_sq_page_offs(entry_id)]
#define cq_entry(entry_id) \
	cq->cq[entry_cq_page_num(entry_id)][entry_cq_page_offs(entry_id)]


extern struct nvmev_dev *vdev;

void nvmev_proc_flush(int qid, int sq_entry, int cq_head) {
	struct nvmev_submission_queue *sq = vdev->sqes[qid];
	struct nvmev_completion_queue *cq = vdev->cqes[sq->cqid];

	cq_entry(cq_head).status = cq->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(sq_entry).features.command_id;
	cq_entry(cq_head).sq_id = qid+1;
	cq_entry(cq_head).sq_head = sq_entry;
}

void nvmev_proc_write(int qid, int sq_entry, int cq_head) {
	struct nvmev_submission_queue *sq = vdev->sqes[qid];
	struct nvmev_completion_queue *cq = vdev->cqes[sq->cqid];

	cq_entry(cq_head).status = cq->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(sq_entry).features.command_id;
	cq_entry(cq_head).sq_id = qid+1;
	cq_entry(cq_head).sq_head = sq_entry;
}

void nvmev_proc_read(int qid, int sq_entry, int cq_head) {
	struct nvmev_submission_queue *sq = vdev->sqes[qid];
	struct nvmev_completion_queue *cq = vdev->cqes[sq->cqid];

	cq_entry(cq_head).status = cq->phase | NVME_SC_SUCCESS << 1;
	cq_entry(cq_head).command_id = sq_entry(sq_entry).features.command_id;
	cq_entry(cq_head).sq_id = qid+1;
	cq_entry(cq_head).sq_head = sq_entry;
}

void nvmev_proc_nvm(int qid, int sq_entry, int cq_head) {
	struct nvmev_submission_queue *sq = vdev->sqes[qid];

	switch(sq_entry(sq_entry).common.opcode) {
		case nvme_cmd_flush:
			nvmev_proc_flush(qid, sq_entry, cq_head);
			break;
		case nvme_cmd_write: 
			nvmev_proc_write(qid, sq_entry, cq_head);
			break;
		case nvme_cmd_read:
			nvmev_proc_read(qid, sq_entry, cq_head);
			break;
		case nvme_cmd_write_uncor:
		case nvme_cmd_compare:
		case nvme_cmd_write_zeroes:
		case nvme_cmd_dsm:
		case nvme_cmd_resv_register:
		case nvme_cmd_resv_report:
		case nvme_cmd_resv_acquire:
		case nvme_cmd_resv_release:
		default:
			break;
	}
}

void nvmev_proc_sq_io(int qid, int new_db, int old_db) {
	struct nvmev_submission_queue *sq = vdev->sqes[qid];
	struct nvmev_completion_queue *cq = vdev->cqes[sq->cqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int cq_head = cq->cq_head;
	struct irq_desc *desc;

	for(seq = 0; seq < num_proc; seq++) {
		nvmev_proc_nvm(qid, sq_entry, cq_head);

		if(++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}

		if(++cq_head == cq->queue_size) {
			cq_head = 0;
			cq->phase = !cq->phase;
		}
	}

	cq->cq_head = cq_head;
	
	if(unlikely(!cq->affinity_settings)) {
		cq->irq_vector += vdev->admin_q->desc->irq;
		desc = irq_to_desc(cq->irq_vector);
		if(desc && desc->affinity_hint) {
			cq->affinity_settings = true;
			cq->cpu_mask = desc->affinity_hint;
		}
	}
	
	
	if(unlikely(!cq->affinity_settings)) {
		generateInterrupt(cq->irq);
	}
	else {
		apic->send_IPI_mask(cq->cpu_mask, cq->irq);
	}

}

void nvmev_proc_cq_io(int qid, int new_db, int old_db) {
	struct nvmev_completion_queue *queue = vdev->cqes[qid];

	queue->cq_tail = new_db-1;
}
