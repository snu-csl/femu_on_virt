#ifndef _LIB_NVMEV_H
#define _LIB_NVMEV_H

#include <linux/pci.h>
#include <asm/apic.h>
#include <linux/irqnr.h>
#include <linux/msi.h>
#include <linux/irq.h>
#include <uapi/linux/irqnr.h>
#include <linux/proc_fs.h>

#include "nvmev_hdr.h"
#include "nvme_hdr.h"

#define PERF_DEBUG 0

#define NVMEV_DRV_NAME "NVMe_Virt_Dev"

#define NVMEV_INFO(string, args...) \
	printk(KERN_INFO "[%s: %s] info: " string, NVMEV_DRV_NAME, __func__, ##args)
#define NVMEV_ERROR(string, args...) \
	printk(KERN_ERR "[%s: %s] error: " string, NVMEV_DRV_NAME, __func__, ##args)

#define ENABLE_DBG_PRINT	0
#if ENABLE_DBG_PRINT
	#define NVMEV_DEBUG(string, args...) \
		printk(KERN_DEBUG "[%s %s] dbg: " string, NVMEV_DRV_NAME, __func__, ##args)
#else
	#define NVMEV_DEBUG(string, args...)
#endif

#define NVMEV_PCI_DOMAIN_NUM 0x0001
#define NVMEV_PCI_BUS_NUM 0x10

//[PCI_HEADER][PM_CAP][MSIX_CAP][PCIE_CAP]
#define SZ_PCI_HDR sizeof(struct pci_header) // 0
#define SZ_PCI_PM_CAP sizeof(struct pci_pm_cap) // 0x40
#define SZ_PCI_MSIX_CAP sizeof(struct pci_msix_cap) // 0x50
#define SZ_PCIE_CAP sizeof(struct pcie_cap) // 0x60

#define OFFS_PCI_HDR 0x0
#define OFFS_PCI_PM_CAP 0x40
#define OFFS_PCI_MSIX_CAP 0x50
#define OFFS_PCIE_CAP 0x60

#define SZ_HEADER 0x60 + SZ_PCIE_CAP

#define PCI_CFG_SPACE_SIZE	256
#define PCIE_EXPCAP_START 0x50

#define IRQ_NUM 16
#define NR_MAX_IO_QUEUE 128
#define NR_MAX_PARALLEL_IO 8192

struct nvmev_ns {
	int nsid;
};

struct nvmev_submission_queue {
	int qid;
	int cqid;
	int sq_priority;
	bool phys_contig;
	
	int queue_size;
	
	struct nvme_command __iomem **sq;
};

struct nvmev_completion_queue {
	int qid;
	int irq;
	int irq_vector;
	bool interrupt_enabled;
	bool phys_contig;
	
	bool interrupt_ready;

	bool affinity_settings;
	const struct cpumask *cpu_mask;

	int queue_size;
	
	int phase;
	int cq_head, cq_tail;
	struct nvme_completion __iomem **cq;

	struct irq_desc *irq_desc;
};

struct nvmev_admin_queue {
	int irq;
	int irq_vector;
	int vector;
	
	bool affinity_settings;
	const struct cpumask *cpu_mask;
	struct msi_desc *desc;

	int phase;
	int sq_depth, cq_depth;
	int cq_head, cq_tail;

	struct nvme_command __iomem **nvme_sq;
	struct nvme_completion __iomem **nvme_cq;
	
	struct irq_desc *irq_desc;
};

struct nvmev_config {
	unsigned long memmap_start; //bytes offs
	unsigned long memmap_size; //bytes offs

	unsigned long storage_start; //bytes offs
	unsigned long storage_size;

	unsigned int read_latency; //ns
	unsigned int write_latency; //ns

	unsigned int read_bw; //MiB
	unsigned int write_bw; //MiB
	long long int read_bw_us;
	long long int write_bw_us;

	unsigned int cpu_nr_proc_reg;
	unsigned int nr_io_cpu;
	unsigned int *cpu_nr_proc_io;
};

struct nvmev_proc_table {
	int sqid;
	int cqid;

	int sq_entry;
	unsigned int command_id;

	long long int nsecs_start;
	long long int nsecs_enqueue;
	long long int nsecs_target;

	bool isCpy;
	bool isProc;

	unsigned int next, prev;
};

struct nvmev_proc_info {
	struct nvmev_proc_table *proc_table;
	unsigned int proc_free_seq;
	unsigned int proc_free_last;
	unsigned int proc_io_seq;
	unsigned int proc_io_seq_end;
	long long int proc_io_nsecs;
	
	struct task_struct *nvmev_io_proc;
	char *thread_name;
};

struct nvmev_dev {
	struct pci_bus *virt_bus;
	void* virtDev;
	struct pci_header* pcihdr;
	struct pci_pm_cap* pmcap;
	struct pci_msix_cap* msixcap;
	struct pcie_cap* pciecap;
	struct aer_cap* aercap;
	struct pci_exp_hdr* pcie_exp_cap;
	
	struct pci_dev *pdev;
	struct pci_ops pci_ops;
	struct pci_sysdata pci_sd;

	struct nvmev_config config;
	struct task_struct *nvmev_reg_proc;

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
	struct nvmev_ns** ns_arr;
	struct nvmev_submission_queue* sqes[NR_MAX_IO_QUEUE + 1];
	struct nvmev_completion_queue* cqes[NR_MAX_IO_QUEUE + 1];
	spinlock_t cq_irq_lock[NR_MAX_IO_QUEUE + 1];

	cpumask_t first_cpu_on_node;

	struct proc_dir_entry *proc_root;
	struct proc_dir_entry *read_latency;
	struct proc_dir_entry *write_latency;
	struct proc_dir_entry *read_bw;
	struct proc_dir_entry *write_bw;
	struct proc_dir_entry *slot;

	unsigned long long *unit_stat;
	int nr_unit;

};

// VDEV Init, Final Function
struct nvmev_dev* VDEV_INIT(void);
void VDEV_FINALIZE(struct nvmev_dev* vdev);
void VDEV_SET_ARGS(struct nvmev_config *config,
		unsigned int memmap_start, unsigned int memmap_size,
		unsigned int read_latency, unsigned int write_latency,
		unsigned int read_bw, unsigned int write_bw,
		char *cpu_mask);

// HEADER Initialize
void PCI_HEADER_SETTINGS(struct nvmev_dev* vdev, struct pci_header* pcihdr);
void PCI_PMCAP_SETTINGS(struct pci_pm_cap* pmcap);
void PCI_MSIXCAP_SETTINGS(struct pci_msix_cap* msixcap);
void PCI_PCIECAP_SETTINGS(struct pcie_cap* pciecap);
void PCI_AERCAP_SETTINGS(struct aer_cap* aercap);
void PCI_PCIE_EXTCAP_SETTINGS(struct pci_exp_hdr* exp_cap);

// OPS_PCI
void nvmev_clone_pci_mem(struct nvmev_dev* vdev);
struct pci_bus* nvmev_create_pci_bus(void);
void nvmev_proc_bars (void);
int get_vector_from_irq(int irq);
void generateInterrupt(int vector);

// OPS ADMIN QUEUE
void nvmev_proc_sq_admin(int new_db, int old_db);
void nvmev_proc_cq_admin(int new_db, int old_db);

// OPS I/O QUEUE
void NVMEV_IO_PROC_INIT(struct nvmev_dev* vdev);
void NVMEV_IO_PROC_FINAL(struct nvmev_dev* vdev);
void nvmev_proc_sq_io(int qid, int new_db, int old_db);
void nvmev_proc_cq_io(int qid, int new_db, int old_db);

void nvmev_proc_io_cleanup(void);
#endif /* _LIB_NVMEV_H */
