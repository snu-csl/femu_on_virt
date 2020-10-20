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

#include "nvmev.h"
#include "nvmev_hdr.h"

struct nvmev_dev *VDEV_INIT(void)
{
	struct nvmev_dev *vdev;
	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);

	vdev->virtDev = kzalloc(PAGE_SIZE, GFP_KERNEL);

	vdev->pcihdr = vdev->virtDev + OFFS_PCI_HDR;
	vdev->pmcap = vdev->virtDev + OFFS_PCI_PM_CAP;
	vdev->msixcap = vdev->virtDev + OFFS_PCI_MSIX_CAP;
	vdev->pciecap = vdev->virtDev + OFFS_PCIE_CAP;
	vdev->aercap = vdev->virtDev + PCI_CFG_SPACE_SIZE;
	vdev->pcie_exp_cap = vdev->virtDev + PCI_CFG_SPACE_SIZE;

	return vdev;
}

void VDEV_FINALIZE(struct nvmev_dev *vdev)
{
	if (vdev->msix_enabled)
		iounmap(vdev->msix_table);

	if (vdev->bar)
		memunmap(vdev->bar);
		//iounmap(vdev->bar);

	if (vdev->old_bar)
		kfree(vdev->old_bar);

	if (vdev->admin_q)
		kfree(vdev->admin_q);

	if (vdev->virtDev)
		kfree(vdev->virtDev);

	if (vdev->io_unit_stat)
		kfree(vdev->io_unit_stat);

	if (vdev)
		kfree(vdev);
}

void PCI_HEADER_SETTINGS(struct pci_header *pcihdr, unsigned long base_pa)
{
	pcihdr->id.did = 0x0101;
	pcihdr->id.vid = 0x0c51;
	/*
	pcihdr->cmd.id = 1;
	pcihdr->cmd.bme = 1;
	pcihdr->cmd.mse = 1;
	*/
	pcihdr->sts.cl = 1;

	pcihdr->htype.mfd = 0;
	pcihdr->htype.hl = PCI_HEADER_TYPE_NORMAL;

	pcihdr->rid = 0x01;

	pcihdr->cc.bcc = PCI_BASE_CLASS_STORAGE;
	pcihdr->cc.scc = 0x08;
	pcihdr->cc.pi = 0x02;

	pcihdr->mlbar.tp = PCI_BASE_ADDRESS_MEM_TYPE_64 >> 1;
	pcihdr->mlbar.ba = (base_pa & 0xFFFFFFFF) >> 14;

	pcihdr->mulbar = base_pa >> 32;

	pcihdr->ss.ssid = 0x370d;
	pcihdr->ss.ssvid = 0x0c51;

	pcihdr->erom = 0x0; //PFN_PHYS(page_to_pfn(bar_pages));//page_to_pfn(bar_pages);//0xDF300000;

	pcihdr->cap = OFFS_PCI_PM_CAP;

	pcihdr->intr.ipin = 1;
	pcihdr->intr.iline = IRQ_NUM;

}

void PCI_PMCAP_SETTINGS(struct pci_pm_cap *pmcap)
{
	pmcap->pid.cid = PCI_CAP_ID_PM;
	pmcap->pid.next = OFFS_PCI_MSIX_CAP;

	pmcap->pc.vs = 3;
	pmcap->pmcs.nsfrst = 1;
	pmcap->pmcs.ps = PCI_PM_CAP_PME_D0 >> 16;
}

void PCI_MSIXCAP_SETTINGS(struct pci_msix_cap *msixcap)
{
	msixcap->mxid.cid = PCI_CAP_ID_MSIX;
	msixcap->mxid.next = OFFS_PCIE_CAP;

	msixcap->mxc.mxe = 1;
	msixcap->mxc.ts = 127; // encoded as n-1

	msixcap->mtab.tbir = 0;
	msixcap->mtab.to = 0x400;

	msixcap->mpba.pbao = 0x1000;
	msixcap->mpba.pbir = 0;
}

void PCI_PCIECAP_SETTINGS(struct pcie_cap *pciecap)
{
	pciecap->pxid.cid = PCI_CAP_ID_EXP;
	pciecap->pxid.next = 0x0;

	pciecap->pxcap.ver = PCI_EXP_FLAGS;
	pciecap->pxcap.imn = 0;
	pciecap->pxcap.dpt = PCI_EXP_TYPE_ENDPOINT;

	pciecap->pxdcap.mps = 1;
	pciecap->pxdcap.pfs = 0;
	pciecap->pxdcap.etfs = 1;
	pciecap->pxdcap.l0sl = 6;
	pciecap->pxdcap.l1l = 2;
	pciecap->pxdcap.rer = 1;
	pciecap->pxdcap.csplv = 0;
	pciecap->pxdcap.cspls = 0;
	pciecap->pxdcap.flrc = 1;
}

void PCI_AERCAP_SETTINGS(struct aer_cap *aercap)
{
	aercap->aerid.cid = PCI_EXT_CAP_ID_ERR;
	aercap->aerid.cver = 1;
	aercap->aerid.next = PCI_CFG_SPACE_SIZE + 0x50;
}

void PCI_PCIE_EXTCAP_SETTINGS(struct pci_exp_hdr *exp_cap)
{
	struct pci_exp_hdr *pcie_exp_cap;

	pcie_exp_cap = exp_cap + 0x50;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_VC;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x80;

	pcie_exp_cap = exp_cap + 0x80;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_PWR;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x90;

	pcie_exp_cap = exp_cap + 0x90;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_ARI;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x170;

	pcie_exp_cap = exp_cap + 0x170;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_DSN;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = PCI_CFG_SPACE_SIZE + 0x1a0;

	pcie_exp_cap = exp_cap + 0x1a0;
	pcie_exp_cap->id.cid = PCI_EXT_CAP_ID_SECPCI;
	pcie_exp_cap->id.cver = 1;
	pcie_exp_cap->id.next = 0;
}
