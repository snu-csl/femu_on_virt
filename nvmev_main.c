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
#include <asm/e820.h>
#include "nvmev.h"

/****************************************************************
 * Memory Layout
 ****************************************************************
 * memmap_start
 * v 
 * -----------------------------------------------------------
 * |<----1MiB---->|<----------- Storage Area --------------->|
 * -----------------------------------------------------------
 *
 * 1MiB Area [BAR] : 
 *		PCI Headers...
 *		MSI-x entries (16 bytes * 64KiB = 1MiB T.T)
 * Storage Area : Storage area : memremap
 *
 ****************************************************************/

/****************************************************************
 * Argument
 ****************************************************************
 * 1. Memmap Start (size in GiB)
 * 2. Memmap Size (size in MiB)
 * 3. Read Latency (export to sysfs, micro seconds)
 * 4. Write Latency (export to sysfs, micro seconds)
 * 5. Read BW
 * 6. Write BW
 * 7. CPU Mask
 ****************************************************************/

struct nvmev_dev *vdev = NULL;
EXPORT_SYMBOL(vdev);

unsigned int memmap_start=0;
unsigned int memmap_size=0;
unsigned int read_latency=0;
unsigned int write_latency=0;
unsigned int read_bw=0;
unsigned int write_bw=0;
unsigned int cpu_mask=0;

module_param(memmap_start, uint, 0);
MODULE_PARM_DESC(memmap_start, "Memmap Start (size in GiB)");
module_param(memmap_size, uint, 0);
MODULE_PARM_DESC(memmap_size, "Memmap Size (size in MiB)");
module_param(read_latency, uint, 0);
MODULE_PARM_DESC(read_latency, "Read Latency (us, micro seconds)");
module_param(write_latency, uint, 0);
MODULE_PARM_DESC(write_latency, "Write Latency (us, micro seconds)");
module_param(read_bw, uint, 0);
MODULE_PARM_DESC(read_bw, "Max Read IOPS");
module_param(write_bw, uint, 0);
MODULE_PARM_DESC(write_bw, "Max Write IOPS");
module_param(cpu_mask, uint, 0);
MODULE_PARM_DESC(write_bw, "CPU Masks for Process, Complete(int.) Thread");

static void nvmev_proc_dbs(void) {
	int qid;
	int dbs_idx;
	int new_db;	
	int old_db;

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
		old_db = vdev->old_dbs[dbs_idx];
		if(new_db != old_db) {
			nvmev_proc_sq_io(qid, new_db, old_db);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}

	// Completion Queue
	for(qid=1; qid<=vdev->nr_cq; qid++) {
		dbs_idx = qid * 2 + 1;
		new_db = vdev->dbs[dbs_idx];
		old_db = vdev->old_dbs[dbs_idx];
		if(new_db != old_db) {
			nvmev_proc_cq_io(qid, new_db, old_db);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}
}

static int nvmev_kthread_proc_reg(void *data)
{
	while(!kthread_should_stop()) {
		// BAR Register Check
		nvmev_proc_bars();
		//Doorbell
		nvmev_proc_dbs();

		//schedule_timeout();
		//schedule_timeout(round_jiffies_relative(HZ));
		schedule_timeout(usecs_to_jiffies(1));
	}

	return 0;
}

int nvmev_args_verify(void) {
	unsigned long resv_start_bytes;
	unsigned long resv_end_bytes;

	if(!memmap_start) {
		NVMEV_ERROR("[memmap_start] should be specified\n");
		return -1;
	}

	if(!memmap_size) {
		NVMEV_ERROR("[memmap_size] should be specified\n");
		return -1;
	}
	else if(memmap_size == 1) {
		NVMEV_ERROR("[memmap_size] should be bigger than 1MiB\n");
		return -1;
	}

	resv_start_bytes = (unsigned long)memmap_start << 30;
	resv_end_bytes = resv_start_bytes + (memmap_size << 20) - 1;

	if(e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RAM) ||
		e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RESERVED_KERN)) {
		NVMEV_ERROR("[mem %#010llx-%#010llx] is usable, not reseved region\n",
		       (unsigned long long) resv_start_bytes,
		       (unsigned long long) resv_end_bytes);
		return -1;
	}

	if(!e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RESERVED)) {
		NVMEV_ERROR("[mem %#010llx-%#010llx] is not reseved region\n",
		       (unsigned long long) resv_start_bytes,
		       (unsigned long long) resv_end_bytes);
		return -1;
	}

	return 0;
}

void NVMEV_REG_PROC_INIT(struct nvmev_dev *vdev) {
	vdev->nvmev_reg_proc = kthread_create(nvmev_kthread_proc_reg, NULL, "nvmev_proc_reg");
	NVMEV_ERROR("Proc IO : %d\n", vdev->config.cpu_nr_proc_reg);
	if(vdev->config.cpu_nr_proc_reg != -1)
		kthread_bind(vdev->nvmev_reg_proc, vdev->config.cpu_nr_proc_reg);
	wake_up_process(vdev->nvmev_reg_proc);
}

void NVMEV_REG_PROC_FINAL(struct nvmev_dev *vdev) {
	if(!IS_ERR_OR_NULL(vdev->nvmev_reg_proc)) {
		kthread_stop(vdev->nvmev_reg_proc);
		vdev->nvmev_reg_proc = NULL;
	}
}

static int NVMeV_init(void){
	
	pr_info("NVMe Virtual Device Initialize Start\n");

	if(nvmev_args_verify() < 0)
		goto ret_err;

	vdev = VDEV_INIT();

	VDEV_SET_ARGS(&vdev->config, 
			memmap_start, memmap_size, 
			read_latency, write_latency, read_bw, write_bw, 
			cpu_mask);

	PCI_HEADER_SETTINGS(vdev, vdev->pcihdr);
	PCI_PMCAP_SETTINGS(vdev->pmcap);
	PCI_MSIXCAP_SETTINGS(vdev->msixcap);
	PCI_PCIECAP_SETTINGS(vdev->pciecap);
	PCI_AERCAP_SETTINGS(vdev->aercap);
	PCI_PCIE_EXTCAP_SETTINGS(vdev->pcie_exp_cap);

	//Create PCI BUS
	vdev->virt_bus = nvmev_create_pci_bus();
	if(!vdev->virt_bus)
		goto ret_err_pci_bus;
	else {
		nvmev_clone_pci_mem(vdev);
	}

	NVMEV_IO_PROC_INIT(vdev);
	NVMEV_REG_PROC_INIT(vdev);

	NVMEV_INFO("Successfully created Virtual NVMe Deivce\n");

    return 0;
	
ret_err_pci_bus:
	VDEV_FINALIZE(vdev);
    return -1;

ret_err:
	return -1;
}

static void NVMeV_exit(void)
{	
	int i;

	NVMEV_REG_PROC_FINAL(vdev);
	NVMEV_IO_PROC_FINAL(vdev);

	if(vdev->virt_bus != NULL) {
		pci_remove_bus(vdev->virt_bus);

		for(i=0; i<vdev->nr_sq; i++) {
			//iounmap
			kfree(vdev->sqes[i]);
		}

		for(i=0; i<vdev->nr_cq; i++) {
			kfree(vdev->cqes[i]);
		}
		
		VDEV_FINALIZE(vdev);
	}
	pr_info("NVMe Virtual Device Close\n");
}

MODULE_LICENSE("Dual BSD/GPL");
module_init(NVMeV_init);
module_exit(NVMeV_exit);
