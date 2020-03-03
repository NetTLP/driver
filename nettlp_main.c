#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/err.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>

#include "nettlp_msg.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
#include <linux/pci-p2pdma.h>
#endif

#define DRV_NAME	"nettlp"
#define NETTLP_VERSION	"0.0.3"

static bool disable_msg_socket = false;
module_param(disable_msg_socket, bool, 0644);
MODULE_PARM_DESC(disable_msg_socket, "Disable NetTLP Message Module");

struct mmio {
	void *virt;
	uint64_t start;
	uint64_t end;
	uint64_t flags;
	uint64_t len;
};

struct nettlp {
	struct mmio bar0;
	struct mmio bar2;
	struct mmio bar4;
};


/* structure describing nettlp device */
struct nettlp_dev {
	struct nettlp dev;
	int num_vec;
	bool irq_allocated[NETTLP_MAX_VEC];

	struct nettlp_msix msix[NETTLP_MAX_VEC];
};



static void nettlp_store_bar_info(struct pci_dev *pdev,
				  struct mmio *bar, int barn, bool do_ioremap)
{
	memset(bar, 0, sizeof(*bar));
	bar->start = pci_resource_start(pdev, barn);
	bar->end   = pci_resource_end(pdev, barn);
	bar->flags = pci_resource_flags(pdev, barn);
	bar->len   = pci_resource_len(pdev, barn);

	pr_info("BAR%d start  : %#llx\n", barn, bar->start);
	pr_info("BAR%d end    : %#llx\n", barn, bar->end);
	pr_info("BAR%d flags  : 0x%llx\n", barn, bar->flags);
	pr_info("BAR%d len    : %llu\n", barn, bar->len);

	if (do_ioremap) {
		bar->virt = ioremap(bar->start, bar->len);
		if (!bar->virt)
			pr_err("failed to map BAR%d\n", barn);
		else
			pr_info("BAR%d mapped : %p\n", barn, bar->virt);
	}
}


static void unregister_interrupts(struct nettlp_dev *nt, struct pci_dev *pdev)
{
	int irq;

	for (irq = 0; irq < NETTLP_MAX_VEC; irq++) {
		if (nt->irq_allocated[irq])
			free_irq(pci_irq_vector(pdev, irq), nt);
		nt->irq_allocated[irq] = false;
	}
}

static irqreturn_t interrupt_handler(int irq, void *nic_irq)
{
	pr_info("Interrupt! irq=%d\n", irq);

	return IRQ_HANDLED;
}

static int register_interrupts(struct nettlp_dev *nt, struct pci_dev *pdev)
{
	int ret, irq;

	nt->num_vec = NETTLP_NUM_VEC;

	// Enable MSI-X
	ret = pci_alloc_irq_vectors(pdev, nt->num_vec, nt->num_vec,
				    PCI_IRQ_MSIX);
	if (ret < 0) {
		pr_info("Request for #%d msix vectors failed, returned %d\n",
		nt->num_vec, ret);
		return 1;
	}

	// register interrupt handler 
	for (irq = 0; irq < nt->num_vec; irq++) {
		ret = request_irq(pci_irq_vector(pdev, irq),
				  interrupt_handler, 0, DRV_NAME, nt);
		if (ret)
			return 1;
		nt->irq_allocated[irq] = true;
	}

	return 0;
}


static int nettlp_pci_init(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	struct nettlp_dev *nt;
	int rc, n;

	pr_info("%s: register nettlp device %s\n", __func__, pci_name(pdev));

	nt = devm_kzalloc(&pdev->dev, sizeof(*nt), GFP_KERNEL);
	if (!nt)
		return -ENOMEM;

	pci_set_drvdata(pdev, nt);

	rc = pci_enable_device(pdev);
	if (rc)
		goto error;

	rc = pci_request_regions(pdev, DRV_NAME);
	if (rc)
		goto error;

	/* set BUS Master Mode */
	pci_set_master(pdev);

	/* BAR0 (pcie pio) */
	nettlp_store_bar_info(pdev, &nt->dev.bar0, 0, false);

	/* BAR2 (MSIX table) */
	nettlp_store_bar_info(pdev, &nt->dev.bar2, 2, true);

	/* BAR4 (pseudo memory-dependent) */
	nettlp_store_bar_info(pdev, &nt->dev.bar4, 4, false);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 20, 0)
	pr_info("Allocate BAR4 as p2pdma memory\n");
	rc = pci_p2pdma_add_resource(pdev, 4, nt->dev.bar4.len, 0);
	if (rc) {
		pr_err("failed to register BAR4 as p2pdma resource\n");
		goto error;
	}
	pci_p2pmem_publish(pdev, true);
#endif

	/* XXX: Register MSIX interrupts.
	 * This basic NetTLP driver does not use MSIX, but we 
	 * implement it here as an exmaple use.
	 */
	rc = register_interrupts(nt, pdev);
	if (rc)
		goto error_interrupts;

	rc = nettlp_msg_fill_msix_table(nt->dev.bar2.virt, nt->msix);
	if (rc)
		pr_err("failed to get MSIX table from BAR2\n");
	else {
		for (n = 0; n < NETTLP_MAX_VEC; n++) {
			pr_info("MSIX [%d]: Addr=%#llx, Data=%08x\n",
				n, nt->msix[n].addr, nt->msix[n].data);
		}
	}

	/* initialize nettlp_msg module */
	if (!disable_msg_socket) {
		nettlp_msg_init(nt->dev.bar4.start,
				PCI_DEVID(pdev->bus->number, pdev->devfn),
				nt->dev.bar2.virt);
	} else
		pr_warn("nettlp message socket is disabled by option\n");

	return 0;

error_interrupts:
	unregister_interrupts(nt, pdev);
	pci_free_irq_vectors(pdev);
	nt->num_vec = 0;
error:
	pr_info("nettlp_pci_init error\n");
	pci_set_drvdata(pdev, NULL);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
	return -1;
}

static void nettlp_pci_remove(struct pci_dev *pdev)
{
	struct nettlp_dev *nt = pci_get_drvdata(pdev);

	pr_info("%s: remove nettlp device %s\n", __func__, pci_name(pdev));

	if (!disable_msg_socket)
		nettlp_msg_fini();

	/* iounmap BAR2 where MSIX table is located */
	if (nt->dev.bar2.virt)
		iounmap(nt->dev.bar2.virt);

	pci_set_drvdata(pdev, NULL);

	unregister_interrupts(nt, pdev);
	pci_free_irq_vectors(pdev);
	nt->num_vec = 0;

	pci_release_regions(pdev);
	pci_disable_device(pdev);
}


static const struct pci_device_id nettlp_pci_tbl[] = {
	{0x3776, 0x8022, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0 },
	{0,}
};
MODULE_DEVICE_TABLE(pci, nettlp_pci_tbl);

struct pci_driver nettlp_pci_driver = {
	.name = DRV_NAME,
	.id_table = nettlp_pci_tbl,
	.probe = nettlp_pci_init,
	.remove = nettlp_pci_remove,
	//	.suspend = nettlp_suspend,
	//	.resume = nettlp_resume,
};

static int __init nt_init(void)
{
	pr_info("nettlp (v%s) is loaded\n", NETTLP_VERSION);

	return pci_register_driver(&nettlp_pci_driver);
}
module_init(nt_init);


static void __exit nt_release(void)
{
	pci_unregister_driver(&nettlp_pci_driver);
	pr_info("nettlp (v%s) is unloaded\n", NETTLP_VERSION);
	return;
}
module_exit(nt_release);


MODULE_AUTHOR("Yohei Kuga <sora@haeena.net>");
MODULE_DESCRIPTION("nettlp");
MODULE_LICENSE("GPL");
MODULE_VERSION(NETTLP_VERSION);

