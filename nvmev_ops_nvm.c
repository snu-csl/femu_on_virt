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

long long int elapsed_usecs(int opcode, unsigned int length, long long int usecs_start) {
	long long int elapsed_usecs = 0;
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
		if(vdev->unit_stat[unit_seq] < usecs_start) {
			vdev->unit_stat[unit_seq]=usecs_start;
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
		
		if(elapsed_usecs < vdev->unit_stat[unit_seq])
			elapsed_usecs = vdev->unit_stat[unit_seq];
	}
	/*
	switch(opcode) {
		case nvme_cmd_write: 
			elapsed_usecs+=vdev->config.write_latency;
			if(vdev->config.write_bw_us)
				elapsed_usecs+=(length / vdev->config.write_bw_us);
			break;
		case nvme_cmd_read:
			elapsed_usecs+=vdev->config.read_latency;
			if(vdev->config.read_bw_us)
				elapsed_usecs+=(length / vdev->config.read_bw_us);
			break;
		default:
			break;
	}
	*/
	return elapsed_usecs;
}

unsigned int nvmev_storage_io(int sqid, int sq_entry) {
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
				io_size -= mem_offs;
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
		long long int usecs_start, long long int usecs_elapse) {
	//long long int usecs_target = usecs_start + usecs_elapse;
	long long int usecs_target = usecs_elapse;
	unsigned int new_entry = vdev->proc_free_seq;
	unsigned int curr_entry = -1;
#if	ENABLE_DBG_PRINT
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
#endif
	//long long int usecs_enqueue = ktime_to_us(ktime_get());
	long long int usecs_enqueue = cpu_clock(vdev->config.cpu_nr_proc_io) >> 10;
	//pr_info("%s:Entry %llu\n", __func__, cpu_clock(0));
	vdev->proc_free_seq = vdev->proc_table[new_entry].next;
	
	NVMEV_DEBUG("New Entry %u[%d], sq-%d cq-%d, entry-%d %lld->%lld\n", new_entry, 
			sq_entry(sq_entry).rw.opcode,
			sqid, cqid, sq_entry, usecs_start, usecs_target);
	
	vdev->proc_table[new_entry].sqid = sqid;
	vdev->proc_table[new_entry].cqid= cqid;
	vdev->proc_table[new_entry].sq_entry = sq_entry;
	vdev->proc_table[new_entry].usecs_start = usecs_start;
	vdev->proc_table[new_entry].usecs_enqueue = usecs_enqueue;
	vdev->proc_table[new_entry].usecs_target = usecs_target;
	vdev->proc_table[new_entry].isProc = false;
	vdev->proc_table[new_entry].next = -1;
	vdev->proc_table[new_entry].prev = -1;

	// (END) -> (START) order, usecs target ascending order
	if(vdev->proc_io_seq == -1) {
		vdev->proc_io_seq = new_entry;
		vdev->proc_io_seq_end = new_entry;
		NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, vdev->proc_io_seq);
	}
	else {
		curr_entry = vdev->proc_io_seq_end;

		while(curr_entry != -1) {
			NVMEV_DEBUG("Move-> Current: %u, Target, %llu, ioproc: %llu, New target: %llu\n",
					curr_entry, vdev->proc_table[curr_entry].usecs_target,
					vdev->proc_io_usecs,
					usecs_target);
			if(vdev->proc_table[curr_entry].usecs_target <= vdev->proc_io_usecs)
				break;

			if(vdev->proc_table[curr_entry].usecs_target <= usecs_target)
				break;

			curr_entry = vdev->proc_table[curr_entry].prev;
		}

		if(curr_entry == -1) {
			vdev->proc_table[new_entry].next = vdev->proc_io_seq;
			vdev->proc_io_seq = new_entry;
			NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, vdev->proc_io_seq);
		}
		else if(vdev->proc_table[curr_entry].next == -1) {
			vdev->proc_table[new_entry].prev = curr_entry;
			vdev->proc_io_seq_end = new_entry;
			vdev->proc_table[curr_entry].next = new_entry;
			NVMEV_DEBUG("Cur Entry : %u, New Entry : %u\n", curr_entry, new_entry);
		}
		else {
			vdev->proc_table[new_entry].prev = curr_entry;
			vdev->proc_table[new_entry].next = vdev->proc_table[curr_entry].next;

			vdev->proc_table[vdev->proc_table[new_entry].next].prev = new_entry;
			vdev->proc_table[curr_entry].next = new_entry;
			NVMEV_DEBUG("%u <- New Entry(%u) -> %u\n", 
					vdev->proc_table[new_entry].prev,
					new_entry,
					vdev->proc_table[new_entry].next);
		}
	}
}

void nvmev_proc_io_cleanup(void) {
	struct nvmev_proc_table *proc_entry, *free_entry;
	unsigned int start_entry = -1, last_entry = -1;
	unsigned int curr_entry;

	start_entry = vdev->proc_io_seq;
	curr_entry = start_entry;

	while(curr_entry != -1) {
		proc_entry = &vdev->proc_table[curr_entry];
		if(proc_entry->isProc == true &&
				proc_entry->usecs_target <= vdev->proc_io_usecs) {
			last_entry = curr_entry;
			curr_entry = proc_entry->next;
		}
		else
			break;
	}

	if(start_entry != -1 && last_entry != -1) {
		proc_entry = &vdev->proc_table[last_entry];
		vdev->proc_io_seq = proc_entry->next;
		NVMEV_DEBUG("%u: PROC_IO_SEQ = %u\n", __LINE__, vdev->proc_io_seq);
		if(proc_entry->next != -1) {
			proc_entry = &vdev->proc_table[vdev->proc_io_seq];
			proc_entry->prev = -1;
		}

		proc_entry = &vdev->proc_table[last_entry];
		proc_entry->next = -1;

		proc_entry = &vdev->proc_table[start_entry];
		proc_entry->prev = vdev->proc_free_last;

		free_entry = &vdev->proc_table[vdev->proc_free_last];
		free_entry->next = start_entry;

		vdev->proc_free_last = last_entry;

		NVMEV_DEBUG("Cleanup %u -> %u\n", start_entry, last_entry);
	}

	return;
}

//void nvmev_proc_nvm(int sqid, int sq_entry, long long int usecs_start) {
void nvmev_proc_nvm(int sqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	long long int usecs_elapsed = 0;
	//long long int usecs_start = ktime_to_us(ktime_get());
	long long int usecs_start = cpu_clock(vdev->config.cpu_nr_proc_io) >> 10;
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
			usecs_elapsed = usecs_start;
			break;
		case nvme_cmd_write: 
			io_len = nvmev_proc_write(sqid, sq_entry);
			usecs_elapsed = elapsed_usecs(nvme_cmd_write, io_len, usecs_start);
			break;
		case nvme_cmd_read:
			io_len = nvmev_proc_read(sqid, sq_entry);
			usecs_elapsed = elapsed_usecs(nvme_cmd_read, io_len, usecs_start);
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
			usecs_start, usecs_elapsed);
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
	//long long int usecs_start = ktime_to_us(ktime_get());

	if(unlikely(num_proc < 0)) num_proc+=sq->queue_size;
	if(unlikely(!sq)) return;

	for(seq = 0; seq < num_proc; seq++) {
		//nvmev_proc_nvm(sqid, sq_entry, usecs_start);
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

void fill_cq_result(int sqid, int cqid, int sq_entry) {
	struct nvmev_submission_queue *sq = vdev->sqes[sqid];
	struct nvmev_completion_queue *cq = vdev->cqes[cqid];
	int cq_head = cq->cq_head;

	cq_entry(cq_head).command_id = sq_entry(sq_entry).rw.command_id;
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
	long long int curr_usecs; 
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
		//curr_usecs = ktime_to_us(ktime_get());
		curr_usecs = local_clock() >> 10;
		vdev->proc_io_usecs = curr_usecs;
		curr_entry = vdev->proc_io_seq;
		while(curr_entry != -1) {
			proc_entry = &vdev->proc_table[curr_entry];
			if(proc_entry->isProc == false && 
					proc_entry->usecs_target <= curr_usecs) {

#if PERF_DEBUG
				elapsed_nr ++;
				elapsed+=(curr_usecs - proc_entry->usecs_start);

				if(elapsed_nr>1000) {
					pr_info("Elapsed time -> %llu\n", elapsed/elapsed_nr);
					elapsed_nr = 0;
					elapsed = 0;
				}
#endif

				NVMEV_DEBUG("(%llu): %llu -> %llu -> %llu\n",  curr_usecs, \
						proc_entry->usecs_start, \
						proc_entry->usecs_enqueue, \
						proc_entry->usecs_target);
			//pr_info("%s:Out %llu\n", __func__, cpu_clock(0));
				fill_cq_result(proc_entry->sqid,
						proc_entry->cqid,
						proc_entry->sq_entry);
				NVMEV_DEBUG("proc Entry %u, %d %d, %d --> %d\n", curr_entry,  \
						proc_entry->sqid, \
						proc_entry->cqid, \
						proc_entry->sq_entry, \
						vdev->proc_table[curr_entry].next \
						);
				
				proc_entry->isProc = true;
				cq = vdev->cqes[proc_entry->cqid];
				cq->interrupt_ready = true;
			
				curr_entry = vdev->proc_table[curr_entry].next;
			}
			else if(proc_entry->isProc == true) {
				NVMEV_DEBUG("Move to Next %u %d\n", curr_entry, vdev->proc_table[curr_entry].next);
				curr_entry = vdev->proc_table[curr_entry].next;
			}
			else {
				NVMEV_DEBUG("=====> Entry: %u, %lld %lld %d\n", curr_entry, curr_usecs, proc_entry->usecs_target, proc_entry->isProc);
				break;
			}
		}

		for(qidx=1; qidx <= vdev->nr_cq; qidx++) {
			cq = vdev->cqes[qidx];
			if(cq == NULL) continue;
			if(!cq->interrupt_enabled) continue;
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
		}
		cond_resched();
		//schedule_timeout(nsecs_to_jiffies(1));
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
	vdev->proc_free_last = (NR_MAX_IO_QUEUE*NR_MAX_PARALLEL_IO)-1;
	vdev->proc_io_seq = -1;

	//vdev->proc_io_usecs = ktime_to_us(ktime_get());
	vdev->proc_io_usecs = cpu_clock(vdev->config.cpu_nr_proc_io) >> 10;

	vdev->nvmev_io_proc = kthread_create(nvmev_kthread_io_proc, 
			NULL, "nvmev_proc_io");

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
