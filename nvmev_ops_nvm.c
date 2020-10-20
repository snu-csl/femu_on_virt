/**********************************************************************
 * Copyright (c) 2020
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
 * *********************************************************************/

#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include "nvmev.h"

#undef PERF_DEBUG

#define PRP_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

#define sq_entry(entry_id) \
	sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) \
	cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern struct nvmev_dev *vdev;

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_proc_reg);
}

/* Return the time to complete */
static unsigned long long __schedule_io_units(int opcode, unsigned long lba, unsigned int length, unsigned long long nsecs_start)
{
	unsigned int io_unit_size = 1 << vdev->config.io_unit_shift;
	unsigned int io_unit = (lba >> (vdev->config.io_unit_shift - 9)) % vdev->config.nr_io_units;
	int nr_io_units = min(vdev->config.nr_io_units, DIV_ROUND_UP(length, io_unit_size));
	unsigned long long latest;	/* Time of completion */
	unsigned int delay = 0;
	unsigned int latency = 0;
	unsigned int trailing = 0;

	if (opcode == nvme_cmd_write) {
		delay = vdev->config.write_delay;
		latency = vdev->config.write_time;
		trailing = vdev->config.write_trailing;
	} else if (opcode == nvme_cmd_read) {
		delay = vdev->config.read_delay;
		latency = vdev->config.read_time;
		trailing = vdev->config.read_trailing;
	}

	latest = max(nsecs_start, vdev->unit_stat[io_unit]) + delay - latency;

	do {
		latest += latency;

		vdev->unit_stat[io_unit] = latest;
		if (nr_io_units-- > 0) {
			vdev->unit_stat[io_unit] += trailing;
		}

		length -= min(length, io_unit_size);
		if (++io_unit >= vdev->config.nr_io_units) io_unit = 0;
	} while (length > 0);

	return latest;
}

static unsigned int __do_perform_io(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	size_t mem_offs = 0;

	offset = sq_entry(sq_entry).rw.slba << 9;
	length = (sq_entry(sq_entry).rw.length + 1) << 9;
	remaining = length;

	while (remaining) {
		size_t io_size;
		void *vaddr;
		const unsigned long PAGE_OFFSET_MASK = PAGE_SIZE - 1;

		prp_offs++;
		if (prp_offs == 1) {
			paddr = sq_entry(sq_entry).rw.prp1;
		} else if (prp_offs == 2) {
			paddr = sq_entry(sq_entry).rw.prp2;
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
		if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
			memcpy(vdev->storage_mapped + offset, vaddr + mem_offs, io_size);
		} else {
			memcpy(vaddr + mem_offs, vdev->storage_mapped + offset, io_size);
		}

		kunmap_atomic(vaddr);

		remaining -= io_size;
		offset += io_size;
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length;
}

static void __nvmev_proc_flush(int sqid, int sq_entry)
{
#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	NVMEV_DEBUG("qid %d entry %d sq %p\n", sqid, sq_entry, sq);
#endif
	return;
}


static unsigned int __req_io_size(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	unsigned int length_bytes;

	length_bytes = (sq_entry(sq_entry).rw.length + 1) << 9;

	return length_bytes;
}

static unsigned int __nvmev_proc_write(int sqid, int sq_entry)
{
#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	NVMEV_DEBUG("qid %d entry %d lba %llu length %d, %llx %llx\n",
			sqid, sq_entry,
			sq_entry(sq_entry).rw.slba,
			sq_entry(sq_entry).rw.length,
			sq_entry(sq_entry).rw.prp1,
			sq_entry(sq_entry).rw.prp2);
#endif

	return __req_io_size(sqid, sq_entry);
}

static unsigned int __nvmev_proc_read(int sqid, int sq_entry)
{
#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	NVMEV_DEBUG("qid %d entry %d lba %llu length %d, %llx %llx\n",
			sqid, sq_entry,
			sq_entry(sq_entry).rw.slba,
			sq_entry(sq_entry).rw.length,
			sq_entry(sq_entry).rw.prp1,
			sq_entry(sq_entry).rw.prp2);
#endif

	return __req_io_size(sqid, sq_entry);
}


static void __enqueue_io_req(int sqid, int cqid, int sq_entry, unsigned long long nsecs_start, unsigned long long nsecs_target)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	unsigned int proc_turn = vdev->proc_turn;
	struct nvmev_proc_info *pi = &vdev->proc_info[proc_turn];
	unsigned int entry = pi->free_seq;

	if (++proc_turn == vdev->config.nr_io_cpu) proc_turn = 0;
	vdev->proc_turn = proc_turn;

	pi->free_seq = pi->proc_table[entry].next;

	NVMEV_DEBUG("New entry %s/%u[%d], sq %d cq %d, entry %d %llu + %llu\n",
			pi->thread_name, entry, sq_entry(sq_entry).rw.opcode,
			sqid, cqid, sq_entry, nsecs_start, nsecs_target - nsecs_start);

	/////////////////////////////////
	pi->proc_table[entry].sqid = sqid;
	pi->proc_table[entry].cqid = cqid;
	pi->proc_table[entry].sq_entry = sq_entry;
	pi->proc_table[entry].command_id = sq_entry(sq_entry).common.command_id;
	pi->proc_table[entry].nsecs_start = nsecs_start;
	pi->proc_table[entry].nsecs_enqueue = local_clock();
	pi->proc_table[entry].nsecs_target = nsecs_target;
	pi->proc_table[entry].is_completed = false;
	pi->proc_table[entry].is_copied = false;
	pi->proc_table[entry].prev = -1;
	pi->proc_table[entry].next = -1;

	// (END) -> (START) order, nsecs target ascending order
	if (pi->io_seq == -1) {
		pi->io_seq = entry;
		pi->io_seq_end = entry;
	} else {
		unsigned int curr_entry = pi->io_seq_end;

		while (curr_entry != -1) {
			if (pi->proc_table[curr_entry].nsecs_target <= pi->proc_io_nsecs)
				break;

			if (pi->proc_table[curr_entry].nsecs_target <= nsecs_target)
				break;

			curr_entry = pi->proc_table[curr_entry].prev;
		}

		if (curr_entry == -1) {
			pi->proc_table[entry].next = pi->io_seq;
			pi->io_seq = entry;
		} else if (pi->proc_table[curr_entry].next == -1) {
			pi->proc_table[entry].prev = curr_entry;
			pi->io_seq_end = entry;
			pi->proc_table[curr_entry].next = entry;
		} else {
			pi->proc_table[entry].prev = curr_entry;
			pi->proc_table[entry].next = pi->proc_table[curr_entry].next;

			pi->proc_table[pi->proc_table[entry].next].prev = entry;
			pi->proc_table[curr_entry].next = entry;
		}
	}
}


static void __nvmev_proc_io_cleanup(void)
{
	unsigned int turn;

	for (turn = 0; turn < vdev->config.nr_io_cpu; turn++) {
		struct nvmev_proc_info *pi;
		struct nvmev_proc_table *proc_entry;

		unsigned int start_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;

		pi = &vdev->proc_info[turn];

		start_entry = pi->io_seq;
		curr = start_entry;

		while (curr != -1) {
			proc_entry = &pi->proc_table[curr];
			if (proc_entry->is_completed == true && proc_entry->is_copied == true &&
					proc_entry->nsecs_target <= pi->proc_io_nsecs) {
				last_entry = curr;
				curr = proc_entry->next;
			} else
				break;
		}

		if (start_entry != -1 && last_entry != -1) {
			proc_entry = &pi->proc_table[last_entry];
			pi->io_seq = proc_entry->next;
			proc_entry->next = -1;

			proc_entry = &pi->proc_table[start_entry];
			proc_entry->prev = pi->free_seq_end;

			proc_entry = &pi->proc_table[pi->free_seq_end];
			proc_entry->next = start_entry;

			pi->free_seq_end = last_entry;

			NVMEV_DEBUG("Cleanup %u -> %u\n", start_entry, last_entry);
		}
	}
}


static int __nvmev_proc_io(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	unsigned long long nsecs_target = 0;
	unsigned long long nsecs_start = local_clock();
	unsigned int io_len = 0;
#ifdef PERF_DEBUG
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	switch(sq_entry(sq_entry).common.opcode) {
	case nvme_cmd_write:
		io_len = __nvmev_proc_write(sqid, sq_entry);
		nsecs_target = __schedule_io_units(nvme_cmd_write,
					sq_entry(sq_entry).rw.slba, io_len, nsecs_start);
		break;
	case nvme_cmd_read:
		io_len = __nvmev_proc_read(sqid, sq_entry);
		nsecs_target = __schedule_io_units(nvme_cmd_read,
					sq_entry(sq_entry).rw.slba, io_len, nsecs_start);
		break;
	case nvme_cmd_flush:
		__nvmev_proc_flush(sqid, sq_entry);
		nsecs_target = nsecs_start;
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

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

	__enqueue_io_req(sqid, sq->cqid, sq_entry, nsecs_start, nsecs_target);

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	__nvmev_proc_io_cleanup();

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - nsecs_start);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		pr_info("LAT: %llu, ENQ: %llu, CLN: %llu\n",
				clock1 / counter, clock2 / counter, clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return io_len;
}

void nvmev_proc_io_sq(int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;

	if (unlikely(num_proc < 0)) num_proc += sq->queue_size;
	if (unlikely(!sq)) return;

	for (seq = 0; seq < num_proc; seq++) {
		int io_size;

		io_size = __nvmev_proc_io(sqid, sq_entry);

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
		vdev->sq_stats[sqid].nr_dispatched++;
		vdev->sq_stats[sqid].nr_in_flight++;
		vdev->sq_stats[sqid].total_io += io_size;
	}
	vdev->sq_stats[sqid].nr_dispatch++;
	vdev->sq_stats[sqid].max_nr_in_flight =
		max_t(int, vdev->sq_stats[sqid].max_nr_in_flight,
				vdev->sq_stats[sqid].nr_in_flight);
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}
		vdev->sq_stats[cq_entry(i).sq_id].nr_in_flight--;
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1) cq->cq_tail = cq->queue_size - 1;
}

static void __signal_cq_completion(int cqid)
{
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];

	if (vdev->msix_enabled) {
		struct cpumask *target = &vdev->first_cpu_on_node; /* Signal 0 by default */
		int irq_vector = readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF;
		struct msi_desc *msi_desc;
		struct irq_desc *irq_desc;

		for_each_msi_entry(msi_desc, (&vdev->pdev->dev)) {
			if (msi_desc->msi_attrib.entry_nr == cq->irq_vector) {
				irq_desc = irq_to_desc(msi_desc->irq);
				break;
			}
		}

#ifdef CONFIG_NUMA
		NVMEV_DEBUG("smphint(latest): %*pbl, vector:%u\n",
				cpumask_pr_args(irq_desc->irq_common_data.affinity),
				irq_vector);

		if (!cpumask_subset(irq_desc->irq_common_data.affinity,
						cpumask_of_node(vdev->pdev->dev.numa_node))) {
			NVMEV_DEBUG("Invalid affinity, -> 0\n");
		} else {
			if (cpumask_equal(irq_desc->irq_common_data.affinity,
						cpumask_of_node(vdev->pdev->dev.numa_node))) {
				NVMEV_DEBUG("Every... -> 0 %d\n", irq_vector);
			} else {
				NVMEV_DEBUG("Send to Target CPU, %d\n", irq_vector);
				target = irq_desc->irq_common_data.affinity;
			}
		}
#else
		printk("Intr -> all, Vector-> %u\n", irq_vector);
		target = irq_desc->irq_common_data.affinity;
#endif 
		//printk("%d: %*pbl\n", irq_vector, cpumask_pr_args(target));
		apic->send_IPI_mask(target, irq_vector);
	} else {
		generateInterrupt(cq->irq_vector);
	}
}

static void __fill_cq_result(int sqid, int cqid, int sq_entry, unsigned int command_id)
{
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head = cq->cq_head;

	spin_lock(&cq->entry_lock);
	cq_entry(cq_head).command_id = command_id;
	cq_entry(cq_head).sq_id = sqid;
	cq_entry(cq_head).sq_head = sq_entry;
	cq_entry(cq_head).status = cq->phase | NVME_SC_SUCCESS << 1;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	spin_unlock(&cq->entry_lock);
}

extern unsigned long long completion_lag;
static unsigned long long __lag_cumul = 0;
static unsigned int __lag_cnt = 0;

static void __update_lag(unsigned long long target, unsigned long long now)
{
	__lag_cumul += (now - target);
	if (++__lag_cnt >= 10000) {
		unsigned long lag_current = __lag_cumul / __lag_cnt;

		NVMEV_DEBUG("Completion lag %llu --> %lu\n", completion_lag, lag_current);

		completion_lag = (completion_lag + 3 * lag_current) >> 2;
		__lag_cumul = 0;
		__lag_cnt = 0;
	}
}

static int nvmev_kthread_io(void *data)
{
	struct nvmev_proc_info *pi = (struct nvmev_proc_info *)data;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[33];
	static unsigned long long intr_counter[33];

	unsigned long long prev_clock;
#endif

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs = __get_wallclock();
		unsigned int curr_entry = pi->io_seq;
		struct nvmev_completion_queue *cq;
		struct nvmev_proc_table *proc_entry;
		int qidx;

		pi->proc_io_nsecs = curr_nsecs;
		while (curr_entry != -1) {
			proc_entry = &pi->proc_table[curr_entry];

			if (proc_entry->is_completed == true) {
				curr_entry = pi->proc_table[curr_entry].next;
				continue;
			}

			if (proc_entry->is_copied == false) {
#ifdef PERF_DEBUG
				unsigned long long memcpy_start = __get_wallclock();
				unsigned long long diff;
				proc_entry->nsecs_copy_start = memcpy_start;
#endif
				__do_perform_io(proc_entry->sqid, proc_entry->sq_entry);

#ifdef PERF_DEBUG
				proc_entry->nsecs_copy_done = __get_wallclock();
				diff = proc_entry->nsecs_copy_done - proc_entry->nsecs_copy_start;
#endif
				proc_entry->is_copied = true;

				NVMEV_DEBUG("%s: copied %u, %d %d %d\n",
						pi->thread_name, curr_entry,
						proc_entry->sqid, proc_entry->cqid, proc_entry->sq_entry,
				);
			}

			if (proc_entry->nsecs_target <= curr_nsecs + completion_lag) {
				__update_lag(proc_entry->nsecs_target, curr_nsecs + completion_lag);

				__fill_cq_result(proc_entry->sqid, proc_entry->cqid,
						proc_entry->sq_entry, proc_entry->command_id);

				NVMEV_DEBUG("%s: completed %u, %d %d %d\n",
						pi->thread_name, curr_entry,
						proc_entry->sqid, proc_entry->cqid, proc_entry->sq_entry);

#ifdef PERF_DEBUG
				proc_entry->nsecs_cq_filled = __get_wallclock();
				pr_info("%llu %llu %llu %llu %llu %llu\n",
						proc_entry->nsecs_start,
						proc_entry->nsecs_enqueue,
						proc_entry->nsecs_copy_start,
						proc_entry->nsecs_copy_done,
						proc_entry->nsecs_cq_filled,
						proc_entry->nsecs_target);
#endif
				proc_entry->is_completed = true;
				cq = vdev->cqes[proc_entry->cqid];
				cq->interrupt_ready = true;

				curr_entry = pi->proc_table[curr_entry].next;
			} else {
				NVMEV_DEBUG("%s =====> Entry: %u, %lld %lld %d\n",
						pi->thread_name, curr_entry, curr_nsecs,
						proc_entry->nsecs_target, proc_entry->is_completed);
				break;
			}
		}

		for (qidx = 1; qidx <= vdev->nr_cq; qidx++) {
			cq = vdev->cqes[qidx];
			if (cq == NULL || !cq->irq_enabled) continue;

			if (spin_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {

#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					__signal_cq_completion(qidx);

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						pr_info("Intr %d: %llu\n", qidx,
								intr_clock[qidx] / intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				spin_unlock(&cq->irq_lock);
			}
		}
		cond_resched();
	}

	return 0;
}

void NVMEV_IO_PROC_INIT(struct nvmev_dev* vdev)
{
	unsigned int i, proc_idx;

	vdev->proc_info = kcalloc(sizeof(struct nvmev_proc_info), vdev->config.nr_io_cpu, GFP_KERNEL);
	vdev->proc_turn = 0;

	for (proc_idx = 0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		struct nvmev_proc_info *pi = &vdev->proc_info[proc_idx];

		pi->proc_table = kzalloc(sizeof(struct nvmev_proc_table) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			pi->proc_table[i].next = i + 1;
			pi->proc_table[i].prev = i - 1;
		}
		pi->proc_table[NR_MAX_PARALLEL_IO - 1].next = -1;

		pi->free_seq = 0;
		pi->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		pi->io_seq = -1;
		pi->io_seq_end = -1;

		snprintf(pi->thread_name, sizeof(pi->thread_name), "nvmev_proc_io_%d", proc_idx);

		pi->nvmev_io_worker = kthread_create(nvmev_kthread_io, pi, pi->thread_name);

		kthread_bind(pi->nvmev_io_worker, vdev->config.cpu_nr_proc_io[proc_idx]);
		wake_up_process(pi->nvmev_io_worker);

		NVMEV_INFO("%s started at cpu %d\n",
				pi->thread_name, vdev->config.cpu_nr_proc_io[proc_idx]);
	}
}

void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev)
{
	unsigned int proc_idx;

	for (proc_idx = 0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		struct nvmev_proc_info *pi = &vdev->proc_info[proc_idx];

		if (!IS_ERR_OR_NULL(pi->nvmev_io_worker)) {
			kthread_stop(pi->nvmev_io_worker);
			pi->nvmev_io_worker = NULL;
		}

		kfree(pi->proc_table);
	}

	kfree(vdev->proc_info);
}
