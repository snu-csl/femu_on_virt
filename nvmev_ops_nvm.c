#include <linux/nvme.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
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

long long int elapsed_usecs(int opcode, unsigned int length) {
	long long int elapsed_usecs = 0;

	switch(opcode) {
		case nvme_cmd_write: 
			break;
		case nvme_cmd_read:
			break;
		default:
			break;
	}

	return jiffies_to_usecs(elapsed_usecs);
}

void nvmev_proc_flush(int sqid, int sq_entry) {
	return;
}

unsigned int nvmev_proc_write(int sqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];


	//LBA, LENGTH
	//sq_entry(sq_entry).rw.slba;
	//sq_entry(sq_entry).rw.length;
	//sq_entry(sq_entry).rw.prp1;
	//sq_entry(sq_entry).rw.prp2;
	NVMEV_ERROR("qid %d entry %d lba %llu length %d\n",
			sqid, sq_entry,
			sq_entry(sq_entry).rw.slba,
			sq_entry(sq_entry).rw.length);

	return sq_entry(sq_entry).rw.length; 
}

unsigned int nvmev_proc_read(int sqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	//LBA, LENGTH
	//sq_entry(sq_entry).rw.slba;
	//sq_entry(sq_entry).rw.length;
	//sq_entry(sq_entry).rw.prp1;
	//sq_entry(sq_entry).rw.prp2;
	NVMEV_ERROR("qid %d entry %d lba %llu length %d\n",
			sqid, sq_entry,
			sq_entry(sq_entry).rw.slba,
			sq_entry(sq_entry).rw.length);

	return sq_entry(sq_entry).rw.length; 
}

void nvmev_proc_io_enqueue(int sqid, int cqid, int sq_entry,
		long long int usecs_start, long long int usecs_elapse) {
	long long int usecs_target = usecs_start + usecs_elapse;
	unsigned int new_entry = vdev->proc_free_seq;
	unsigned int curr_entry = -1;

	vdev->proc_free_seq = vdev->proc_table[new_entry].next;
	NVMEV_ERROR("New Entry %u, %d %d, %d\n", new_entry, sqid, cqid, sq_entry);
	vdev->proc_table[new_entry].sqid = sqid;
	vdev->proc_table[new_entry].cqid= cqid;
	vdev->proc_table[new_entry].sq_entry = sq_entry;
	vdev->proc_table[new_entry].usecs_start = usecs_start;
	vdev->proc_table[new_entry].usecs_target = usecs_target;
	vdev->proc_table[new_entry].next = -1;
	vdev->proc_table[new_entry].prev = -1;

	// (END) -> (START) order, usecs target ascending order
	if(vdev->proc_io_seq_start == -1) {
		vdev->proc_io_seq_end = new_entry;
		vdev->proc_io_seq_start = new_entry;
		NVMEV_ERROR("New\n");
	}
	else {
		curr_entry = vdev->proc_io_seq_end;

		while(curr_entry != -1) {
			if(vdev->proc_table[curr_entry].usecs_target <= vdev->proc_io_usecs)
				break;

			if(vdev->proc_table[curr_entry].usecs_target <= usecs_target)
				break;

			curr_entry = vdev->proc_table[curr_entry].prev;
		}

		if(curr_entry == -1) {
			NVMEV_ERROR("Mid First\n");
			vdev->proc_table[new_entry].next = vdev->proc_io_seq_start;
			vdev->proc_io_seq_start = new_entry;
		}
		else if(vdev->proc_table[curr_entry].next == -1) {
			NVMEV_ERROR("Mid Last\n");
			vdev->proc_table[curr_entry].next = new_entry;
			vdev->proc_table[new_entry].prev = curr_entry;
			vdev->proc_io_seq_end = new_entry;
		}
		else {
			NVMEV_ERROR("Mid\n");
			vdev->proc_table[new_entry].prev = curr_entry;
			vdev->proc_table[new_entry].next = vdev->proc_table[curr_entry].next;

			vdev->proc_table[vdev->proc_table[curr_entry].next].prev = new_entry;
			vdev->proc_table[curr_entry].next = new_entry;
		}
	}
}

void nvmev_proc_io_cleanup(void) {
	if(vdev->proc_cleanup_seq_start == -1)
		return;

	if(vdev->proc_cleanup_seq_start == vdev->proc_io_seq_start)
		return;
 
	NVMEV_ERROR("Cleanup %u -> %u\n", vdev->proc_cleanup_seq_start, vdev->proc_cleanup_seq_end);
	vdev->proc_table[vdev->proc_cleanup_seq_end].next = vdev->proc_free_seq;
	vdev->proc_table[vdev->proc_cleanup_seq_start].prev = -1;

	vdev->proc_free_seq = vdev->proc_cleanup_seq_start;
	NVMEV_ERROR("Free Seq = %u\n", vdev->proc_free_seq);
	vdev->proc_cleanup_seq_start = vdev->proc_table[vdev->proc_cleanup_seq_end].next;
	NVMEV_ERROR("Cleanup Start = %u\n", vdev->proc_table[vdev->proc_cleanup_seq_end].next);
}

void nvmev_proc_nvm(int sqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	long long int usecs_start = ktime_to_us(ktime_get());
	long long int usecs_elapsed = 0;
	unsigned int io_len;

	switch(sq_entry(sq_entry).common.opcode) {
		case nvme_cmd_flush:
			nvmev_proc_flush(sqid, sq_entry);
			break;
		case nvme_cmd_write: 
			io_len = nvmev_proc_write(sqid, sq_entry);
			usecs_elapsed = elapsed_usecs(nvme_cmd_write, io_len);
			break;
		case nvme_cmd_read:
			io_len = nvmev_proc_read(sqid, sq_entry);
			usecs_elapsed = elapsed_usecs(nvme_cmd_read, io_len);
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

	nvmev_proc_io_enqueue(sqid, sq->cqid, sq_entry, 
			usecs_start, usecs_elapsed);
	nvmev_proc_io_cleanup();
}

void nvmev_proc_sq_io(int sqid, int new_db, int old_db) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;

	if(unlikely(num_proc < 0)) num_proc+=sq->queue_size;
	if(unlikely(!sq)) return;

	for(seq = 0; seq < num_proc; seq++) {
		nvmev_proc_nvm(sqid, sq_entry);

		if(++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
	}
}

void nvmev_proc_cq_io(int cqid, int new_db, int old_db) {
	struct nvmev_completion_queue *cq= vdev->cqes[cqid];

	cq->cq_tail = new_db - 1;
	if(new_db == -1) cq->cq_tail = cq->queue_size-1;
}

void nvmev_intr_issue(int cqid) {
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	struct irq_desc *desc;

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

void fill_cq_result(int sqid, int cqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head = cq->cq_head;

	cq_entry(cq_head).command_id = sq_entry(sq_entry).rw.command_id;
	cq_entry(cq_head).sq_id = sqid;
	cq_entry(cq_head).sq_head = sq_entry;
	cq_entry(cq_head).status = cq->phase | NVME_SC_SUCCESS << 1;

	if(++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
}

static int nvmev_kthread_io_proc(void *data)
{
	long long int curr_usecs; 
	unsigned int curr_entry, start_entry=-1, last_entry=-1;
	struct nvmev_completion_queue *cq;// = vdev->cqes[cqid];
	int qidx;

	while(!kthread_should_stop()) {
		curr_usecs = ktime_to_us(ktime_get());
		vdev->proc_io_usecs = curr_usecs;
		curr_entry = vdev->proc_io_seq_start;
		last_entry = -1;
		while(curr_entry != -1) {

			if(vdev->proc_table[curr_entry].usecs_target <= 
					curr_usecs) {
				fill_cq_result(vdev->proc_table[curr_entry].sqid,
						vdev->proc_table[curr_entry].cqid,
						vdev->proc_table[curr_entry].sq_entry);
				NVMEV_ERROR("proc Entry %u, %d %d, %d\n", curr_entry, 
						vdev->proc_table[curr_entry].sqid, 
						vdev->proc_table[curr_entry].cqid,
						vdev->proc_table[curr_entry].sq_entry);
	
				cq = vdev->cqes[vdev->proc_table[curr_entry].cqid];
				cq->interrupt_ready = true;
			
				if(start_entry == -1) start_entry = curr_entry;
				last_entry = curr_entry;
				curr_entry = vdev->proc_table[curr_entry].next;
			}
			else 
				break;
		}

		if(last_entry != -1) {
			vdev->proc_cleanup_seq_end = last_entry;
			NVMEV_ERROR("Cleanup End = %u\n", last_entry);
		}
		if(vdev->proc_cleanup_seq_start == -1 && start_entry != -1) {
			vdev->proc_cleanup_seq_start = start_entry;
			NVMEV_ERROR("Cleanup Start = %u\n", start_entry);
		}

		vdev->proc_io_seq_start = curr_entry;
		NVMEV_ERROR("Next IO Start = %u\n", curr_entry);

		for(qidx=1; qidx <= vdev->nr_cq; qidx++) {
			cq = vdev->cqes[qidx];
			if(cq == NULL) continue;

			if(cq->interrupt_ready == true) {
				cq->interrupt_ready = false;
				nvmev_intr_issue(qidx);
			}
		}

		//schedule_timeout(round_usecs_relative(HZ));
		schedule_timeout(jiffies_to_usecs(1));
	}

	return 0;
}

void NVMEV_IO_PROC_INIT(struct nvmev_dev* vdev) {
	unsigned int i;

	vdev->proc_table = kzalloc(sizeof(struct nvmev_proc_table) * NR_MAX_IO_QUEUE * NR_MAX_PARALLEL_IO, GFP_KERNEL);

	for(i=0; i<(NR_MAX_IO_QUEUE*NR_MAX_PARALLEL_IO); i++) {
		vdev->proc_table[i].next = i+1;
		vdev->proc_table[i].prev = i-1;
	}

	vdev->proc_table[(NR_MAX_IO_QUEUE*NR_MAX_PARALLEL_IO)-1].next = -1;

	vdev->proc_free_seq = 0;
	vdev->proc_io_seq_start = -1;
	vdev->proc_io_seq_end = -1;
	vdev->proc_cleanup_seq_start = -1;
	vdev->proc_cleanup_seq_end = -1;


	vdev->proc_io_usecs = ktime_to_us(ktime_get());

	vdev->nvmev_io_proc = kthread_create(nvmev_kthread_io_proc, 
			NULL, "nvmev_proc_io");
	NVMEV_ERROR("Proc IO : %d\n", vdev->config.cpu_nr_proc_io);
	if(vdev->config.cpu_nr_proc_io != -1)
		kthread_bind(vdev->nvmev_io_proc, vdev->config.cpu_nr_proc_io);
	wake_up_process(vdev->nvmev_io_proc);
}

void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev) {
	if(!IS_ERR_OR_NULL(vdev->nvmev_io_proc)) {
		kthread_stop(vdev->nvmev_io_proc);
		vdev->nvmev_io_proc = NULL;
	}

	kfree(vdev->proc_table);
}
