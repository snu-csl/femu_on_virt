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

#ifndef _LIB_NVMEV_H
#define _LIB_NVMEV_H

#include <linux/pci.h>
#include <linux/msi.h>
#include <asm/apic.h>

#include "nvme.h"

#undef CONFIG_NVMEV_DEBUG_VERBOSE

#define SUPPORT_ZNS 0
#define SUPPORT_MULTI_IO_WORKER_BY_SQ	1 
#define SUPPORT_VIRTUAL_CAPACITY		0

#if SUPPORT_ZNS == 0
#define NS_CSI NVME_CSI_NVM
#else
#define NS_CSI NVME_CSI_ZNS
#endif 

#define NVMEV_DRV_NAME "NVMeVirt"

#define NVMEV_INFO(string, args...) \
	printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ERROR(string, args...) \
	printk(KERN_ERR "%s: " string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ASSERT(x) \
	if (!(x)) printk(KERN_ERR "%s: FEMU ASSERT at line %d, (%s)\n", NVMEV_DRV_NAME, __LINE__, #x)

#ifdef CONFIG_NVMEV_DEBUG_VERBOSE
#define NVMEV_DEBUG(string, args...) \
	printk(KERN_INFO "%s: " string, NVMEV_DRV_NAME, ##args)
#else
#define NVMEV_DEBUG(string, args...) 
#endif

#ifdef CONFIG_NVMEV_DEBUG_JAEHOON
#define NVMEV_JH(string, args...) \
	printk(KERN_INFO "%s: [JH_LOG]" string, NVMEV_DRV_NAME, ##args)
#else
#define NVMEV_JH(string, args...)
#endif

#define NR_MAX_IO_QUEUE 72
#define NR_MAX_PARALLEL_IO 16384

#define PAGE_OFFSET_MASK (PAGE_SIZE - 1)

#define KB(k) ((k) * 1024)
#define MB(m) (KB((m) * 1024))

#define BYTE_TO_KB(b) ((b) / 1024)
#define BYTE_TO_MB(b) ((b) / 1024 / 1024)
#define BYTE_TO_GB(b) ((b) / 1024 / 1024 / 1024)

#define MS_PER_SEC(s)	((s) * 1000)
#define US_PER_SEC(s)	(MS_PER_SEC(s) * 1000)
#define NS_PER_SEC(s)	(US_PER_SEC(s) * 1000)	

#define LBA_TO_BYTE(lba) ((lba) << 9)
#define BYTE_TO_LBA(byte) ((byte) >> 9)

#define LPN_TO_BYTE(lpn) ((lpn) << 12)
#define BYTE_TO_LPN(byte) ((byte) >> 12)


#define LBA_TO_LPN(lba) (BYTE_TO_LPN(LBA_TO_BYTE(lba)))

#define INVALID32 (0xFFFFFFFF)
#define INVALID64 (0xFFFFFFFFFFFFFFFF)
#define ASSERT(X)

struct nvmev_sq_stat {
	unsigned int nr_dispatched;
	unsigned int nr_dispatch;
	unsigned int nr_in_flight;
	unsigned int max_nr_in_flight;
	unsigned long long total_io;
};

struct nvmev_submission_queue {
	int qid;
	int cqid;
	int sq_priority;
	bool phys_contig;

	int queue_size;

	struct nvmev_sq_stat stat;

	struct nvme_command __iomem **sq;
};

struct nvmev_completion_queue {
	int qid;
	int irq_vector;
	bool irq_enabled;
	bool interrupt_ready;
	bool phys_contig;

	spinlock_t entry_lock;
	spinlock_t irq_lock;

	int queue_size;

	int phase;
	int cq_head;
	int cq_tail;

	struct nvme_completion __iomem **cq;
};

struct nvmev_admin_queue {
	int phase;

	int sq_depth;
	int cq_depth;

	int cq_head;

	struct nvme_command __iomem **nvme_sq;
	struct nvme_completion __iomem **nvme_cq;
};

#define NR_SQE_PER_PAGE	(PAGE_SIZE / sizeof(struct nvme_command))
#define NR_CQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_completion))

#define SQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_CQE_PER_PAGE)

#define SQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_CQE_PER_PAGE)


struct nvmev_config {
	unsigned long memmap_start; // byte
	unsigned long memmap_size;	// byte

	unsigned long storage_start; //byte
	unsigned long storage_size;	// byte
#if SUPPORT_VIRTUAL_CAPACITY
	unsigned long virtual_storage_size;
#endif	
	unsigned int read_delay;	// ns
	unsigned int read_time;		// ns
	unsigned int read_trailing;	// ns
	unsigned int write_delay;	// ns
	unsigned int write_time;	// ns
	unsigned int write_trailing;// ns

	unsigned int nr_io_units;
	unsigned int io_unit_shift;	// 2^

	unsigned int cpu_nr_dispatcher;
	unsigned int nr_io_cpu;
	unsigned int cpu_nr_proc_io[32];

#if SUPPORT_ZNS
	unsigned int zone_size;	// byte
	unsigned int nr_zones;	
	unsigned int nr_active_zones;
	unsigned int nr_open_zones;
	unsigned int nr_zrwa_zones;

	unsigned int nr_io_units_per_zone;
#endif
};

struct nvmev_proc_table {
	int sqid;
	int cqid;

	int sq_entry;
	unsigned int command_id;

	unsigned long long nsecs_start;
	unsigned long long nsecs_target_early;
	unsigned long long nsecs_target;

	unsigned long long nsecs_enqueue;
	unsigned long long nsecs_copy_start;
	unsigned long long nsecs_copy_done;
	unsigned long long nsecs_cq_filled;

	bool is_copied;
	bool is_early_completed;
	bool is_completed;
	bool early_completion;

	bool internal_command;
	unsigned int pgs_to_release;

	unsigned int status;
	unsigned int result0;
	unsigned int result1;

	unsigned int next, prev;
};

struct nvmev_proc_info {
	struct nvmev_proc_table *proc_table;

	unsigned int free_seq;		/* free io req head index */
	unsigned int free_seq_end;	/* free io req tail index */
	unsigned int io_seq;		/* io req head index */
	unsigned int io_seq_end;	/* io req tail index */
	unsigned int id;
	
	unsigned long long proc_io_nsecs;

	struct task_struct *nvmev_io_worker;
	char thread_name[32];
};

struct nvmev_dev {
	struct pci_bus *virt_bus;
	void *virtDev;
	struct pci_header *pcihdr;
	struct pci_pm_cap *pmcap;
	struct pci_msix_cap *msixcap;
	struct pcie_cap *pciecap;
	struct aer_cap *aercap;
	struct pci_exp_hdr *pcie_exp_cap;

	struct pci_dev *pdev;
	struct pci_ops pci_ops;
	struct pci_sysdata pci_sd;

	struct nvmev_config config;
	struct task_struct *nvmev_manager;

	void *storage_mapped;

	struct nvmev_proc_info *proc_info;
	unsigned int proc_turn;

	bool msix_enabled;
	void __iomem *msix_table;

	struct __nvme_bar *old_bar;
	struct nvme_ctrl_regs __iomem *bar;

	u32 *old_dbs;
	u32 __iomem *dbs;

	int nr_ns;
	int nr_sq, nr_cq;

	struct nvmev_admin_queue *admin_q;
	struct nvmev_submission_queue *sqes[NR_MAX_IO_QUEUE + 1];
	struct nvmev_completion_queue *cqes[NR_MAX_IO_QUEUE + 1];

	struct proc_dir_entry *proc_root;
	struct proc_dir_entry *proc_read_times;
	struct proc_dir_entry *proc_write_times;
	struct proc_dir_entry *proc_io_units;
	struct proc_dir_entry *proc_stat;

	unsigned long long *io_unit_stat;
};

// VDEV Init, Final Function
struct nvmev_dev *VDEV_INIT(void);
void VDEV_FINALIZE(struct nvmev_dev *vdev);

// OPS_PCI
void nvmev_proc_bars(void);
bool NVMEV_PCI_INIT(struct nvmev_dev *dev);
void nvmev_signal_irq(int msi_index);

// OPS ADMIN QUEUE
void nvmev_proc_admin_sq(int new_db, int old_db);
void nvmev_proc_admin_cq(int new_db, int old_db);

// OPS I/O QUEUE
void NVMEV_IO_PROC_INIT(struct nvmev_dev *vdev);
void NVMEV_IO_PROC_FINAL(struct nvmev_dev *vdev);
int nvmev_proc_io_sq(int qid, int new_db, int old_db);
void nvmev_proc_io_cq(int qid, int new_db, int old_db);


#endif /* _LIB_NVMEV_H */
