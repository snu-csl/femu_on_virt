#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/nvme.h>
#include "nvmev.h"

static struct task_struct *nvmev_thread;
struct nvmev_dev *vdev;
EXPORT_SYMBOL(vdev);

static void nvmev_proc_dbs(void) {
	int qid;
	int dbs_idx;
	int new_db;	

	// Admin Queue
	new_db = vdev->dbs[0];
	if(new_db != vdev->old_dbs[0]) {
		nvmev_proc_sq_admin(new_db, vdev->old_dbs[0]);
		vdev->old_dbs[0] = new_db;
	}
	new_db = vdev->dbs[1];
	if(new_db != vdev->old_dbs[1]) {
		nvmev_proc_cq_admin(new_db, vdev->old_dbs[1]);
		vdev->old_dbs[1] = new_db;
	}

	// Submission Queue
	for(qid=1; qid<=vdev->nr_sq; qid++) {
		dbs_idx = qid * 2;
		new_db = vdev->dbs[dbs_idx];
		if(new_db != vdev->old_dbs[dbs_idx]) {
			nvmev_proc_sq_io(qid-1, new_db, vdev->old_dbs[dbs_idx]);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}

	// Completion Queue
	for(qid=1; qid<=vdev->nr_cq; qid++) {
		dbs_idx = qid * 2 + 1;
		new_db = vdev->dbs[dbs_idx];
		if(new_db != vdev->old_dbs[dbs_idx]) {
			nvmev_proc_cq_io(qid-1, new_db, vdev->old_dbs[dbs_idx]);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}
}

static int nvmev_kthread(void *data)
{
	while(!kthread_should_stop()) {

		// BAR Register Check
		nvmev_proc_bars();
		//Doorbell
		nvmev_proc_dbs();

		schedule_timeout(round_jiffies_relative(HZ));
	}

	return 0;
}

static int NVMeV_init(void){
	struct pci_exp_hdr* pcie_exp_cap;
	unsigned long pva;
	
	pr_info("NVMe Virtual Device Initialize Start\n");

	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);

	vdev->virtDev = kzalloc(PAGE_SIZE, GFP_KERNEL);

	vdev->pcihdr = (void *)vdev->virtDev + OFFS_PCI_HDR;
	vdev->pmcap = (void *)vdev->virtDev + OFFS_PCI_PM_CAP;
	vdev->msixcap = (void *)vdev->virtDev + OFFS_PCI_MSIX_CAP;
	vdev->pciecap = (void *)vdev->virtDev + OFFS_PCIE_CAP;
	pcie_exp_cap = (void *)vdev->virtDev + PCI_CFG_SPACE_SIZE;
	vdev->aercap = (void *)vdev->virtDev + PCI_CFG_SPACE_SIZE;

	// PCI Header Setting
	vdev->pcihdr->id.did = 0x0101;
	vdev->pcihdr->id.vid = 0x0c51;
	/*
	vdev->pcihdr->cmd.id = 1;
	vdev->pcihdr->cmd.bme = 1;
	vdev->pcihdr->cmd.mse = 1;
	*/
	vdev->pcihdr->sts.cl = 1;

	vdev->pcihdr->htype.mfd = 0;
	vdev->pcihdr->htype.hl = PCI_HEADER_TYPE_NORMAL;

	vdev->pcihdr->rid = 0x01;

	vdev->pcihdr->cc.bcc = PCI_BASE_CLASS_STORAGE;
	vdev->pcihdr->cc.scc = 0x08;
	vdev->pcihdr->cc.pi = 0x02;

	pva = 0x0000000780000000; //-0x00000007ffffffff//PFN_PHYS(page_to_pfn(bar_pages));

	vdev->pcihdr->mlbar.tp = PCI_BASE_ADDRESS_MEM_TYPE_64 >> 1;
	vdev->pcihdr->mlbar.ba = (pva & 0xFFFFFFFF) >> 14;
	
	vdev->pcihdr->mulbar = pva >> 32;

	vdev->pcihdr->ss.ssid = 0x370d;
	vdev->pcihdr->ss.ssvid = 0x0c51;
	
	vdev->pcihdr->erom = 0x0; //PFN_PHYS(page_to_pfn(bar_pages));//page_to_pfn(bar_pages);//0xDF300000;

	vdev->pcihdr->cap = OFFS_PCI_PM_CAP;

	vdev->pcihdr->intr.ipin = 1;
	vdev->pcihdr->intr.iline = IRQ_NUM;
	
	// PM HEADER Setting
	vdev->pmcap->pid.cid = PCI_CAP_ID_PM;
	vdev->pmcap->pid.next = OFFS_PCI_MSIX_CAP;

	vdev->pmcap->pc.vs = 3;
	vdev->pmcap->pmcs.nsfrst = 1;
	vdev->pmcap->pmcs.ps = PCI_PM_CAP_PME_D0 >> 16;

	// PCI MSI-X HEADER Setting
	vdev->msixcap->mxid.cid = PCI_CAP_ID_MSIX;
	vdev->msixcap->mxid.next = OFFS_PCIE_CAP;
	
	vdev->msixcap->mxc.mxe = 1;
	vdev->msixcap->mxc.ts = 31; // encoded as n-1

	vdev->msixcap->mtab.tbir = 0;
	vdev->msixcap->mtab.to = 0x400;
	
	vdev->msixcap->mpba.pbao = 0x600;
	vdev->msixcap->mpba.pbir = 0;

	// PCI-X Header Setting
	vdev->pciecap->pxid.cid = PCI_CAP_ID_EXP;
	vdev->pciecap->pxid.next = 0x0;

	vdev->pciecap->pxcap.ver = PCI_EXP_FLAGS;
	vdev->pciecap->pxcap.imn = 0;
	vdev->pciecap->pxcap.dpt = PCI_EXP_TYPE_ENDPOINT;

	vdev->pciecap->pxdcap.mps = 1;
	vdev->pciecap->pxdcap.pfs = 0;
	vdev->pciecap->pxdcap.etfs = 1;
	vdev->pciecap->pxdcap.l0sl = 6;
	vdev->pciecap->pxdcap.l1l = 2;
	vdev->pciecap->pxdcap.rer = 1;
	vdev->pciecap->pxdcap.csplv = 0;
	vdev->pciecap->pxdcap.cspls = 0;
	vdev->pciecap->pxdcap.flrc = 1;
	
	vdev->aercap->aerid.cid = PCI_EXT_CAP_ID_ERR;
	vdev->aercap->aerid.cver = 1;
	vdev->aercap->aerid.next = PCI_CFG_SPACE_SIZE + 0x50;

	pcie_exp_cap = (void*)vdev->virtDev + vdev->aercap->aerid.next;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_VC;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x80;

	pcie_exp_cap = (void*)vdev->virtDev + pcie_exp_cap->id.next;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_PWR;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x90;

	pcie_exp_cap = (void*)vdev->virtDev + pcie_exp_cap->id.next;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_ARI;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x170;

	pcie_exp_cap = (void*)vdev->virtDev + pcie_exp_cap->id.next;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_DSN;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x1a0;

	pcie_exp_cap = (void*)vdev->virtDev + pcie_exp_cap->id.next;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_SECPCI;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = 0;

	//Create PCI BUS
	vdev->virt_bus = nvmev_create_pci_bus();
	if(!vdev->virt_bus)
		goto ret_err_pci_bus;
	else {
		vdev->old_dbs = kzalloc(4096, GFP_KERNEL);
		if(vdev->old_dbs == NULL) {
			pr_err("Error");
		}
		vdev->old_bar = kzalloc(4096, GFP_KERNEL);
		if(vdev->old_bar == NULL) {
			pr_err("Error");
		}

		memcpy(vdev->old_bar, vdev->bar, sizeof(*vdev->old_bar));
		pr_err("%s: %p %p %p\n", __func__, vdev,
				vdev->bar, vdev->old_bar);

	}

	nvmev_thread = kthread_run(nvmev_kthread, NULL, "nvmev");

	NVMEV_INFO("Successfully created Virtual NVMe Deivce\n");

    return 0;
	
ret_err_pci_bus:
	kfree(vdev->virtDev);
	kfree(vdev);
    return -1;
}

static void NVMeV_exit(void)
{	
	struct task_struct *tmp = NULL;
	int i;

	if(!IS_ERR_OR_NULL(nvmev_thread)) {
		tmp = nvmev_thread;
		nvmev_thread = NULL;
	}

	if (tmp)
		kthread_stop(tmp);

	if(vdev->virt_bus != NULL) {
		pci_remove_bus(vdev->virt_bus);

		for(i=0; i<vdev->nr_sq; i++) {
			//iounmap
			kfree(vdev->sqes[i]);
		}

		for(i=0; i<vdev->nr_cq; i++) {
			kfree(vdev->cqes[i]);
		}
		
		if(vdev->msix_enabled)
			iounmap(vdev->msix_table);

		iounmap(vdev->bar);
		kfree(vdev->old_bar);
		kfree(vdev->admin_q);
		kfree(vdev->virtDev);
		kfree(vdev);
	}
	pr_info("NVMe Virtual Device Close\n");
}

MODULE_LICENSE("Dual BSD/GPL");
module_init(NVMeV_init);
module_exit(NVMeV_exit);
