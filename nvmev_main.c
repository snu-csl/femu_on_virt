#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/pci_ids.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
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
 * 3. Read Latency (export to sysfs, nano seconds)
 * 4. Write Latency (export to sysfs, nano seconds)
 * 5. Read BW
 * 6. Write BW
 * 7. CPU Mask
 ****************************************************************/

struct nvmev_dev *vdev = NULL;
EXPORT_SYMBOL(vdev);

unsigned int memmap_start = 0;
unsigned int memmap_size = 0;
unsigned int read_latency = 0;
unsigned int write_latency = 0;
unsigned int read_bw = 0;
unsigned int write_bw = 0;
char *cpus;

module_param(memmap_start, uint, 0);
MODULE_PARM_DESC(memmap_start, "Memmap start in GiB");
module_param(memmap_size, uint, 0);
MODULE_PARM_DESC(memmap_size, "Memmap size in MiB");
module_param(read_latency, uint, 0);
MODULE_PARM_DESC(read_latency, "Read latency in nanoseconds");
module_param(write_latency, uint, 0);
MODULE_PARM_DESC(write_latency, "Write latency in nanoseconds");
module_param(read_bw, uint, 0);
MODULE_PARM_DESC(read_bw, "Max read bandwidth (MiB/s)");
module_param(write_bw, uint, 0);
MODULE_PARM_DESC(write_bw, "Max write bandwidth (MiB/s)");
module_param(cpus, charp, 0);
MODULE_PARM_DESC(cpus, "CPU list for process, completion(int.) threads, Seperated by Comma(,)");

static void nvmev_proc_dbs(void)
{
	int qid;
	int dbs_idx;
	int new_db;
	int old_db;

	// Admin Queue
	new_db = vdev->dbs[0];
	if (new_db != vdev->old_dbs[0]) {
		nvmev_proc_sq_admin(new_db, vdev->old_dbs[0]);
		vdev->old_dbs[0] = new_db;
	}
	new_db = vdev->dbs[1];
	if (new_db != vdev->old_dbs[1]) {
		nvmev_proc_cq_admin(new_db, vdev->old_dbs[1]);
		vdev->old_dbs[1] = new_db;
	}

	// Submission Queue
	for (qid = 1; qid <= vdev->nr_sq; qid++) {
		if (vdev->sqes[qid] == NULL) continue;
		dbs_idx = qid * 2;
		new_db = vdev->dbs[dbs_idx];
		old_db = vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_sq_io(qid, new_db, old_db);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}

	// Completion Queue
	for (qid = 1; qid <= vdev->nr_cq; qid++) {
		if (vdev->cqes[qid] == NULL) continue;
		dbs_idx = qid * 2 + 1;
		new_db = vdev->dbs[dbs_idx];
		old_db = vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_cq_io(qid, new_db, old_db);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}
}

static int nvmev_kthread_proc_reg(void *data)
{
	while (!kthread_should_stop()) {
		// BAR Register Check
		nvmev_proc_bars();
		//Doorbell
		nvmev_proc_dbs();

		//schedule_timeout();
		//schedule_timeout(round_jiffies_relative(HZ));
		cond_resched();
		//schedule_timeout(nsecs_to_jiffies(1));
	}

	return 0;
}

int nvmev_args_verify(void)
{
	unsigned long resv_start_bytes;
	unsigned long resv_end_bytes;

	if (!memmap_start) {
		NVMEV_ERROR("[memmap_start] should be specified\n");
		return -EINVAL;
	}

	if (!memmap_size) {
		NVMEV_ERROR("[memmap_size] should be specified\n");
		return -EINVAL;
	}
	else if (memmap_size == 1) {
		NVMEV_ERROR("[memmap_size] should be bigger than 1MiB\n");
		return -EINVAL;
	}

	resv_start_bytes = (unsigned long)memmap_start << 30;
	resv_end_bytes = resv_start_bytes + ((unsigned long)memmap_size << 20) - 1;

	if (e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RAM) ||
		e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RESERVED_KERN)) {
		NVMEV_ERROR("[mem %#010llx-%#010llx] is usable, not reseved region\n",
		       (unsigned long long) resv_start_bytes,
		       (unsigned long long) resv_end_bytes);
		return -EPERM;
	}

	if (!e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RESERVED)) {
		NVMEV_ERROR("[mem %#010llx-%#010llx] is not reseved region\n",
		       (unsigned long long) resv_start_bytes,
		       (unsigned long long) resv_end_bytes);
		return -EPERM;
	}

	return 0;
}

void NVMEV_REG_PROC_INIT(struct nvmev_dev *vdev)
{
	vdev->nvmev_reg_proc = kthread_create(nvmev_kthread_proc_reg, NULL, "nvmev_proc_reg");
	NVMEV_INFO("Proc IO : %d\n", vdev->config.cpu_nr_proc_reg);
	if (vdev->config.cpu_nr_proc_reg != -1)
		kthread_bind(vdev->nvmev_reg_proc, vdev->config.cpu_nr_proc_reg);
	wake_up_process(vdev->nvmev_reg_proc);
}

void NVMEV_REG_PROC_FINAL(struct nvmev_dev *vdev)
{
	if (!IS_ERR_OR_NULL(vdev->nvmev_reg_proc)) {
		kthread_stop(vdev->nvmev_reg_proc);
		vdev->nvmev_reg_proc = NULL;
	}
}

void print_perf_configs(void)
{
	NVMEV_INFO("=============== Configure Change =============\n");
	NVMEV_INFO("* Latency\n");
	NVMEV_INFO("  Read     : %u (ns)\n", vdev->config.read_latency);
	NVMEV_INFO("  Write    : %u (ns)\n", vdev->config.write_latency);
	NVMEV_INFO("* Bandwidth\n");
	NVMEV_INFO("  Read     : %lu (MiB/s)\n", vdev->config.read_bw);
	NVMEV_INFO("             %lu (B/us)\n", vdev->config.read_bw_us);
	NVMEV_INFO("  Write    : %lu (MiB/s)\n", vdev->config.write_bw);
	NVMEV_INFO("             %lu (B/us)\n", vdev->config.write_bw_us);
	NVMEV_INFO("* IO depth : %d\n", vdev->nr_unit);
}

static int __get_nr_entries(int dbs_idx, int queue_size)
{
	int diff = vdev->dbs[dbs_idx] - vdev->old_dbs[dbs_idx];
	if (diff < 0) {
		diff += queue_size;
	}
	return diff;
}

static ssize_t proc_file_read(struct file *filp, char *buf, size_t len, loff_t *offp)
{
	const char *fname = filp->f_path.dentry->d_name.name;
	if (*offp) return 0;
	buf[0] = '\0';

	if (strcmp(fname, "read_latency") == 0) {
		snprintf(buf, len, "%u", vdev->config.read_latency);
	} else if (strcmp(fname, "write_latency") == 0) {
		snprintf(buf, len, "%u", vdev->config.write_latency);
	} else if (strcmp(fname, "slot") == 0) {
		snprintf(buf, len, "%u", vdev->nr_unit);
	} else if (strcmp(fname, "stat") == 0) {
		int offset = 0, i;
		unsigned int nr_in_flight = 0;
		unsigned int nr_dispatch = 0;
		unsigned int nr_dispatched = 0;
		unsigned long long total_io = 0;
		for (i = 1; i < vdev->nr_sq; i++) {
			offset += snprintf(buf + offset, len - offset,
					"%u %u %u %u %u %llu ",
					__get_nr_entries(i * 2, vdev->sqes[i]->queue_size),
					vdev->sq_stats[i].nr_in_flight,
					vdev->sq_stats[i].max_nr_in_flight,
					vdev->sq_stats[i].nr_dispatch,
					vdev->sq_stats[i].nr_dispatched,
					vdev->sq_stats[i].total_io);

			nr_in_flight += vdev->sq_stats[i].nr_in_flight;
			nr_dispatch += vdev->sq_stats[i].nr_dispatch;
			nr_dispatched  += vdev->sq_stats[i].nr_dispatched;
			total_io += vdev->sq_stats[i].total_io;

			barrier();
			vdev->sq_stats[i].max_nr_in_flight = 0;
		}
		offset += snprintf(buf + offset, len - offset, " / %u %u %u %llu", nr_in_flight, nr_dispatch, nr_dispatched, total_io);
	}
	*offp += strlen(buf);
	return *offp;
}
static ssize_t proc_file_write(struct file *filp,const char *buf,size_t len, loff_t *offp)
{
	ssize_t count = len;
	const char* fname = filp->f_path.dentry->d_name.name;
	char *endptr;
	char input[128];
	unsigned int *val = PDE_DATA(filp->f_inode);
	unsigned int newval;
	unsigned long long *old_stat;
	bool force_slot = false;
	copy_from_user(input, buf, len);

	newval = simple_strtol(input, &endptr, 10);

	*val = newval;

	if (!strcmp(fname, "read_bw")) {
		vdev->config.read_bw = newval;
		vdev->config.read_bw_us = (unsigned long)((newval << 20) / 1000000);
	}
	else if (!strcmp(fname, "write_bw")) {
		vdev->config.write_bw = newval;
		vdev->config.write_bw_us = (unsigned long)((newval << 20) / 1000000);
	}
	else if (!strcmp(fname, "read_latency")) {
		vdev->config.read_latency = newval;
	}
	else if (!strcmp(fname, "write_latency")) {
		vdev->config.write_latency = newval;
	}
	else if (!strcmp(fname, "slot")) {
		vdev->config.read_bw = newval * 4 * (1000000000 / vdev->config.read_latency) / 1024;
		vdev->config.read_bw_us = (unsigned long long)((vdev->config.read_bw << 20) / 1000000);

		vdev->config.write_bw = newval * 4 * (1000000000 / vdev->config.write_latency) / 1024;
		vdev->config.write_bw_us = (unsigned long long)((vdev->config.write_bw << 20) / 1000000);
		force_slot = true;
	}
	if (!force_slot) {
		int us_per_page = DIV_ROUND_UP(4096, vdev->config.read_bw_us);
		vdev->nr_unit = DIV_ROUND_UP(vdev->config.read_latency, us_per_page);
	}
	old_stat = vdev->unit_stat;
	vdev->unit_stat = kzalloc(sizeof(unsigned long long) * vdev->nr_unit,
			GFP_KERNEL);
	kfree(old_stat);

	print_perf_configs();

	memset(vdev->sq_stats, 0x00, sizeof(vdev->sq_stats));

	return count;
}
static const struct file_operations proc_file_fops = {
	.owner = THIS_MODULE,
	.read = proc_file_read,
	.write = proc_file_write,
};

void NVMEV_STORAGE_INIT(struct nvmev_dev *vdev)
{
	vdev->storage_mapped = memremap(vdev->config.storage_start,
			vdev->config.storage_size, MEMREMAP_WB);
	NVMEV_INFO("Storage : %lu -> %lu\n",
			vdev->config.storage_start, vdev->config.storage_size);
	if (vdev->storage_mapped == NULL)
		NVMEV_ERROR("Storage Memory Remap Error!!!!!\n");

	vdev->proc_root = proc_mkdir("nvmev", NULL);
	vdev->read_latency = proc_create_data(
			"read_latency", 0664, vdev->proc_root,
			&proc_file_fops, &vdev->config.read_latency);
	vdev->write_latency = proc_create_data(
			"write_latency", 0664, vdev->proc_root,
			&proc_file_fops, &vdev->config.write_latency);
	vdev->read_bw = proc_create_data(
			"read_bw", 0664, vdev->proc_root,
			&proc_file_fops, &vdev->config.read_bw);
	vdev->write_bw = proc_create_data(
			"write_bw", 0664, vdev->proc_root,
			&proc_file_fops, &vdev->config.write_bw);
	vdev->slot = proc_create_data(
			"slot", 0664, vdev->proc_root,
			&proc_file_fops, &vdev->nr_unit);
	proc_create_data(
			"stat", 0444, vdev->proc_root,
			&proc_file_fops, vdev);
}

void NVMEV_STORAGE_FINAL(struct nvmev_dev *vdev)
{
	if (vdev->storage_mapped)
		memunmap(vdev->storage_mapped);

	remove_proc_entry("read_latency", vdev->proc_root);
	remove_proc_entry("write_latency", vdev->proc_root);
	remove_proc_entry("read_bw", vdev->proc_root);
	remove_proc_entry("write_bw", vdev->proc_root);
	remove_proc_entry("slot", vdev->proc_root);
	remove_proc_entry("stat", vdev->proc_root);

	remove_proc_entry("nvmev", NULL);
}

static int NVMeV_init(void)
{
	int us_per_page;

	pr_info("NVMe Virtual Device Initialize Start\n");

	if (nvmev_args_verify() < 0)
		goto ret_err;

	vdev = VDEV_INIT();

	VDEV_SET_ARGS(&vdev->config,
			memmap_start, memmap_size,
			read_latency, write_latency, read_bw, write_bw,
			cpus);

	if (!vdev->config.read_bw_us) {
		goto ret_err_pci_bus;
	}

	us_per_page = DIV_ROUND_UP(4096, vdev->config.read_bw_us);
	vdev->nr_unit = DIV_ROUND_UP(vdev->config.read_latency, us_per_page);

	vdev->unit_stat = kzalloc(sizeof(unsigned long long) * vdev->nr_unit,
			GFP_KERNEL);

	PCI_HEADER_SETTINGS(vdev, vdev->pcihdr);
	PCI_PMCAP_SETTINGS(vdev->pmcap);
	PCI_MSIXCAP_SETTINGS(vdev->msixcap);
	PCI_PCIECAP_SETTINGS(vdev->pciecap);
	PCI_AERCAP_SETTINGS(vdev->aercap);
	PCI_PCIE_EXTCAP_SETTINGS(vdev->pcie_exp_cap);

	//Create PCI BUS
	vdev->virt_bus = nvmev_create_pci_bus();
	if (!vdev->virt_bus)
		goto ret_err_pci_bus;
	else {
		nvmev_clone_pci_mem(vdev);
	}

	print_perf_configs();

	NVMEV_STORAGE_INIT(vdev);

	NVMEV_IO_PROC_INIT(vdev);
	NVMEV_REG_PROC_INIT(vdev);

	cpumask_clear(&vdev->first_cpu_on_node);
	cpumask_set_cpu(cpumask_first(cpumask_of_node(vdev->pdev->dev.numa_node)),
			&vdev->first_cpu_on_node);

	NVMEV_INFO("NODE: %d, First CPU: %*pbl\n",
				vdev->pdev->dev.numa_node,
				cpumask_pr_args(&vdev->first_cpu_on_node));

	NVMEV_INFO("Successfully created Virtual NVMe Deivce\n");

    return 0;

ret_err_pci_bus:
	VDEV_FINALIZE(vdev);
    return -EIO;

ret_err:
	return -EINVAL;
}

static void NVMeV_exit(void)
{
	int i;

	NVMEV_STORAGE_FINAL(vdev);
	NVMEV_REG_PROC_FINAL(vdev);
	NVMEV_IO_PROC_FINAL(vdev);

	if (vdev->virt_bus != NULL) {
		pci_remove_bus(vdev->virt_bus);

		for (i = 0; i < vdev->nr_sq; i++) {
			//iounmap
			kfree(vdev->sqes[i]);
		}

		for (i = 0; i < vdev->nr_cq; i++) {
			kfree(vdev->cqes[i]);
		}

		VDEV_FINALIZE(vdev);
	}
	pr_info("NVMe Virtual Device Close\n");
}

MODULE_LICENSE("Dual BSD/GPL");
module_init(NVMeV_init);
module_exit(NVMeV_exit);
