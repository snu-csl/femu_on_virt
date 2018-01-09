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

long long int elapsed_nsecs(int opcode, unsigned int length, long long int nsecs_start) {
	long long int elapsed_nsecs = 0;
	int unit_seq = 0;
	int req_unit = length / 4096 + !!(length % 4096);
	int lowest_unit = 0;
	long long int lowest_time = vdev->unit_stat[0];
	int i;

	for(unit_seq = 1; unit_seq < vdev->nr_unit; unit_seq++) {
		if(vdev->unit_stat[unit_seq] < lowest_time) {
			lowest_time = vdev->unit_stat[unit_seq];
			lowest_unit = unit_seq;
		}
	}

	for(i=0; i<req_unit; i++) {
		unit_seq = (lowest_unit + i) % vdev->nr_unit;
		if(vdev->unit_stat[unit_seq] < nsecs_start) {
			vdev->unit_stat[unit_seq]=nsecs_start;
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
		
		if(elapsed_nsecs < vdev->unit_stat[unit_seq])
			elapsed_nsecs = vdev->unit_stat[unit_seq];
	}
	/*
	switch(opcode) {
		case nvme_cmd_write: 
			elapsed_nsecs+=vdev->config.write_latency;
			if(vdev->config.write_bw_us)
				elapsed_nsecs+=(length / vdev->config.write_bw_us);
			break;
		case nvme_cmd_read:
			elapsed_nsecs+=vdev->config.read_latency;
			if(vdev->config.read_bw_us)
				elapsed_nsecs+=(length / vdev->config.read_bw_us);
			break;
		default:
			break;
	}
	*/
	return elapsed_nsecs;
}

unsigned int nvmev_storage_io(int sqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	unsigned int length_bytes;
	
	length_bytes = (sq_entry(sq_entry).rw.length + 1) << 9;

	return length_bytes;
}

unsigned int nvmev_storage_memcpy(int sqid, int sq_entry) {
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
	while(remain_io) {
		prp_offs++;
		if(prp_offs == 1) {
			paddr = sq_entry(sq_entry).rw.prp1;
		}
		else if(prp_offs == 2) {
			if(remain_io <= 4096)
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
		}
		else {
			paddr = paddr_list[prp2_offs];
			prp2_offs++;
		}

		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
		/*
		if(paddr & 0xFFF) {
			mem_offs = paddr & 0xFFF;
			NVMEV_DEBUG("Offset Mismatch =Remain_io = %u==============\n", remain_io);
			if(sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
				NVMEV_DEBUG("Write %llu->%p, %llu %u\n", paddr, vaddr, io_offs, remain_io);
			}
			else {
				NVMEV_DEBUG("Read %llu->%p, %llu %u\n", paddr, vaddr, io_offs, remain_io);
			}
			
			NVMEV_ERROR("PRP 1 0: 0x%llx %llx\n", paddr, paddr&0xFFF);
			if(paddr_list != NULL) {
				while(paddr_list[temp] != 0) {
					NVMEV_ERROR("PRP 2 %d: 0x%llx %llx\n", temp, paddr_list[temp], paddr_list[temp]&0xFFF);
					temp++;
				}
			}
			
			
		}
		*/
		if(remain_io > 4096) io_size = 4096;
		else io_size = remain_io;
		
		if(paddr & 0xFFF) {
			mem_offs = paddr & 0xFFF;
			if(io_size + mem_offs > 4096)
				io_size = 4096 - mem_offs;
		}
		if(sq_entry(sq_entry).rw.opcode == nvme_cmd_write) {
			// write
			memcpy(vdev->storage_mapped + io_offs, vaddr + mem_offs, io_size);
			//NVMEV_INFO("Write %llu->%p, %u %llu %u\n", paddr, vaddr + mem_offs, mem_offs, io_offs, io_size);
		}
		else {
			// read
			memcpy(vaddr + mem_offs, vdev->storage_mapped + io_offs, io_size);
			//NVMEV_INFO("Read %llu->%p, %u %llu %u\n", paddr, vaddr + mem_offs, mem_offs, io_offs, io_size);
		}

		kunmap_atomic(vaddr);

		remain_io-=io_size;
		io_offs+=io_size;
		mem_offs = 0;
	}

	if(paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length_bytes;
}

void nvmev_proc_flush(int sqid, int sq_entry) {
	return;
}

unsigned int nvmev_proc_write(int sqid, int sq_entry) {
#if	ENABLE_DBG_PRINT
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
#endif
	//LBA, LENGTH
	//sq_entry(sq_entry).rw.slba;
	//sq_entry(sq_entry).rw.length;
	//sq_entry(sq_entry).rw.prp1;
	//sq_entry(sq_entry).rw.prp2;
	
	NVMEV_DEBUG("qid %d entry %d lba %llu length %d\n", \
			sqid, sq_entry, \
			sq_entry(sq_entry).rw.slba, \
			sq_entry(sq_entry).rw.length);
	
	return nvmev_storage_io(sqid, sq_entry); 
}

unsigned int nvmev_proc_read(int sqid, int sq_entry) {
#if	ENABLE_DBG_PRINT
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
#endif

	//LBA, LENGTH
	//sq_entry(sq_entry).rw.slba;
	//sq_entry(sq_entry).rw.length;
	//sq_entry(sq_entry).rw.prp1;
	//sq_entry(sq_entry).rw.prp2;
	
	NVMEV_DEBUG("qid %d entry %d lba %llu length %d, %llu %llu\n", \
			sqid, sq_entry, \
			sq_entry(sq_entry).rw.slba, \
			sq_entry(sq_entry).rw.length,
			sq_entry(sq_entry).rw.prp1,
			sq_entry(sq_entry).rw.prp2);
	
	return nvmev_storage_io(sqid, sq_entry); 
}

void nvmev_proc_io_enqueue(int sqid, int cqid, int sq_entry,
		long long int nsecs_start, long long int nsecs_elapse) {
	
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];

	unsigned int proc_turn = vdev->proc_turn;
	struct nvmev_proc_info *proc_info = &vdev->proc_info[proc_turn++];

	long long int nsecs_target = nsecs_elapse;
	unsigned int new_entry = proc_info->proc_free_seq;
	unsigned int curr_entry = -1;

	long long int nsecs_enqueue = cpu_clock(vdev->config.cpu_nr_proc_reg);

	if(proc_turn == vdev->config.nr_io_cpu) proc_turn = 0;
	vdev->proc_turn = proc_turn;

	proc_info->proc_free_seq = proc_info->proc_table[new_entry].next;
	
	NVMEV_DEBUG("New Entry %u[%d], sq-%d cq-%d, entry-%d %lld->%lld\n", new_entry, 
			sq_entry(sq_entry).rw.opcode,
			sqid, cqid, sq_entry, nsecs_start, nsecs_target);
	
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
	if(proc_info->proc_io_seq == -1) {
		proc_info->proc_io_seq = new_entry;
		proc_info->proc_io_seq_end = new_entry;
		NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, proc_info->proc_io_seq);
	}
	else {
		curr_entry = proc_info->proc_io_seq_end;

		while(curr_entry != -1) {
			NVMEV_DEBUG("Move-> Current: %u, Target, %llu, ioproc: %llu, New target: %llu\n",
					curr_entry, proc_info->proc_table[curr_entry].nsecs_target,
					proc_info->proc_io_nsecs,
					nsecs_target);
			if(proc_info->proc_table[curr_entry].nsecs_target <= proc_info->proc_io_nsecs)
				break;

			if(proc_info->proc_table[curr_entry].nsecs_target <= nsecs_target)
				break;

			curr_entry = proc_info->proc_table[curr_entry].prev;
		}

		if(curr_entry == -1) {
			proc_info->proc_table[new_entry].next = proc_info->proc_io_seq;
			proc_info->proc_io_seq = new_entry;
			NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, proc_info->proc_io_seq);
		}
		else if(proc_info->proc_table[curr_entry].next == -1) {
			proc_info->proc_table[new_entry].prev = curr_entry;
			proc_info->proc_io_seq_end = new_entry;
			proc_info->proc_table[curr_entry].next = new_entry;
			NVMEV_DEBUG("Cur Entry : %u, New Entry : %u\n", curr_entry, new_entry);
		}
		else {
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

void nvmev_proc_io_cleanup(void) {
	struct nvmev_proc_info *proc_info;
	struct nvmev_proc_table *proc_entry, *free_entry;
	unsigned int start_entry = -1, last_entry = -1;
	unsigned int curr_entry;
	unsigned int turn;

	for(turn = 0; turn<vdev->config.nr_io_cpu; turn++) {
		proc_info = &vdev->proc_info[turn];

		start_entry = proc_info->proc_io_seq;
		curr_entry = start_entry;

		while(curr_entry != -1) {
			proc_entry = &proc_info->proc_table[curr_entry];
			if(proc_entry->isProc == true &&
					proc_entry->nsecs_target <= proc_info->proc_io_nsecs) {
				last_entry = curr_entry;
				curr_entry = proc_entry->next;
			}
			else
				break;
		}

		if(start_entry != -1 && last_entry != -1) {
			proc_entry = &proc_info->proc_table[last_entry];
			proc_info->proc_io_seq = proc_entry->next;
			NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, proc_info->proc_io_seq);
			if(proc_entry->next != -1) {
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
	}

	return;
}

//void nvmev_proc_nvm(int sqid, int sq_entry, long long int nsecs_start) {
void nvmev_proc_nvm(int sqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	long long int nsecs_elapsed = 0;
	//long long int nsecs_start = ktime_to_us(ktime_get());
	long long int nsecs_start = cpu_clock(vdev->config.cpu_nr_proc_reg);
	unsigned int io_len;
#if PERF_DEBUG
	unsigned long long prev_clock = 0;
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
	
	prev_clock = local_clock();
#endif
	switch(sq_entry(sq_entry).common.opcode) {
		case nvme_cmd_flush:
			nvmev_proc_flush(sqid, sq_entry);
			nsecs_elapsed = nsecs_start;
			break;
		case nvme_cmd_write: 
			io_len = nvmev_proc_write(sqid, sq_entry);
			nsecs_elapsed = elapsed_nsecs(nvme_cmd_write, io_len, nsecs_start);
			break;
		case nvme_cmd_read:
			io_len = nvmev_proc_read(sqid, sq_entry);
			nsecs_elapsed = elapsed_nsecs(nvme_cmd_read, io_len, nsecs_start);
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

	clock1+= (prev_clock2 - prev_clock);
	clock2+= (prev_clock3 - prev_clock2);
	clock3+= (prev_clock4 - prev_clock3);
	counter++;

	if(counter > 2000) {
		pr_info("MEM: %llu, ENQ: %llu, CLN: %llu\n", 
				clock1/counter, clock2/counter, clock3/counter);
		clock1=0;
		clock2=0;
		clock3=0;
		counter = 0;
	}
#endif
}

void nvmev_proc_sq_io(int sqid, int new_db, int old_db) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	//long long int nsecs_start = ktime_to_us(ktime_get());

	if(unlikely(num_proc < 0)) num_proc+=sq->queue_size;
	if(unlikely(!sq)) return;

	for(seq = 0; seq < num_proc; seq++) {
		//nvmev_proc_nvm(sqid, sq_entry, nsecs_start);
#if PERF_DEBUG
		pr_info("%s:Entry %llu\n", __func__, cpu_clock(0));
#endif
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
	struct msi_desc *msi_desc;

	if(vdev->msix_enabled) {
		if(unlikely(!cq->irq_desc)) {
			for_each_msi_entry(msi_desc, (&vdev->pdev->dev)) {
				if(msi_desc->msi_attrib.entry_nr == cq->irq_vector)
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

		if(unlikely(!cpumask_subset(cq->irq_desc->irq_common_data.affinity,
						cpumask_of_node(vdev->pdev->dev.numa_node)))) {
			//apic->send_IPI_mask(cpumask_of_node(vdev->pdev->dev.numa_node),
			NVMEV_DEBUG("Invalid affinity, -> 0\n");
			apic->send_IPI_mask(&vdev->first_cpu_on_node, cq->irq);
		}
		else {
			/*
			if(IS_ERR_OR_NULL(per_cpu(vector_irq, cpumask_first(cq->irq_desc->irq_common_data.affinity))[cq->irq])) {
				NVMEV_INFO("NULL\n");
				apic->send_IPI_mask(cpumask_of_node(vdev->pdev->dev.numa_node),
					cq->irq);
			}
			else {
			*/
			/*
			if(cq->irq != (readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF)) {
				NVMEV_ERROR("2nd IPI %u\n", readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF);
				apic->send_IPI_mask(cq->irq_desc->irq_common_data.affinity,
					readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF);
			}
			else {
				NVMEV_ERROR("1st IPI %u\n", cq->irq);
				apic->send_IPI_mask(cq->irq_desc->irq_common_data.affinity,
					cq->irq);
			}
			*/
			if(unlikely(cq->irq != (readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF))) {
				NVMEV_DEBUG("IRQ : %d -> %d\n", cq->irq, (readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF));
				cq->irq = (readl(vdev->msix_table + (PCI_MSIX_ENTRY_SIZE * cq->irq_vector) + PCI_MSIX_ENTRY_DATA) & 0xFF);
			}
			if(unlikely(cpumask_equal(cq->irq_desc->irq_common_data.affinity, cpumask_of_node(vdev->pdev->dev.numa_node)))) {
				NVMEV_DEBUG("Every... -> 0\n");
				apic->send_IPI_mask(&vdev->first_cpu_on_node, cq->irq);
			}
			else {
				NVMEV_DEBUG("Send to Target CPU\n");
				apic->send_IPI_mask(cq->irq_desc->irq_common_data.affinity, cq->irq);
			}

			
			//}
		}
#else
		NVMEV_DEBUG("Intr -> all, Vector->%u\n", cq->irq);

		apic->send_IPI_mask(cq->irq_desc->irq_common_data.affinity, cq->irq);
#endif
	}
	else {
		generateInterrupt(cq->irq);
	}
}

void fill_cq_result(int sqid, int cqid, int sq_entry, unsigned int command_id) {
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head = cq->cq_head;

	cq_entry(cq_head).command_id = command_id;
	cq_entry(cq_head).sq_id = sqid;
	cq_entry(cq_head).sq_head = sq_entry;
	wmb();
	cq_entry(cq_head).status = cq->phase | NVME_SC_SUCCESS << 1;

	if(++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
}

static int nvmev_kthread_io_proc(void *data)
{
	struct nvmev_proc_info *proc_info = (struct nvmev_proc_info *)data;
	long long int curr_nsecs; 
	unsigned int curr_entry;
	struct nvmev_completion_queue *cq;// = vdev->cqes[cqid];
	struct nvmev_proc_table* proc_entry;
	int qidx;
	
#if PERF_DEBUG
	static unsigned long long elapsed = 0;
	static unsigned long long elapsed_nr = 0;

	static unsigned long long intr_clock[33];
	static unsigned long long intr_counter[33];

	unsigned long long prev_clock;
#endif

	while(!kthread_should_stop()) {
		//curr_nsecs = ktime_to_us(ktime_get());
		curr_nsecs = cpu_clock(vdev->config.cpu_nr_proc_reg); //local_clock();
		proc_info->proc_io_nsecs = curr_nsecs;
		curr_entry = proc_info->proc_io_seq;
		while(curr_entry != -1) {
			proc_entry = &proc_info->proc_table[curr_entry];
			if(proc_entry->isProc == false && proc_entry->isCpy == false) {
				nvmev_storage_memcpy(proc_entry->sqid, proc_entry->sq_entry);
				proc_entry->isCpy = true;
				NVMEV_DEBUG("proc Entry %u, %d %d, %d --> %d   COPY MEM\n", curr_entry,  \
						proc_entry->sqid, \
						proc_entry->cqid, \
						proc_entry->sq_entry, \
						proc_info->proc_table[curr_entry].next \
				);
			}
			if(proc_entry->isProc == false && proc_entry->isCpy == true &&
					proc_entry->nsecs_target <= curr_nsecs) {
#if PERF_DEBUG
				elapsed_nr ++;
				elapsed+=(curr_nsecs - proc_entry->nsecs_start);

				if(elapsed_nr>1000) {
					pr_info("Elapsed time -> %llu\n", elapsed/elapsed_nr);
					elapsed_nr = 0;
					elapsed = 0;
				}
#endif

				NVMEV_DEBUG("(%llu): %llu -> %llu -> %llu\n",  curr_nsecs, \
						proc_entry->nsecs_start, \
						proc_entry->nsecs_enqueue, \
						proc_entry->nsecs_target);
			//pr_info("%s:Out %llu\n", __func__, cpu_clock(0));
				
				spin_lock(&vdev->cq_entry_lock[proc_entry->cqid]);
				fill_cq_result(proc_entry->sqid,
						proc_entry->cqid,
						proc_entry->sq_entry, proc_entry->command_id);
				spin_unlock(&vdev->cq_entry_lock[proc_entry->cqid]);

				NVMEV_DEBUG("proc Entry %u, %d %d, %d --> %d\n", curr_entry,  \
						proc_entry->sqid, \
						proc_entry->cqid, \
						proc_entry->sq_entry, \
						proc_info->proc_table[curr_entry].next \
						);
				
				proc_entry->isProc = true;
				cq = vdev->cqes[proc_entry->cqid];
				cq->interrupt_ready = true;
			
				curr_entry = proc_info->proc_table[curr_entry].next;
			}
			else if(proc_entry->isProc == true) {
				//NVMEV_DEBUG("Move to Next %u %d\n", curr_entry, proc_info->proc_table[curr_entry].next);
				curr_entry = proc_info->proc_table[curr_entry].next;
			}
			else {
				NVMEV_DEBUG("=====> Entry: %u, %lld %lld %d\n", curr_entry, curr_nsecs, proc_entry->nsecs_target, proc_entry->isProc);
				break;
			}
		}

		for(qidx=1; qidx <= vdev->nr_cq; qidx++) {
			cq = vdev->cqes[qidx];
			if(cq == NULL) continue;
			if(!cq->interrupt_enabled) continue;
			if(spin_trylock(&vdev->cq_irq_lock[qidx])) {
				if(cq->interrupt_ready == true) {
#if PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_intr_issue(qidx);
#if PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if(intr_counter[qidx] > 1000) {
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

void NVMEV_IO_PROC_INIT(struct nvmev_dev* vdev) {
	unsigned int i, proc_idx;
	struct nvmev_proc_info *proc_info;
	vdev->proc_info = kcalloc(sizeof(struct nvmev_proc_info), vdev->config.nr_io_cpu, GFP_KERNEL);
	vdev->proc_turn = 0;

	for(proc_idx=0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		proc_info = &vdev->proc_info[proc_idx];

		proc_info->proc_table = kzalloc(sizeof(struct nvmev_proc_table) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for(i=0; i<(NR_MAX_PARALLEL_IO); i++) {
			proc_info->proc_table[i].next = i+1;
			proc_info->proc_table[i].prev = i-1;
		}

		proc_info->proc_table[(NR_MAX_PARALLEL_IO)-1].next = -1;

		proc_info->proc_free_seq = 0;
		proc_info->proc_free_last = (NR_MAX_PARALLEL_IO)-1;
		proc_info->proc_io_seq = -1;

		proc_info->proc_io_nsecs = cpu_clock(vdev->config.cpu_nr_proc_reg);

		proc_info->thread_name = kzalloc(sizeof(char) * 16, GFP_KERNEL);

		sprintf(proc_info->thread_name, "nvmev_proc_io_%d", proc_idx);

		proc_info->nvmev_io_proc = kthread_create(nvmev_kthread_io_proc, proc_info, proc_info->thread_name);

		kthread_bind(proc_info->nvmev_io_proc, vdev->config.cpu_nr_proc_io[proc_idx]);
		wake_up_process(proc_info->nvmev_io_proc);
	}

	for(proc_idx=0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		proc_info = &vdev->proc_info[proc_idx];
		pr_err("%s start\n", proc_info->thread_name);
		wake_up_process(proc_info->nvmev_io_proc);
	}
}

void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev) {
	unsigned int proc_idx;
	struct nvmev_proc_info *proc_info;
	
	for(proc_idx=0; proc_idx < vdev->config.nr_io_cpu; proc_idx++) {
		proc_info = &vdev->proc_info[proc_idx];
		
		if(!IS_ERR_OR_NULL(proc_info->nvmev_io_proc)) {
			kthread_stop(proc_info->nvmev_io_proc);
			proc_info->nvmev_io_proc = NULL;

			kfree(proc_info->thread_name);
		}

		kfree(proc_info->proc_table);
	}

	kfree(vdev->proc_info);
}
