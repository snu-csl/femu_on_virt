#ifndef _NVMEVIRT_SSD_CONFIG_H
#define _NVMEVIRT_SSD_CONFIG_H

/* SSD Configuration*/
#define SAMSUNG_970PRO 0
#define ZNS_PROTOTYPE 1

#define BASE_SSD   (SAMSUNG_970PRO)

/* Macros for specific setting. Modify these macros for your target */
#if  (BASE_SSD == SAMSUNG_970PRO)
#define NR_NAMESPACES	1 

/* NVME_CSI_NVM : Conv
   NVME_CSI_ZNS : ZNS
   NS_CAPACITY : MB (0 -> Full capacity) */

#define NS_CSI_0 NVME_CSI_NVM
#define NS_CAPACITY_0 (0) 
#define NS_CSI_1 NVME_CSI_NVM 
#define NS_CAPACITY_1 (0)

#define SSD_PARTITIONS       (4)
#define SSD_PARTITION_BITS   (2) 
#define NAND_CHANNELS        (8)
#define LUNS_PER_NAND_CH     (2)
#define PLNS_PER_LUN         (1)
#define FLASH_PAGE_SIZE      (32*1024)
#define ONESHOT_PAGE_SIZE    (FLASH_PAGE_SIZE * 1)
#define BLKS_PER_PLN         (8192) 
#define BLK_SIZE             (0) /*BLKS_PER_PLN should not be 0 */

#define MAX_CH_XFER_SIZE   (16*1024) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE    (512) 

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			   (3360ull)//MB/s

#define NAND_4KB_READ_LATENCY (35760)  //ns
#define NAND_READ_LATENCY     (36013)
#define NAND_PROG_LATENCY     (185000 + 5000)
#define NAND_ERASE_LATENCY    (0)

#define FW_4KB_READ_LATENCY   (25510 - 17010)
#define FW_READ_LATENCY       (30326 - 19586)
#define FW_WBUF_LATENCY0      (4000)
#define FW_WBUF_LATENCY1      (460)
#define FW_CH_XFER_LATENCY    (0)
#define OP_AREA_PERCENT       (0.07)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * ONESHOT_PAGE_SIZE * 2)
#define WRITE_EARLY_COMPLETION   1

/***Don't touch. Not used ******/
#define ZONE_SIZE       (1) //byte
#define DIES_PER_ZONE   (0)

#define MAX_ZRWA_ZONES (0) /* 0 : Not support ZRWA */
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE   (0)
#define ZRWA_BUFFER_SIZE   (0)
/****************************/
#elif (BASE_SSD == ZNS_PROTOTYPE)
#define NR_NAMESPACES	1 

/* NVME_CSI_NVM : Conv
   NVME_CSI_ZNS : ZNS
   NS_CAPACITY : MB (0 -> Full capacity) */

#define NS_CSI_0 NVME_CSI_ZNS
#define NS_CAPACITY_0 (0) 
#define NS_CSI_1 NVME_CSI_NVM 
#define NS_CAPACITY_1 (0)

#define SSD_PARTITIONS        (1)
#define SSD_PARTITION_BITS    (0)
#define NAND_CHANNELS         (8)
#define LUNS_PER_NAND_CH      (16)
#define FLASH_PAGE_SIZE       (64*1024)
#define PLNS_PER_LUN          (1) /* not used*/
#define DIES_PER_ZONE         (1)

#if 0 /*Real device configuration. Need to modify kernel to support zone size which is power of 2*/
#define ONESHOT_PAGE_SIZE     (FLASH_PAGE_SIZE * 3)
#define ZONE_SIZE             (96*1024*1024) //byte. kernal only support zone size which is power of 2  
#else /*If kernel is not modified, use this config for just testing ZNS*/
#define ONESHOT_PAGE_SIZE     (FLASH_PAGE_SIZE * 2)
#define ZONE_SIZE             (32*1024*1024) 
#endif

#define MAX_CH_XFER_SIZE    (FLASH_PAGE_SIZE) /* to overlap with pcie transfer */
#define WRITE_UNIT_SIZE     (ONESHOT_PAGE_SIZE) 

#define NAND_CHANNEL_BANDWIDTH	(800ull) //MB/s
#define PCIE_BANDWIDTH			   (3200ull) //MB/s

#define NAND_4KB_READ_LATENCY (25485)
#define NAND_READ_LATENCY (40950)
#define NAND_PROG_LATENCY (1913640)
#define NAND_ERASE_LATENCY (0)

#define FW_4KB_READ_LATENCY (37540 - 7390 + 2000)
#define FW_READ_LATENCY     (37540 - 7390 + 2000)
#define FW_WBUF_LATENCY0    (0)
#define FW_WBUF_LATENCY1    (0)
#define FW_CH_XFER_LATENCY  (413)
#define OP_AREA_PERCENT     (0)

#define WRITE_BUFFER_SIZE   (NAND_CHANNELS * LUNS_PER_NAND_CH * ONESHOT_PAGE_SIZE * 2)
#define WRITE_EARLY_COMPLETION   0

/*Don't touch. BLK_SIZE is caculated by ZONE_SIZE, DIES_PER_ZONE*/
#define BLKS_PER_PLN         0 /* BLK_SIZE should not be 0 */
#define BLK_SIZE             (ZONE_SIZE / DIES_PER_ZONE)

static_assert((ZONE_SIZE % DIES_PER_ZONE) == 0);

/*for ZRWA */
#define MAX_ZRWA_ZONES (0) 
#define ZRWAFG_SIZE (0)
#define ZRWA_SIZE   (0)
#define ZRWA_BUFFER_SIZE   (0)
#endif // BASE_SSD == ZNS_PROTOTYPE
///////////////////////////////////////////////////////////////////////////

static const uint32_t ns_csi[] = {NS_CSI_0, NS_CSI_1};
static const uint64_t ns_capacity[] = {NS_CAPACITY_0, NS_CAPACITY_1}; // MB

#define NS_CSI(ns) (ns_csi[ns])
#define NS_CAPACITY(ns) (ns_capacity[ns])

#if NR_NAMESPACES >= 3
	#error "ONLY SUPPORT NR_NAMESPACES <= 2"
#elif NR_NAMESPACES == 2
	#if NS_CSI_0 == NS_CSI_1 
		#error "ONLY SUPPORT 1 ZNS Namepsace, 1 Conv Namespace"
	#endif
#endif


static_assert((ONESHOT_PAGE_SIZE % FLASH_PAGE_SIZE) == 0);
#endif
