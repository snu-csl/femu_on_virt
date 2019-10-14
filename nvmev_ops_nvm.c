#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include "nvmev.h"

#define PRP_PFN(x)	((unsigned long)((x) >> PAGE_SHIFT))

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

static inline unsigned long long __get_wallclock(void)
{
	return cpu_clock(vdev->config.cpu_nr_proc_reg);
}

#define UNIT_SIZE (1 << 20)
#define NR_UNITS 8

static unsigned long long schedule_units(int opcode, unsigned long lba, unsigned int length, unsigned long long nsecs_start)
{
	unsigned long start_unit = (lba << 9) % UNIT_SIZE;
	unsigned int nr_units = DIV_ROUND_UP(length, UNIT_SIZE);
	unsigned long long latest = nsecs_start;
	unsigned int latency = (opcode == nvme_cmd_write) ?
			vdev->config.write_latency : vdev->config.read_latency;
	int i;

	for (i = 0; i < nr_units; i++) {
		int unit = (start_unit + i)	% NR_UNITS;
		if (vdev->unit_stat[unit] < nsecs_start) {
			vdev->unit_stat[unit] = nsecs_start + latency;
		} else {
			vdev->unit_stat[unit] += latency;
		}
		latest = max(latest, vdev->unit_stat[unit]);
	}

	return latest;
}

unsigned long long elapsed_nsecs(int opcode, unsigned int length, unsigned long long nsecs_start)
{
	unsigned long long elapsed_nsecs = 0;
	int unit_seq = 0;
	int req_unit = DIV_ROUND_UP(length, 4096);
	int lowest_unit = 0;
	unsigned long long lowest_time = vdev->unit_stat[0];
	int i;

	for (unit_seq = 1; unit_seq < vdev->nr_unit; unit_seq++) {
		if (vdev->unit_stat[unit_seq] < lowest_time) {
			lowest_time = vdev->unit_stat[unit_seq];
			lowest_unit = unit_seq;
		}
	}

	for (i = 0; i < req_unit; i++) {
		unit_seq = (lowest_unit + i) % vdev->nr_unit;
		if (vdev->unit_stat[unit_seq] < nsecs_start) {
			vdev->unit_stat[unit_seq] = nsecs_start;
		}

		switch(opcode) {
			case nvme_cmd_write:
				vdev->unit_stat[unit_seq] += vdev->config.write_latency;
				break;
			case nvme_cmd_read:
				vdev->unit_stat[unit_seq] += vdev->config.read_latency;
				break;
			default:
				break;
		}

		if (elapsed_nsecs < vdev->unit_stat[unit_seq])
			elapsed_nsecs = vdev->unit_stat[unit_seq];
	}
	/*
	switch(opcode) {
		case nvme_cmd_write:
			elapsed_nsecs += vdev->config.write_latency;
			if (vdev->config.write_bw_us)
				elapsed_nsecs += (length / vdev->config.write_bw_us);
			break;
		case nvme_cmd_read:
			elapsed_nsecs += vdev->config.read_latency;
			if (vdev->config.read_bw_us)
				elapsed_nsecs += (length / vdev->config.read_bw_us);
			break;
		default:
			break;
	}
	*/
	return elapsed_nsecs;
}

unsigned int nvmev_storage_io(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	unsigned int length_bytes;

	length_bytes = (sq_entry(sq_entry).rw.length + 1) << 9;

	return length_bytes;
}

unsigned int nvmev_storage_memcpy(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	//unsigned long long start_bytes;
	unsigned long long io_offs;
	unsigned int mem_offs;
	unsigned int length_bytes;
	unsigned int remain_io;
	unsigned int io_size;
	unsigned int prp_offs = 0;
	unsigned int prp2_offs = 0;
	u64 paddr, paddr2;
	u64 *paddr_list = NULL;
	void* temp_ptr;
	void *vaddr;
	//int temp;

	io_offs = sq_entry(sq_entry).rw.slba << 9;
	length_bytes = (sq_entry(sq_entry).rw.length + 1) << 9;
	remain_io = length_bytes;
	//NVMEV_INFO("IO REQ: %llu %u\n", io_offs, length_bytes);
	while (remain_io) {
		prp_offs++;
		if (prp_offs == 1) {
			paddr = sq_entry(sq_entry).rw.prp1;
		} else if (prp_offs == 2) {
			if (remain_io <= 4096)
				paddr = sq_entry(sq_entry).rw.prp2;
			else {
				paddr2 = sq_entry(sq_entry).rw.prp2;
				temp_ptr = kmap_atomic_pfn(PRP_PFN(paddr2));
				temp_ptr+= (paddr2 & 0xFFF);
				paddr_list = temp_ptr;
				//NVMEV_INFO("PRP2 Addr: %llu\n", paddr2);
				paddr = paddr_list[prp2_offs];

				//NVMEV_INFO("PRP Offs %d: %llu\n", prp2_offs, paddr);

				prp2_offs++;
			}
		} else {
			paddr = paddr_list[prp2_offs];
			prp2_offs++;
		}

		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
		/*
		if (paddr & 0xFFF) {
			mem_offs = paddr & 0xFFF;
			NVMEV_DEBUG("Offset Mismatch =Remain_io = %u==============\n", remain_io);
			if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
				NVMEV_DEBUG("Write %llu->%p, %llu %u\n", paddr, vaddr, io_offs, remain_io);
			} else {
				NVMEV_DEBUG("Read %llu->%p, %llu %u\n", paddr, vaddr, io_offs, remain_io);
			}

			NVMEV_ERROR("PRP 1 0: 0x%llx %llx\n", paddr, paddr&0xFFF);
			if (paddr_list != NULL) {
				while (paddr_list[temp] != 0) {
					NVMEV_ERROR("PRP 2 %d: 0x%llx %llx\n", temp, paddr_list[temp], paddr_list[temp]&0xFFF);
					temp++;
				}
			}


		}
		*/
		if (remain_io > 4096)
			io_size = 4096;
		else
			io_size = remain_io;

		if (paddr & 0xFFF) {
			mem_offs = paddr & 0xFFF;
			if (io_size + mem_offs > 4096)
				io_size = 4096 - mem_offs;
		}
		if (sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
			// write
			memcpy(vdev->storage_mapped + io_offs, vaddr + mem_offs, io_size);
			//NVMEV_INFO("Write %llu->%p, %u %llu %u\n", paddr, vaddr + mem_offs, mem_offs, io_offs, io_size);
		} else {
			// read
			memcpy(vaddr + mem_offs, vdev->storage_mapped + io_offs, io_size);
			//NVMEV_INFO("Read %llu->%p, %u %llu %u\n", paddr, vaddr + mem_offs, mem_offs, io_offs, io_size);
		}

		kunmap_atomic(vaddr);

		remain_io -= io_size;
		io_offs += io_size;
		mem_offs = 0;
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length_bytes;
}

void nvmev_proc_flush(int sqid, int sq_entry)
{
#if ENABLE_DBG_PRINT
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	NVMEV_DEBUG("qid %d entry %d\n", sqid, sq_entry);
#endif
	return;
}

unsigned int nvmev_proc_write(int sqid, int sq_entry)
{
#if	ENABLE_DBG_PRINT
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	NVMEV_DEBUG("qid %d entry %d lba %llu length %d\n",
			sqid, sq_entry,
			sq_entry(sq_entry).rw.slba,
			sq_entry(sq_entry).rw.length);
#endif

	return nvmev_storage_io(sqid, sq_entry);
}

unsigned int nvmev_proc_read(int sqid, int sq_entry)
{
#if	ENABLE_DBG_PRINT
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	NVMEV_DEBUG("qid %d entry %d lba %llu length %d, %llu %llu\n",
			sqid, sq_entry,
			sq_entry(sq_entry).rw.slba,
			sq_entry(sq_entry).rw.length,
			sq_entry(sq_entry).rw.prp1,
			sq_entry(sq_entry).rw.prp2);
#endif

	return nvmev_storage_io(sqid, sq_entry);
}

void nvmev_proc_io_enqueue(int sqid, int cqid, int sq_entry,
		unsigned long long nsecs_start, unsigned long long nsecs_elapse)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	unsigned int proc_turn = vdev->proc_turn;
	struct nvmev_proc_info *proc_info = &vdev->proc_info[proc_turn++];

	unsigned long long nsecs_target = nsecs_elapse;
	unsigned int new_entry = proc_info->proc_free_seq;
	unsigned int curr_entry = -1;

	unsigned long long nsecs_enqueue = local_clock();

	if (proc_turn == vdev->config.nr_io_cpu) proc_turn = 0;
	vdev->proc_turn = proc_turn;

	proc_info->proc_free_seq = proc_info->proc_table[new_entry].next;

	NVMEV_DEBUG("New Entry %s->%u[%d], sq-%d cq-%d, entry-%d %llu + %llu\n",
			proc_info->thread_name,
			new_entry,
			sq_entry(sq_entry).rw.opcode,
			sqid, cqid, sq_entry, nsecs_start, nsecs_target - nsecs_start);

	/////////////////////////////////
	proc_info->proc_table[new_entry].sqid = sqid;
	proc_info->proc_table[new_entry].cqid= cqid;
	proc_info->proc_table[new_entry].sq_entry = sq_entry;
	proc_info->proc_table[new_entry].command_id = sq_entry(sq_entry).common.command_id;
	proc_info->proc_table[new_entry].nsecs_start = nsecs_start;
	proc_info->proc_table[new_entry].nsecs_enqueue = nsecs_enqueue;
	proc_info->proc_table[new_entry].nsecs_target = nsecs_target;
	proc_info->proc_table[new_entry].isProc = false;
	proc_info->proc_table[new_entry].isCpy = false;
	proc_info->proc_table[new_entry].next = -1;
	proc_info->proc_table[new_entry].prev = -1;

	// (END) -> (START) order, nsecs target ascending order
	if (proc_info->proc_io_seq == -1) {
		proc_info->proc_io_seq = new_entry;
		proc_info->proc_io_seq_end = new_entry;
		NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, proc_info->proc_io_seq);
	} else {
		curr_entry = proc_info->proc_io_seq_end;

		while (curr_entry != -1) {
			NVMEV_DEBUG("Move-> Current: %u, Target, %llu, ioproc: %llu, New target: %llu\n",
					curr_entry, proc_info->proc_table[curr_entry].nsecs_target,
					proc_info->proc_io_nsecs,
					nsecs_target);
			if (proc_info->proc_table[curr_entry].nsecs_target <= proc_info->proc_io_nsecs)
				break;

			if (proc_info->proc_table[curr_entry].nsecs_target <= nsecs_target)
				break;

			curr_entry = proc_info->proc_table[curr_entry].prev;
		}

		if (curr_entry == -1) {
			proc_info->proc_table[new_entry].next = proc_info->proc_io_seq;
			proc_info->proc_io_seq = new_entry;
			NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, proc_info->proc_io_seq);
		} else if (proc_info->proc_table[curr_entry].next == -1) {
			proc_info->proc_table[new_entry].prev = curr_entry;
			proc_info->proc_io_seq_end = new_entry;
			proc_info->proc_table[curr_entry].next = new_entry;
			NVMEV_DEBUG("Cur Entry : %u, New Entry : %u\n", curr_entry, new_entry);
		} else {
			proc_info->proc_table[new_entry].prev = curr_entry;
			proc_info->proc_table[new_entry].next = proc_info->proc_table[curr_entry].next;

			proc_info->proc_table[proc_info->proc_table[new_entry].next].prev = new_entry;
			proc_info->proc_table[curr_entry].next = new_entry;
			NVMEV_DEBUG("%u <- New Entry(%u) -> %u\n",
					proc_info->proc_table[new_entry].prev,
					new_entry,
					proc_info->proc_table[new_entry].next);
		}
	}
}

void nvmev_proc_io_cleanup(void)
{
	struct nvmev_proc_info *proc_info;
	struct nvmev_proc_table *proc_entry, *free_entry;
	unsigned int start_entry = -1, last_entry = -1;
	unsigned int curr_entry;
	unsigned int turn;

	for (turn = 0; turn < vdev->config.nr_io_cpu; turn++) {
		start_entry = -1;
		last_entry = -1;

		proc_info = &vdev->proc_info[turn];
		NVMEV_DEBUG("%s: Start\n", proc_info->thread_name);
		start_entry = proc_info->proc_io_seq;
		curr_entry = start_entry;
		NVMEV_DEBUG("Before Start: Start: %u Curr: %u\n", start_entry, curr_entry);
		while (curr_entry != -1) {
			proc_entry = &proc_info->proc_table[curr_entry];
			if (proc_entry->isProc == true && proc_entry->isCpy == true &&
					proc_entry->nsecs_target <= proc_info->proc_io_nsecs) {
				NVMEV_DEBUG("Cleanup Target : %d %d %d\n",
						curr_entry, proc_entry->isProc, proc_entry->isCpy);
				last_entry = curr_entry;
				curr_entry = proc_entry->next;
			} else
				break;
		}
		NVMEV_DEBUG("End Check : Start: %u Last : %u\n", start_entry, last_entry);

		if (start_entry != -1 && last_entry != -1) {
			proc_entry = &proc_info->proc_table[last_entry];
			proc_info->proc_io_seq = proc_entry->next;
			NVMEV_DEBUG("%u: PROC_NEXT_IO_SEQ = %u\n", __LINE__, proc_info->proc_io_seq);
			if (proc_entry->next != -1) {
				proc_entry = &proc_info->proc_table[proc_info->proc_io_seq];
				proc_entry->prev = -1;
			}

			proc_entry = &proc_info->proc_table[last_entry];
			proc_entry->next = -1;

			proc_entry = &proc_info->proc_table[start_entry];
			proc_entry->prev = proc_info->proc_free_last;

			free_entry = &proc_info->proc_table[proc_info->proc_free_last];
			free_entry->next = start_entry;

			proc_info->proc_free_last = last_entry;

			NVMEV_DEBUG("Cleanup %u -> %u\n", start_entry, last_entry);
		}
		NVMEV_DEBUG("%s: End\n", proc_info->thread_name);
	}

	return;
}

//void nvmev_proc_nvm(int sqid, int sq_entry, unsigned long long nsecs_start)
int nvmev_proc_nvm(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	unsigned long long nsecs_elapsed = 0;
	unsigned long long nsecs_start = local_clock();
	unsigned int io_len = 0;
#if PERF_DEBUG
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
		io_len = nvmev_proc_write(sqid, sq_entry);
		//nsecs_elapsed = elapsed_nsecs(nvme_cmd_write, io_len, nsecs_start);
		nsecs_elapsed = schedule_units(nvme_cmd_write,
					sq_entry(sq_entry).rw.slba, io_len, nsecs_start);
		break;
	case nvme_cmd_read:
		io_len = nvmev_proc_read(sqid, sq_entry);
		//nsecs_elapsed = elapsed_nsecs(nvme_cmd_read, io_len, nsecs_start);
		nsecs_elapsed = schedule_units(nvme_cmd_read,
					sq_entry(sq_entry).rw.slba, io_len, nsecs_start);
		break;
	case nvme_cmd_flush:
		nvmev_proc_flush(sqid, sq_entry);
		nsecs_elapsed = nsecs_start;
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

#if PERF_DEBUG
	prev_clock2 = local_clock();
#endif
	nvmev_proc_io_enqueue(sqid, sq->cqid, sq_entry,
			nsecs_start, nsecs_elapsed);
#if PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	nvmev_proc_io_cleanup();
#if PERF_DEBUG
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

void nvmev_proc_sq_io(int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;

	if (unlikely(num_proc < 0)) num_proc += sq->queue_size;
	if (unlikely(!sq)) return;

	for (seq = 0; seq < num_proc; seq++) {
		int io_size;

		io_size = nvmev_proc_nvm(sqid, sq_entry);

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

void nvmev_proc_cq_io(int cqid, int new_db, int old_db)
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
	struct cpumask *target = &vdev->first_cpu_on_node; /* SIgnal 0 by default */

	if (vdev->msix_enabled) {
		if (unlikely(!cq->irq_desc)) {
			struct msi_desc *msi_desc;
			for_each_msi_entry(msi_desc, (&vdev->pdev->dev)) {
				if (msi_desc->msi_attrib.entry_nr == cq->irq_vector)
					break;
			}
			cq->irq_desc = irq_to_desc(msi_desc->irq);
		}

#ifdef CONFIG_NUMA
		NVMEV_DEBUG("smphint(latest): %*pbl, vector:%u\n",
				cpumask_pr_args(cq->irq_desc->irq_common_data.affinity),
				cq->irq);
		NVMEV_DEBUG("Old: %u, New: %u\n", cq->irq,
			readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF);

		if (unlikely(!cpumask_subset(cq->irq_desc->irq_common_data.affinity,
						cpumask_of_node(vdev->pdev->dev.numa_node)))) {
			NVMEV_DEBUG("Invalid affinity, -> 0\n");
		} else {
			if (unlikely(cq->irq != (readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF))) {
				NVMEV_DEBUG("IRQ : %d -> %d\n", cq->irq, (readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF));
				cq->irq = (readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF);
			}
			if (unlikely(cpumask_equal(cq->irq_desc->irq_common_data.affinity, cpumask_of_node(vdev->pdev->dev.numa_node)))) {
				NVMEV_DEBUG("Every... -> 0\n");
			} else {
				NVMEV_DEBUG("Send to Target CPU\n");
				target = cq->irq_desc->irq_common_data.affinity;
			}
		}
#else
		NVMEV_DEBUG("Intr -> all, Vector->%u\n", cq->irq);
		target = cq->irq_desc->irq_common_data.affinity;
#endif
		apic->send_IPI_mask(target, cq->irq);
	} else {
		generateInterrupt(cq->irq);
	}
}

void fill_cq_result(int sqid, int cqid, int sq_entry, unsigned int command_id)
{
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head = cq->cq_head;

	cq_entry(cq_head).command_id = command_id;
	cq_entry(cq_head).sq_id = sqid;
	cq_entry(cq_head).sq_head = sq_entry;
	wmb();
	cq_entry(cq_head).status = cq->phase | NVME_SC_SUCCESS << 1;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
}

static int nvmev_kthread_io_proc(void *data)
{
	struct nvmev_proc_info *proc_info = (struct nvmev_proc_info *)data;

#if PERF_DEBUG
	static unsigned long long intr_clock[33];
	static unsigned long long intr_counter[33];

	unsigned long long prev_clock;
#endif

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs = __get_wallclock();
		unsigned int curr_entry = proc_info->proc_io_seq;
		struct nvmev_completion_queue *cq;
		struct nvmev_proc_table* proc_entry;
		int qidx;

		proc_info->proc_io_nsecs = curr_nsecs;
		while (curr_entry != -1) {
			proc_entry = &proc_info->proc_table[curr_entry];
			if (proc_entry->isProc == true) {
				curr_entry = proc_info->proc_table[curr_entry].next;
				continue;
			}

			if (proc_entry->isCpy == false) {
#if PERF_DEBUG
				unsigned long long memcpy_start = __get_wallclock();
				unsigned long long diff;
				proc_entry->nsecs_copy_start = memcpy_start;
#endif
				nvmev_storage_memcpy(proc_entry->sqid, proc_entry->sq_entry);

#if PERF_DEBUG
				proc_entry->nsecs_copy_done = __get_wallclock();
				diff = proc_entry->nsecs_copy_done - proc_entry->nsecs_copy_start;
#endif
				proc_entry->isCpy = true;
				NVMEV_DEBUG("%s proc Entry %u, %d %d, %d --> %d   COPY MEM\n",
						proc_info->thread_name,
						curr_entry,
						proc_entry->sqid,
						proc_entry->cqid,
						proc_entry->sq_entry,
						proc_info->proc_table[curr_entry].next
				);
			}

			if (proc_entry->nsecs_target <= curr_nsecs) {

				spin_lock(&vdev->cq_entry_lock[proc_entry->cqid]);
				fill_cq_result(proc_entry->sqid,
						proc_entry->cqid,
						proc_entry->sq_entry, proc_entry->command_id);
				spin_unlock(&vdev->cq_entry_lock[proc_entry->cqid]);

				NVMEV_DEBUG("%s proc Entry %u, %d %d, %d --> %d\n",
						proc_info->thread_name,
						curr_entry,
						proc_entry->sqid,
						proc_entry->cqid,
						proc_entry->sq_entry,
						proc_info->proc_table[curr_entry].next
						);

#if PERF_DEBUG
				proc_entry->nsecs_cq_filled = __get_wallclock();
				pr_info("%llu %llu %llu %llu %llu %llu\n",
						proc_entry->nsecs_start,
						proc_entry->nsecs_enqueue,
						proc_entry->nsecs_copy_start,
						proc_entry->nsecs_copy_done,
						proc_entry->nsecs_cq_filled,
						proc_entry->nsecs_target);
#endif
				proc_entry->isProc = true;
				cq = vdev->cqes[proc_entry->cqid];
				cq->interrupt_ready = true;

				curr_entry = proc_info->proc_table[curr_entry].next;
			} else {
				NVMEV_DEBUG("%s =====> Entry: %u, %lld %lld %d\n",
						proc_info->thread_name, curr_entry, curr_nsecs,
						proc_entry->nsecs_target, proc_entry->isProc);
				break;
			}
		}

		for (qidx = 1; qidx <= vdev->nr_cq; qidx++) {
			cq = vdev->cqes[qidx];
			if (cq == NULL) continue;
			if (!cq->interrupt_enabled) continue;
			if (spin_trylock(&vdev->cq_irq_lock[qidx])) {
				if (cq->interrupt_ready == true) {
#if PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					__signal_cq_completion(qidx);
#if PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 1000) {
						pr_info("Gen Intr Clock -> Q:%d, %llu\n", qidx,
								intr_clock[qidx]/intr_counter[qidx]);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
					NVMEV_DEBUG("Gen Interrupt %d\n", qidx);
				}

				spin_unlock(&vdev->cq_irq_lock[qidx]);
			}
		}
		cond_resched();
		//schedule_timeout(nsecs_to_jiffies(1));
	}

	return 0;
}

void NVMEV_IO_PROC_INIT(struct nvmev_dev* vdev)
{
	unsigned int i, proc_idx;
	struct nvmev_proc_info *proc_info;
	vdev->proc_info = kcalloc(sizeof(struct nvmev_proc_info), vdev->config.nr_io_cpu, GFP_KERNEL);
	vdev->proc_turn = 0;

	for (proc_idx = 0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		proc_info = &vdev->proc_info[proc_idx];

		proc_info->proc_table = kzalloc(sizeof(struct nvmev_proc_table) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			proc_info->proc_table[i].next = i + 1;
			proc_info->proc_table[i].prev = i - 1;
		}

		proc_info->proc_table[NR_MAX_PARALLEL_IO - 1].next = -1;

		proc_info->proc_free_seq = 0;
		proc_info->proc_free_last = NR_MAX_PARALLEL_IO - 1;
		proc_info->proc_io_seq = -1;

		proc_info->proc_io_nsecs = __get_wallclock();

		proc_info->thread_name = kzalloc(sizeof(char) * 32, GFP_KERNEL);

		sprintf(proc_info->thread_name, "nvmev_proc_io_%d", proc_idx);

		proc_info->nvmev_io_proc = kthread_create(nvmev_kthread_io_proc, proc_info, proc_info->thread_name);

		kthread_bind(proc_info->nvmev_io_proc, vdev->config.cpu_nr_proc_io[proc_idx]);
		wake_up_process(proc_info->nvmev_io_proc);

		pr_info("%s started at cpu %d\n",
				proc_info->thread_name, vdev->config.cpu_nr_proc_io[proc_idx]);
	}
}

void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev)
{
	unsigned int proc_idx;
	struct nvmev_proc_info *proc_info;

	for (proc_idx = 0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		proc_info = &vdev->proc_info[proc_idx];

		if (!IS_ERR_OR_NULL(proc_info->nvmev_io_proc)) {
			kthread_stop(proc_info->nvmev_io_proc);
			proc_info->nvmev_io_proc = NULL;

			kfree(proc_info->thread_name);
		}

		kfree(proc_info->proc_table);
	}

	kfree(vdev->proc_info);
}
