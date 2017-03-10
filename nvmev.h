#ifndef _LIB_NVMEV_H
#define _LIB_NVMEV_H

#include <linux/pci.h>
#include "nvmev_hdr.h"

#define NVMEV_DRV_NAME "NVMe_Virt_Dev"

#define NVMEV_INFO(string, args...) \
	printk(KERN_INFO "[%s]" string, NVMEV_DRV_NAME, ##args)
#define NVMEV_ERROR(string, args...) \
	printk(KERN_ERR "[%s]" string, NVMEV_DRV_NAME, ##args)
#define NVMEV_DEBUG(string, args...) \
	printk(KERN_DEBUG "[%s]" string, NVMEV_DRV_NAME, ##args)

#define NVMEV_PCI_DOMAIN_NUM 0x0001
#define NVMEV_PCI_BUS_NUM 0x01

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

#define IRQ_NUM 16

struct nvmev_ns {
	int nsid;
};

struct nvmev_queue {
	int qid;
	
	int irq;
	int irq_vector;
	
	int phase;
	int sq_depth, cq_depth;
	int cq_head, cq_tail;

	struct nvme_command __iomem *nvme_sq;
	struct nvme_completion __iomem *nvme_cq;
};

struct nvmev_dev {
	struct pci_bus *virt_bus;
	void* virtDev;
	struct pci_header* pcihdr;
	struct pci_pm_cap* pmcap;
	struct pci_msix_cap* msixcap;
	struct pcie_cap* pciecap;
	struct aer_cap* aercap;
	
	struct pci_dev *pdev;
	struct pci_ops pci_ops;
	struct pci_sysdata pci_sd;

	struct nvme_bar *old_bar;
	struct nvme_ctrl_regs __iomem *bar;

	u32 *old_dbs;
	u32 __iomem *dbs;

	int nr_ns;
	int nr_queue;

	struct nvmev_ns** ns_arr;
	struct nvmev_queue** queue_arr;
};


// OPS_PCI
struct pci_bus* nvmev_create_pci_bus(void);
void nvmev_proc_bars (void);
int get_vector_from_irq(int irq);
void generateInterrupt(int vector);

// OPS ADMIN QUEUE
void nvmev_proc_sq_admin(int qid, int new_db, int old_db);
void nvmev_proc_cq_admin(int qid, int new_db, int old_db);

// OPS I/O QUEUE
void nvmev_proc_sq_io(int qid, int new_db, int old_db);
void nvmev_proc_cq_io(int qid, int new_db, int old_db);

extern struct nvmev_dev *dev;


#endif /* _LIB_NVMEV_H */
