#include <linux/pci.h>
#include <linux/irq.h>
#include "nvmev.h"

extern struct nvmev_dev *vdev;

struct __apic_chip_data {
	struct irq_cfg		cfg;
	cpumask_var_t		domain;
	cpumask_var_t		old_domain;
	u8			move_in_progress : 1;
};

int get_vector_from_irq(int irq)
{
	struct irq_data * irqd = irq_get_irq_data(irq);
	struct irq_cfg *irqd_cfg;
	struct __apic_chip_data *chip_data;

	while (irqd->parent_data)
		irqd = irqd->parent_data;

	chip_data = irqd->chip_data;
	irqd_cfg = &chip_data->cfg;

	return irqd_cfg->vector;
}

void nvmev_proc_bars ()
{
	struct __nvme_bar *old_bar = vdev->old_bar;
	struct nvme_ctrl_regs *bar = vdev->bar;
	struct nvmev_admin_queue *queue;
	unsigned int num_pages, i;

#if 0
// Read only Register
	if (old_bar->cap != bar->u_cap) {
		memcpy(&old_bar->cap, &bar->cap, sizeof(old_bar->cap));
	}
	if (old_bar->vs != bar->u_vs) {
		memcpy(&old_bar->vs, &bar->vs, sizeof(old_bar->vs));
	}
	if (old_bar->cmbloc != bar->u_cmbloc) {
		memcpy(&old_bar->cmbloc, &bar->cmbloc, sizeof(old_bar->cmbloc));
	}
	if (old_bar->cmbsz != bar->u_cmbsz) {
		memcpy(&old_bar->cmbsz, &bar->cmbsz, sizeof(old_bar->cmbsz));
	}
#endif
	if (old_bar->intms != bar->intms) {
		memcpy(&old_bar->intms, &bar->intms, sizeof(old_bar->intms));
	}
	if (old_bar->intmc != bar->intmc) {
		memcpy(&old_bar->intmc, &bar->intmc, sizeof(old_bar->intmc));
	}
	if (old_bar->cc != bar->u_cc) {
		//////////////////////////////////
		// Enable
		//////////////////////////////////
		if (bar->cc.en == 1)
			bar->csts.rdy = 1;
		else if (bar->cc.en == 0)
			bar->csts.rdy = 0;

		//////////////////////////////////
		// Shutdown
		//////////////////////////////////
		if (bar->cc.shn == 1) {
			bar->csts.shst = 1; // proc
			bar->csts.shst = 2; // end
		}

		memcpy(&old_bar->cc, &bar->cc, sizeof(old_bar->cc));
	}
	if (old_bar->rsvd1 != bar->rsvd1) {
		memcpy(&old_bar->rsvd1, &bar->rsvd1, sizeof(old_bar->rsvd1));
	}
	if (old_bar->csts != bar->u_csts) {
		memcpy(&old_bar->csts, &bar->csts, sizeof(old_bar->csts));
	}
	if (old_bar->nssr != bar->nssr) {
		memcpy(&old_bar->nssr, &bar->nssr, sizeof(old_bar->nssr));
	}
	if (old_bar->aqa != bar->u_aqa) {
		//Admin Queue Initial..
		memcpy(&old_bar->aqa, &bar->aqa, sizeof(old_bar->aqa));
		queue =	kzalloc(sizeof(struct nvmev_admin_queue), GFP_KERNEL);

		queue->irq = IRQ_NUM;
		queue->irq_vector = get_vector_from_irq(queue->irq);
		queue->vector = 0;
		queue->cq_head = 0;
		queue->cq_tail = -1;
		queue->phase = 1;
		queue->sq_depth = bar->aqa.asqs + 1;
		queue->cq_depth = bar->aqa.acqs + 1;
		vdev->admin_q = queue;

		/*
		 * MSI is re-enabled so that MSI interrupt vectors
		 * can be allocated by the nvme driver
		 */

		vdev->pdev->no_msi = 0;
	}
	if (old_bar->asq != bar->u_asq) {
		queue = vdev->admin_q;
		memcpy(&old_bar->asq, &bar->asq, sizeof(old_bar->asq));

		num_pages = (queue->sq_depth * sizeof(struct nvme_command)) / PAGE_SIZE;
		if ((queue->sq_depth * sizeof(struct nvme_command)) % PAGE_SIZE) num_pages++;

		vdev->admin_q->nvme_sq = kzalloc(sizeof(struct nvme_command *) * num_pages, GFP_KERNEL);
		if (queue->nvme_sq == NULL)
			NVMEV_ERROR("Error on Setup Admin Queue [Submission]\n");

		for (i = 0; i < num_pages; i++) {
			vdev->admin_q->nvme_sq[i] = page_address(pfn_to_page(vdev->bar->u_asq >> PAGE_SHIFT) + i);
		}
	}
	if (old_bar->acq != bar->u_acq) {
		queue = vdev->admin_q;
		memcpy(&old_bar->acq, &bar->acq, sizeof(old_bar->acq));

		num_pages = (queue->cq_depth * sizeof(struct nvme_completion)) / PAGE_SIZE;
		if ((queue->cq_depth * sizeof(struct nvme_completion)) % PAGE_SIZE) num_pages++;

		vdev->admin_q->nvme_cq = kzalloc(sizeof(struct nvme_completion*) * num_pages, GFP_KERNEL);
		if (queue->nvme_cq == NULL)
			NVMEV_ERROR("Error on Setup Admin Queue [Completion]\n");

		for (i = 0; i < num_pages; i++) {
			vdev->admin_q->nvme_cq[i] = page_address(pfn_to_page(vdev->bar->u_acq >> PAGE_SHIFT) + i);
		}
	}
}

int nvmev_pci_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	if (devfn != 0) return 1;

	memcpy(val, vdev->virtDev+where, size);

	return 0;
};

int nvmev_pci_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 _val)
{
	u32 mask = 0xFFFFFFFF;
	u32 val;
	int target = where;

	memcpy(&val, vdev->virtDev+where, size);

	if (where < OFFS_PCI_PM_CAP) {
	// PCI_HDR
		if (target == 0x0) mask = 0x0;
		else if (target == 0x4) mask = 0x0547;
		else if (target == 0x6) mask = 0xF200;
		else if (target == 0x9) mask = 0x0;
		else if (target == 0xd) mask = 0x0;
		else if (target == 0xe) mask = 0x0;
		else if (target == 0xf) mask = 0x40;
		else if (target == 0x10) mask = 0xFFFFC000;
		//else if (target == 0x18) mask = 0xFFFFFFF8;
		else if (target == 0x18) mask = 0x0;
		else if (target == 0x1c) mask = 0x0;
		else if (target == 0x20) mask = 0x0;
		else if (target == 0x24) mask = 0x0;
		else if (target == 0x28) mask = 0x0;
		else if (target == 0x2c) mask = 0x0;
		else if (target == 0x34) mask = 0x0;
		else if (target == 0x3c) mask = 0xF;
		else if (target == 0x3e) mask = 0x0;
		else if (target == 0x3f) mask = 0x0;
	} else if (where < OFFS_PCI_MSIX_CAP) {
	// PCI_PM_CAP
	} else if (where < OFFS_PCIE_CAP) {
	// PCI_MSIX_CAP
		target -= OFFS_PCI_MSIX_CAP;
		if (target == 0)
			mask = 0x0;
		else if (target == 2) {
			mask = 0xC000;

			//MSIX enabled? -> admin queue irq setup
			if ((val & mask) == mask) {
				vdev->msix_enabled = true;
				vdev->msix_table = ioremap(pci_resource_start(vdev->pdev,0) + 0x2000, NR_MAX_IO_QUEUE * PCI_MSIX_ENTRY_SIZE);
				//vdev->msix_table = memremap(pci_resource_start(vdev->pdev,0) + 0x2000, NR_MAX_IO_QUEUE * PCI_MSIX_ENTRY_SIZE, MEMREMAP_WT);
				vdev->admin_q->irq_vector = readl(vdev->msix_table+PCI_MSIX_ENTRY_DATA) & 0xFF;
			}
		}
		else if (target == 4) mask = 0x0;
		else if (target == 8) mask = 0x0;
	} else {
		// PCIE_CAP
	}
	val = (val & (~mask)) | (_val & mask);
	memcpy(vdev->virtDev+where, &val, size);

	return 0;
};

struct pci_bus* nvmev_create_pci_bus()
{
    struct pci_bus* nvmev_pci_bus = NULL;
	struct resource *res;
	struct pci_dev *dev;
	memset (&vdev->pci_ops, 0, sizeof(vdev->pci_ops));
    vdev->pci_ops.read = nvmev_pci_read;
    vdev->pci_ops.write = nvmev_pci_write;

	memset (&vdev->pci_sd, 0, sizeof(vdev->pci_sd));
	vdev->pci_sd.domain = NVMEV_PCI_DOMAIN_NUM;
	vdev->pci_sd.node = 0;

	// Create Root Bus
    nvmev_pci_bus = pci_scan_bus(NVMEV_PCI_BUS_NUM, &vdev->pci_ops, (void *)&vdev->pci_sd);
	//pci_rescan_bus(nvmev_pci_bus);

	if (!nvmev_pci_bus){
		NVMEV_ERROR("Fail to create PCI bus\n");
		return NULL;
	}

	list_for_each_entry(dev, &nvmev_pci_bus->devices, bus_list) {
		res = &dev->resource[0];
		res->parent = &iomem_resource;
		//vdev->bar = ioremap(pci_resource_start(dev, 0), 8192);
		vdev->bar = memremap(pci_resource_start(dev, 0), 8192, MEMREMAP_WT);
		memset(vdev->bar, 0x0, 8192);
		vdev->dbs = ((void *)vdev->bar) + 4096;
		NVMEV_INFO("%s: %p %p %p\n", __func__, vdev,
				vdev->bar, vdev->old_bar);
		vdev->pdev = dev;

		/*
		 * Prevents a crash when the nvme driver assigns an
		 * initial single MSI vector. Virt has no admin queue
		 * at this point and will crash when reading the MSI
		 * BAR
		 */

		vdev->pdev->no_msi = 1;
	}
	pci_bus_add_devices(nvmev_pci_bus);
	vdev->pcihdr->cmd.mse = 1;

	vdev->bar->vs.mnr = 0;
	vdev->bar->vs.mjr = 1;
	vdev->bar->cap.mpsmin = 0;
	vdev->bar->cap.mqes = 1024 - 1; //base value = 0, 0 means depth 1

	NVMEV_INFO("Successfully created Virtual PCI bus\n");

	return nvmev_pci_bus;
};

void nvmev_clone_pci_mem(struct nvmev_dev* vdev)
{
	vdev->old_dbs = kzalloc(4096, GFP_KERNEL);
	if (vdev->old_dbs == NULL) {
		NVMEV_ERROR("Allocating old DBs memory");
	}
	vdev->old_bar = kzalloc(4096, GFP_KERNEL);
	if (vdev->old_bar == NULL) {
		NVMEV_ERROR("Allocating old BAR memory");
	}

	memcpy(vdev->old_bar, vdev->bar, sizeof(*vdev->old_bar));
	memcpy(vdev->old_dbs, vdev->dbs, sizeof(*vdev->old_dbs));
}

void generateInterrupt(int vector)
{
	switch(vector) {
		case 0x0: asm("int $0x0"); break;
		case 0x1: asm("int $0x1"); break;
		case 0x2: asm("int $0x2"); break;
		case 0x3: asm("int $0x3"); break;
		case 0x4: asm("int $0x4"); break;
		case 0x5: asm("int $0x5"); break;
		case 0x6: asm("int $0x6"); break;
		case 0x7: asm("int $0x7"); break;
		case 0x8: asm("int $0x8"); break;
		case 0x9: asm("int $0x9"); break;
		case 0xA: asm("int $0xA"); break;
		case 0xB: asm("int $0xB"); break;
		case 0xC: asm("int $0xC"); break;
		case 0xD: asm("int $0xD"); break;
		case 0xE: asm("int $0xE"); break;
		case 0xF: asm("int $0xF"); break;
		case 0x10: asm("int $0x10"); break;
		case 0x11: asm("int $0x11"); break;
		case 0x12: asm("int $0x12"); break;
		case 0x13: asm("int $0x13"); break;
		case 0x14: asm("int $0x14"); break;
		case 0x15: asm("int $0x15"); break;
		case 0x16: asm("int $0x16"); break;
		case 0x17: asm("int $0x17"); break;
		case 0x18: asm("int $0x18"); break;
		case 0x19: asm("int $0x19"); break;
		case 0x1A: asm("int $0x1A"); break;
		case 0x1B: asm("int $0x1B"); break;
		case 0x1C: asm("int $0x1C"); break;
		case 0x1D: asm("int $0x1D"); break;
		case 0x1E: asm("int $0x1E"); break;
		case 0x1F: asm("int $0x1F"); break;
		case 0x20: asm("int $0x20"); break;
		case 0x21: asm("int $0x21"); break;
		case 0x22: asm("int $0x22"); break;
		case 0x23: asm("int $0x23"); break;
		case 0x24: asm("int $0x24"); break;
		case 0x25: asm("int $0x25"); break;
		case 0x26: asm("int $0x26"); break;
		case 0x27: asm("int $0x27"); break;
		case 0x28: asm("int $0x28"); break;
		case 0x29: asm("int $0x29"); break;
		case 0x2A: asm("int $0x2A"); break;
		case 0x2B: asm("int $0x2B"); break;
		case 0x2C: asm("int $0x2C"); break;
		case 0x2D: asm("int $0x2D"); break;
		case 0x2E: asm("int $0x2E"); break;
		case 0x2F: asm("int $0x2F"); break;
		case 0x30: asm("int $0x30"); break;
		case 0x31: asm("int $0x31"); break;
		case 0x32: asm("int $0x32"); break;
		case 0x33: asm("int $0x33"); break;
		case 0x34: asm("int $0x34"); break;
		case 0x35: asm("int $0x35"); break;
		case 0x36: asm("int $0x36"); break;
		case 0x37: asm("int $0x37"); break;
		case 0x38: asm("int $0x38"); break;
		case 0x39: asm("int $0x39"); break;
		case 0x3A: asm("int $0x3A"); break;
		case 0x3B: asm("int $0x3B"); break;
		case 0x3C: asm("int $0x3C"); break;
		case 0x3D: asm("int $0x3D"); break;
		case 0x3E: asm("int $0x3E"); break;
		case 0x3F: asm("int $0x3F"); break;
		case 0x40: asm("int $0x40"); break;
		case 0x41: asm("int $0x41"); break;
		case 0x42: asm("int $0x42"); break;
		case 0x43: asm("int $0x43"); break;
		case 0x44: asm("int $0x44"); break;
		case 0x45: asm("int $0x45"); break;
		case 0x46: asm("int $0x46"); break;
		case 0x47: asm("int $0x47"); break;
		case 0x48: asm("int $0x48"); break;
		case 0x49: asm("int $0x49"); break;
		case 0x4A: asm("int $0x4A"); break;
		case 0x4B: asm("int $0x4B"); break;
		case 0x4C: asm("int $0x4C"); break;
		case 0x4D: asm("int $0x4D"); break;
		case 0x4E: asm("int $0x4E"); break;
		case 0x4F: asm("int $0x4F"); break;
		case 0x50: asm("int $0x50"); break;
		case 0x51: asm("int $0x51"); break;
		case 0x52: asm("int $0x52"); break;
		case 0x53: asm("int $0x53"); break;
		case 0x54: asm("int $0x54"); break;
		case 0x55: asm("int $0x55"); break;
		case 0x56: asm("int $0x56"); break;
		case 0x57: asm("int $0x57"); break;
		case 0x58: asm("int $0x58"); break;
		case 0x59: asm("int $0x59"); break;
		case 0x5A: asm("int $0x5A"); break;
		case 0x5B: asm("int $0x5B"); break;
		case 0x5C: asm("int $0x5C"); break;
		case 0x5D: asm("int $0x5D"); break;
		case 0x5E: asm("int $0x5E"); break;
		case 0x5F: asm("int $0x5F"); break;
		case 0x60: asm("int $0x60"); break;
		case 0x61: asm("int $0x61"); break;
		case 0x62: asm("int $0x62"); break;
		case 0x63: asm("int $0x63"); break;
		case 0x64: asm("int $0x64"); break;
		case 0x65: asm("int $0x65"); break;
		case 0x66: asm("int $0x66"); break;
		case 0x67: asm("int $0x67"); break;
		case 0x68: asm("int $0x68"); break;
		case 0x69: asm("int $0x69"); break;
		case 0x6A: asm("int $0x6A"); break;
		case 0x6B: asm("int $0x6B"); break;
		case 0x6C: asm("int $0x6C"); break;
		case 0x6D: asm("int $0x6D"); break;
		case 0x6E: asm("int $0x6E"); break;
		case 0x6F: asm("int $0x6F"); break;
		case 0x70: asm("int $0x70"); break;
		case 0x71: asm("int $0x71"); break;
		case 0x72: asm("int $0x72"); break;
		case 0x73: asm("int $0x73"); break;
		case 0x74: asm("int $0x74"); break;
		case 0x75: asm("int $0x75"); break;
		case 0x76: asm("int $0x76"); break;
		case 0x77: asm("int $0x77"); break;
		case 0x78: asm("int $0x78"); break;
		case 0x79: asm("int $0x79"); break;
		case 0x7A: asm("int $0x7A"); break;
		case 0x7B: asm("int $0x7B"); break;
		case 0x7C: asm("int $0x7C"); break;
		case 0x7D: asm("int $0x7D"); break;
		case 0x7E: asm("int $0x7E"); break;
		case 0x7F: asm("int $0x7F"); break;
		case 0x80: asm("int $0x80"); break;
		case 0x81: asm("int $0x81"); break;
		case 0x82: asm("int $0x82"); break;
		case 0x83: asm("int $0x83"); break;
		case 0x84: asm("int $0x84"); break;
		case 0x85: asm("int $0x85"); break;
		case 0x86: asm("int $0x86"); break;
		case 0x87: asm("int $0x87"); break;
		case 0x88: asm("int $0x88"); break;
		case 0x89: asm("int $0x89"); break;
		case 0x8A: asm("int $0x8A"); break;
		case 0x8B: asm("int $0x8B"); break;
		case 0x8C: asm("int $0x8C"); break;
		case 0x8D: asm("int $0x8D"); break;
		case 0x8E: asm("int $0x8E"); break;
		case 0x8F: asm("int $0x8F"); break;
		case 0x90: asm("int $0x90"); break;
		case 0x91: asm("int $0x91"); break;
		case 0x92: asm("int $0x92"); break;
		case 0x93: asm("int $0x93"); break;
		case 0x94: asm("int $0x94"); break;
		case 0x95: asm("int $0x95"); break;
		case 0x96: asm("int $0x96"); break;
		case 0x97: asm("int $0x97"); break;
		case 0x98: asm("int $0x98"); break;
		case 0x99: asm("int $0x99"); break;
		case 0x9A: asm("int $0x9A"); break;
		case 0x9B: asm("int $0x9B"); break;
		case 0x9C: asm("int $0x9C"); break;
		case 0x9D: asm("int $0x9D"); break;
		case 0x9E: asm("int $0x9E"); break;
		case 0x9F: asm("int $0x9F"); break;
		case 0xA0: asm("int $0xA0"); break;
		case 0xA1: asm("int $0xA1"); break;
		case 0xA2: asm("int $0xA2"); break;
		case 0xA3: asm("int $0xA3"); break;
		case 0xA4: asm("int $0xA4"); break;
		case 0xA5: asm("int $0xA5"); break;
		case 0xA6: asm("int $0xA6"); break;
		case 0xA7: asm("int $0xA7"); break;
		case 0xA8: asm("int $0xA8"); break;
		case 0xA9: asm("int $0xA9"); break;
		case 0xAA: asm("int $0xAA"); break;
		case 0xAB: asm("int $0xAB"); break;
		case 0xAC: asm("int $0xAC"); break;
		case 0xAD: asm("int $0xAD"); break;
		case 0xAE: asm("int $0xAE"); break;
		case 0xAF: asm("int $0xAF"); break;
		case 0xB0: asm("int $0xB0"); break;
		case 0xB1: asm("int $0xB1"); break;
		case 0xB2: asm("int $0xB2"); break;
		case 0xB3: asm("int $0xB3"); break;
		case 0xB4: asm("int $0xB4"); break;
		case 0xB5: asm("int $0xB5"); break;
		case 0xB6: asm("int $0xB6"); break;
		case 0xB7: asm("int $0xB7"); break;
		case 0xB8: asm("int $0xB8"); break;
		case 0xB9: asm("int $0xB9"); break;
		case 0xBA: asm("int $0xBA"); break;
		case 0xBB: asm("int $0xBB"); break;
		case 0xBC: asm("int $0xBC"); break;
		case 0xBD: asm("int $0xBD"); break;
		case 0xBE: asm("int $0xBE"); break;
		case 0xBF: asm("int $0xBF"); break;
		case 0xC0: asm("int $0xC0"); break;
		case 0xC1: asm("int $0xC1"); break;
		case 0xC2: asm("int $0xC2"); break;
		case 0xC3: asm("int $0xC3"); break;
		case 0xC4: asm("int $0xC4"); break;
		case 0xC5: asm("int $0xC5"); break;
		case 0xC6: asm("int $0xC6"); break;
		case 0xC7: asm("int $0xC7"); break;
		case 0xC8: asm("int $0xC8"); break;
		case 0xC9: asm("int $0xC9"); break;
		case 0xCA: asm("int $0xCA"); break;
		case 0xCB: asm("int $0xCB"); break;
		case 0xCC: asm("int $0xCC"); break;
		case 0xCD: asm("int $0xCD"); break;
		case 0xCE: asm("int $0xCE"); break;
		case 0xCF: asm("int $0xCF"); break;
		case 0xD0: asm("int $0xD0"); break;
		case 0xD1: asm("int $0xD1"); break;
		case 0xD2: asm("int $0xD2"); break;
		case 0xD3: asm("int $0xD3"); break;
		case 0xD4: asm("int $0xD4"); break;
		case 0xD5: asm("int $0xD5"); break;
		case 0xD6: asm("int $0xD6"); break;
		case 0xD7: asm("int $0xD7"); break;
		case 0xD8: asm("int $0xD8"); break;
		case 0xD9: asm("int $0xD9"); break;
		case 0xDA: asm("int $0xDA"); break;
		case 0xDB: asm("int $0xDB"); break;
		case 0xDC: asm("int $0xDC"); break;
		case 0xDD: asm("int $0xDD"); break;
		case 0xDE: asm("int $0xDE"); break;
		case 0xDF: asm("int $0xDF"); break;
		case 0xE0: asm("int $0xE0"); break;
		case 0xE1: asm("int $0xE1"); break;
		case 0xE2: asm("int $0xE2"); break;
		case 0xE3: asm("int $0xE3"); break;
		case 0xE4: asm("int $0xE4"); break;
		case 0xE5: asm("int $0xE5"); break;
		case 0xE6: asm("int $0xE6"); break;
		case 0xE7: asm("int $0xE7"); break;
		case 0xE8: asm("int $0xE8"); break;
		case 0xE9: asm("int $0xE9"); break;
		case 0xEA: asm("int $0xEA"); break;
		case 0xEB: asm("int $0xEB"); break;
		case 0xEC: asm("int $0xEC"); break;
		case 0xED: asm("int $0xED"); break;
		case 0xEE: asm("int $0xEE"); break;
		case 0xEF: asm("int $0xEF"); break;
		case 0xF0: asm("int $0xF0"); break;
		case 0xF1: asm("int $0xF1"); break;
		case 0xF2: asm("int $0xF2"); break;
		case 0xF3: asm("int $0xF3"); break;
		case 0xF4: asm("int $0xF4"); break;
		case 0xF5: asm("int $0xF5"); break;
		case 0xF6: asm("int $0xF6"); break;
		case 0xF7: asm("int $0xF7"); break;
		case 0xF8: asm("int $0xF8"); break;
		case 0xF9: asm("int $0xF9"); break;
		case 0xFA: asm("int $0xFA"); break;
		case 0xFB: asm("int $0xFB"); break;
		case 0xFC: asm("int $0xFC"); break;
		case 0xFD: asm("int $0xFD"); break;
		case 0xFE: asm("int $0xFE"); break;
		case 0xFF: asm("int $0xFF"); break;
	}
}
