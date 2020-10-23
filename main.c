/**********************************************************************
 * Copyright (c) 2020
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
#include <linux/delay.h>
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

unsigned long memmap_start = 0;
unsigned long memmap_size = 0;

unsigned int read_time = 1;
unsigned int read_delay = 1;
unsigned int read_trailing = 0;

unsigned int write_time = 1;
unsigned int write_delay = 1;
unsigned int write_trailing = 0;

unsigned int nr_io_units = 8;
unsigned int io_unit_shift = 12;

unsigned long long completion_lag = 0;

char *cpus;

module_param(memmap_start, ulong, 0);
MODULE_PARM_DESC(memmap_start, "Memmap start in GiB");
module_param(memmap_size, ulong, 0);
MODULE_PARM_DESC(memmap_size, "Memmap size in MiB");
module_param(read_time, uint, 0);
MODULE_PARM_DESC(read_time, "Read time in nanoseconds");
module_param(read_delay, uint, 0);
MODULE_PARM_DESC(read_delay, "Read delay in nanoseconds");
module_param(read_trailing, uint, 0);
MODULE_PARM_DESC(read_trailing, "Read trailing in nanoseconds");
module_param(write_time, uint, 0);
MODULE_PARM_DESC(write_time, "Write time in nanoseconds");
module_param(write_delay, uint, 0);
MODULE_PARM_DESC(write_delay, "Write delay in nanoseconds");
module_param(write_trailing, uint, 0);
MODULE_PARM_DESC(write_trailing, "Write trailing in nanoseconds");
module_param(nr_io_units, uint, 0);
MODULE_PARM_DESC(nr_io_units, "Number of I/O units that operate in parallel");
module_param(io_unit_shift, uint, 0);
MODULE_PARM_DESC(io_unit_shift, "Size of each I/O unit (2^)");
module_param(cpus, charp, 0);
MODULE_PARM_DESC(cpus, "CPU list for process, completion(int.) threads, Seperated by Comma(,)");

static void nvmev_proc_dbs(void)
{
	int qid;
	int dbs_idx;
	int new_db;
	int old_db;

	// Admin queue
	new_db = vdev->dbs[0];
	if (new_db != vdev->old_dbs[0]) {
		nvmev_proc_admin_sq(new_db, vdev->old_dbs[0]);
		vdev->old_dbs[0] = new_db;
	}
	new_db = vdev->dbs[1];
	if (new_db != vdev->old_dbs[1]) {
		nvmev_proc_admin_cq(new_db, vdev->old_dbs[1]);
		vdev->old_dbs[1] = new_db;
	}

	// Submission queues
	for (qid = 1; qid <= vdev->nr_sq; qid++) {
		if (vdev->sqes[qid] == NULL) continue;
		dbs_idx = qid * 2;
		new_db = vdev->dbs[dbs_idx];
		old_db = vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_io_sq(qid, new_db, old_db);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}

	// Completion queues
	for (qid = 1; qid <= vdev->nr_cq; qid++) {
		if (vdev->cqes[qid] == NULL) continue;
		dbs_idx = qid * 2 + 1;
		new_db = vdev->dbs[dbs_idx];
		old_db = vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_io_cq(qid, new_db, old_db);
			vdev->old_dbs[dbs_idx] = new_db;
		}
	}
}

static int nvmev_kthread_manager(void *data)
{
	while (!kthread_should_stop()) {
		nvmev_proc_bars();
		nvmev_proc_dbs();

		cond_resched();
	}

	return 0;
}

static void NVMEV_REG_PROC_INIT(struct nvmev_dev *vdev)
{
	vdev->nvmev_manager = kthread_create(nvmev_kthread_manager, NULL, "nvmev_proc_reg");
	if (vdev->config.cpu_nr_proc_reg != -1)
		kthread_bind(vdev->nvmev_manager, vdev->config.cpu_nr_proc_reg);
	wake_up_process(vdev->nvmev_manager);
	NVMEV_INFO("nvmev_proc_reg started on %d\n", vdev->config.cpu_nr_proc_reg);
}

static void NVMEV_REG_PROC_FINAL(struct nvmev_dev *vdev)
{
	if (!IS_ERR_OR_NULL(vdev->nvmev_manager)) {
		kthread_stop(vdev->nvmev_manager);
		vdev->nvmev_manager = NULL;
	}
}


static int __validate_configs(void)
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

	resv_start_bytes = memmap_start << 30;
	resv_end_bytes = resv_start_bytes + (memmap_size << 20) - 1;

	if (e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RAM) ||
		e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RESERVED_KERN)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is usable, not reseved region\n",
		       (unsigned long) resv_start_bytes,
		       (unsigned long) resv_end_bytes);
		return -EPERM;
	}

	if (!e820_any_mapped(resv_start_bytes, resv_end_bytes, E820_RESERVED)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is not reseved region\n",
		       (unsigned long) resv_start_bytes,
		       (unsigned long) resv_end_bytes);
		return -EPERM;
	}

	if (nr_io_units == 0 || io_unit_shift == 0) {
		NVMEV_ERROR("Need non-zero IO unit size and at least one IO unit\n");
		return -EINVAL;
	}
	if (read_time == 0) {
		NVMEV_ERROR("Need non-zero read time\n");
		return -EINVAL;
	}
	if (write_time == 0) {
		NVMEV_ERROR("Need non-zero write time\n");
		return -EINVAL;
	}

	return 0;
}

static void __print_perf_configs(void)
{
	unsigned long unit_perf_kb =
			vdev->config.nr_io_units << (vdev->config.io_unit_shift - 10);
	struct nvmev_config *cfg = &vdev->config;

	NVMEV_INFO("=============== Configurations ===============\n");
	NVMEV_INFO("* IO units : %d x %d\n",
			cfg->nr_io_units, 1 << cfg->io_unit_shift);
	NVMEV_INFO("* I/O times\n");
	NVMEV_INFO("  Read     : %u + %u x + %u ns\n",
				cfg->read_delay, cfg->read_time, cfg->read_trailing);
	NVMEV_INFO("  Write    : %u + %u x + %u ns\n",
				cfg->write_delay, cfg->write_time, cfg->write_trailing);
	NVMEV_INFO("* Bandwidth\n");
	NVMEV_INFO("  Read     : %lu MiB/s\n",
			(1000000000UL / (cfg->read_time + cfg->read_delay + cfg->read_trailing)) * unit_perf_kb >> 10);
	NVMEV_INFO("  Write    : %lu MiB/s\n",
			(1000000000UL / (cfg->write_time + cfg->write_delay + cfg->write_trailing)) * unit_perf_kb >> 10);
}

static int __get_nr_entries(int dbs_idx, int queue_size)
{
	int diff = vdev->dbs[dbs_idx] - vdev->old_dbs[dbs_idx];
	if (diff < 0) {
		diff += queue_size;
	}
	return diff;
}

static int __proc_file_read(struct seq_file *m, void *data)
{
	const char *filename = m->private;
	struct nvmev_config *cfg = &vdev->config;

	if (strcmp(filename, "read_times") == 0) {
		seq_printf(m, "%u + %u x + %u",
				cfg->read_delay, cfg->read_time, cfg->read_trailing);
	} else if (strcmp(filename, "write_times") == 0) {
		seq_printf(m, "%u + %u x + %u",
				cfg->write_delay, cfg->write_time, cfg->write_trailing);
	} else if (strcmp(filename, "io_units") == 0) {
		seq_printf(m, "%u x %u",
				cfg->nr_io_units, cfg->io_unit_shift);
	} else if (strcmp(filename, "stat") == 0) {
		int i;
		unsigned int nr_in_flight = 0;
		unsigned int nr_dispatch = 0;
		unsigned int nr_dispatched = 0;
		unsigned long long total_io = 0;
		for (i = 1; i < vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = vdev->sqes[i];
			if (!sq) continue;

			seq_printf(m, "%2d: %2u %4u %4u %4u %4u %llu\n", i,
					__get_nr_entries(i * 2, sq->queue_size),
					sq->stat.nr_in_flight,
					sq->stat.max_nr_in_flight,
					sq->stat.nr_dispatch,
					sq->stat.nr_dispatched,
					sq->stat.total_io);

			nr_in_flight += sq->stat.nr_in_flight;
			nr_dispatch += sq->stat.nr_dispatch;
			nr_dispatched  += sq->stat.nr_dispatched;
			total_io += sq->stat.total_io;

			barrier();
			sq->stat.max_nr_in_flight = 0;
		}
		seq_printf(m, "total: %u %u %u %llu\n", nr_in_flight, nr_dispatch, nr_dispatched, total_io);
	} else if (strcmp(filename, "debug") == 0) {
		seq_printf(m, "%llu", completion_lag);
	}

	return 0;
}

static ssize_t __proc_file_write(struct file *file, const char __user *buf, size_t len, loff_t *offp)
{
	ssize_t count = len;
	const char *filename = file->f_path.dentry->d_name.name;
	char input[128];
	unsigned int ret;
	unsigned long long *old_stat;
	struct nvmev_config *cfg = &vdev->config;

	copy_from_user(input, buf, min(len, sizeof(input)));

	if (!strcmp(filename, "read_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->read_delay, &cfg->read_time, &cfg->read_trailing);
	} else if (!strcmp(filename, "write_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->write_delay, &cfg->write_time, &cfg->write_trailing);
	} else if (!strcmp(filename, "io_units")) {
		ret = sscanf(input, "%d %d", &cfg->nr_io_units, &cfg->io_unit_shift);
		if (ret < 1) goto out;

		old_stat = vdev->io_unit_stat;
		vdev->io_unit_stat = kzalloc(
				sizeof(*vdev->io_unit_stat) * cfg->nr_io_units, GFP_KERNEL);

		mdelay(100);	/* XXX: Delay the free of old stat so that outstanding
						 * requests accessing the unit_stat are all returned
						 */
		kfree(old_stat);
	} else if (!strcmp(filename, "debug") == 0) {

	}

out:
	__print_perf_configs();

	return count;
}

static int __proc_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, __proc_file_read,
			(char *)file->f_path.dentry->d_name.name);
}

static const struct file_operations proc_file_fops = {
	.owner = THIS_MODULE,
	.open = __proc_file_open,
	.write = __proc_file_write,
	.read	= seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

void NVMEV_STORAGE_INIT(struct nvmev_dev *vdev)
{
	NVMEV_INFO("Storage : %lx + %lx\n",
			vdev->config.storage_start, vdev->config.storage_size);

	vdev->io_unit_stat = kzalloc(
			sizeof(*vdev->io_unit_stat) * vdev->config.nr_io_units, GFP_KERNEL);

	vdev->storage_mapped = memremap(vdev->config.storage_start,
			vdev->config.storage_size, MEMREMAP_WB);
	if (vdev->storage_mapped == NULL)
		NVMEV_ERROR("Failed to map storage memory.\n");

	vdev->proc_root = proc_mkdir("nvmev", NULL);
	vdev->proc_read_times = proc_create(
			"read_times", 0664, vdev->proc_root, &proc_file_fops);
	vdev->proc_write_times = proc_create(
			"write_times", 0664, vdev->proc_root, &proc_file_fops);
	vdev->proc_io_units = proc_create(
			"io_units", 0664, vdev->proc_root, &proc_file_fops);
	vdev->proc_stat = proc_create(
			"stat", 0444, vdev->proc_root, &proc_file_fops);
	vdev->proc_stat = proc_create(
			"debug", 0444, vdev->proc_root, &proc_file_fops);
}

void NVMEV_STORAGE_FINAL(struct nvmev_dev *vdev)
{
	remove_proc_entry("read_times", vdev->proc_root);
	remove_proc_entry("write_times", vdev->proc_root);
	remove_proc_entry("io_units", vdev->proc_root);
	remove_proc_entry("stat", vdev->proc_root);
	remove_proc_entry("debug", vdev->proc_root);

	remove_proc_entry("nvmev", NULL);

	if (vdev->storage_mapped)
		memunmap(vdev->storage_mapped);

	if (vdev->io_unit_stat)
		kfree(vdev->io_unit_stat);
}

static bool __load_configs(struct nvmev_config *config)
{
	bool first = true;
	unsigned int cpu_nr;
	char *cpu;

	if (__validate_configs() < 0) {
		return false;
	}

	config->memmap_start = memmap_start << 30;
	config->memmap_size = memmap_size << 20;
	config->storage_start = config->memmap_start + (1UL << 20);
	config->storage_size = (memmap_size - 1) << 20;

	config->read_time = read_time;
	config->read_delay = read_delay;
	config->read_trailing = read_trailing;
	config->write_time = write_time;
	config->write_delay = write_delay;
	config->write_trailing = write_trailing;
	config->nr_io_units = nr_io_units;
	config->io_unit_shift = io_unit_shift;

	config->nr_io_cpu = 0;
	config->cpu_nr_proc_reg = -1;
	config->cpu_nr_proc_io = kcalloc(sizeof(unsigned int), 32, GFP_KERNEL);

	while ((cpu = strsep(&cpus, ",")) != NULL) {
		cpu_nr = (unsigned int)simple_strtol(cpu, NULL, 10);
		if (first) {
			config->cpu_nr_proc_reg = cpu_nr;
		} else {
			config->cpu_nr_proc_io[config->nr_io_cpu] = cpu_nr;
			config->nr_io_cpu++;
		}
		first = false;
	}

	return true;
}

static int NVMeV_init(void)
{
	vdev = VDEV_INIT();
	if (!vdev) return -EINVAL;

	if (!__load_configs(&vdev->config)) {
		goto ret_err;
	}

	if (!NVMEV_PCI_INIT(vdev)) {
		goto ret_err;
	}

	__print_perf_configs();

	NVMEV_STORAGE_INIT(vdev);

	NVMEV_IO_PROC_INIT(vdev);
	NVMEV_REG_PROC_INIT(vdev);

	NVMEV_INFO("Successfully created Virtual NVMe deivce\n");

    return 0;

ret_err:
	VDEV_FINALIZE(vdev);
    return -EIO;
}

static void NVMeV_exit(void)
{
	NVMEV_REG_PROC_FINAL(vdev);
	NVMEV_IO_PROC_FINAL(vdev);

	NVMEV_STORAGE_FINAL(vdev);

	if (vdev->virt_bus != NULL) {
		int i;
		pci_remove_root_bus(vdev->virt_bus);

		for (i = 0; i < vdev->nr_sq; i++) {
			kfree(vdev->sqes[i]);
		}

		for (i = 0; i < vdev->nr_cq; i++) {
			kfree(vdev->cqes[i]);
		}
	}
	VDEV_FINALIZE(vdev);

	NVMEV_INFO("Virtual NVMe device closed\n");
}

MODULE_LICENSE("Dual BSD/GPL");
module_init(NVMeV_init);
module_exit(NVMeV_exit);