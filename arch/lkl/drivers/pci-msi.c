// SPDX-License-Identifier: GPL-2.0
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <asm/host_ops.h>
#include <asm/irq.h>

#define LKL_MSI_USER	"pci-msi"
#define LKL_MSIX_USER	"pci-msix"

static void lkl_pci_msi_ack(struct irq_data *data)
{
}

static struct irq_chip lkl_pci_msi_chip = {
	.name		= "LKL-PCI-MSI",
	.irq_ack	= lkl_pci_msi_ack,
	.irq_mask	= pci_msi_mask_irq,
	.irq_unmask	= pci_msi_unmask_irq,
	.flags		= IRQCHIP_SKIP_SET_WAKE | IRQCHIP_ONESHOT_SAFE,
};

static int lkl_pci_setup_msi_desc(struct msi_desc *desc, unsigned int irq)
{
	int i, ret;

	irq_set_chip_and_handler(irq, &lkl_pci_msi_chip, handle_edge_irq);

	for (i = 0; i < desc->nvec_used; i++) {
		ret = irq_set_msi_desc_off(irq, i, desc);
		if (ret) {
			while (--i >= 0)
				irq_set_msi_desc_off(irq, i, NULL);
			desc->irq = 0;
			return ret;
		}
	}

	return 0;
}

static void lkl_pci_clear_msi_desc(struct msi_desc *desc)
{
	unsigned int irq = desc->irq;
	int i;

	for (i = 0; i < desc->nvec_used; i++)
		irq_set_msi_desc_off(irq, i, NULL);

	desc->irq = 0;
}

static int lkl_pci_setup_msi_irqs(struct pci_dev *dev, int nvec)
{
	struct msi_desc *desc;
	int *irqs, base, i, ret;

	desc = msi_first_desc(&dev->dev, MSI_DESC_NOTASSOCIATED);
	if (!desc || desc->nvec_used != nvec)
		return -EINVAL;

	base = lkl_get_free_irq_block(LKL_MSI_USER, nvec);
	if (base < 0)
		return base;

	irqs = kcalloc(nvec, sizeof(*irqs), GFP_KERNEL);
	if (!irqs) {
		lkl_put_irq_block(base, nvec, LKL_MSI_USER);
		return -ENOMEM;
	}

	for (i = 0; i < nvec; i++)
		irqs[i] = base + i;

	ret = lkl_pci_setup_msi_desc(desc, base);
	if (ret)
		goto err_free_irq;

	ret = lkl_ops->pci_ops->msi_init(dev->sysdata, LKL_PCI_IRQ_MSI,
					 nvec, irqs);
	if (ret)
		goto err_clear_desc;

	kfree(irqs);
	return 0;

err_clear_desc:
	lkl_pci_clear_msi_desc(desc);
err_free_irq:
	lkl_put_irq_block(base, nvec, LKL_MSI_USER);
	kfree(irqs);
	return ret;
}

static int lkl_pci_setup_msix_irqs(struct pci_dev *dev)
{
	struct msi_desc *desc;
	int *irqs, irq, nvec = 0, ret;

	msi_for_each_desc(desc, &dev->dev, MSI_DESC_NOTASSOCIATED)
		nvec = max_t(int, nvec, desc->msi_index + 1);

	if (!nvec)
		return -EINVAL;

	irqs = kcalloc(nvec, sizeof(*irqs), GFP_KERNEL);
	if (!irqs)
		return -ENOMEM;

	msi_for_each_desc(desc, &dev->dev, MSI_DESC_NOTASSOCIATED) {
		irq = lkl_get_free_irq(LKL_MSIX_USER);
		if (irq < 0) {
			ret = irq;
			goto err_clear;
		}

		ret = lkl_pci_setup_msi_desc(desc, irq);
		if (ret) {
			lkl_put_irq(irq, LKL_MSIX_USER);
			goto err_clear;
		}

		irqs[desc->msi_index] = irq;
	}

	ret = lkl_ops->pci_ops->msi_init(dev->sysdata, LKL_PCI_IRQ_MSIX,
					 nvec, irqs);
	if (ret)
		goto err_clear;

	kfree(irqs);
	return 0;

err_clear:
	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED) {
		irq = desc->irq;
		lkl_pci_clear_msi_desc(desc);
		lkl_put_irq(irq, LKL_MSIX_USER);
	}
	kfree(irqs);
	return ret;
}

int arch_setup_msi_irqs(struct pci_dev *dev, int nvec, int type)
{
	if (!lkl_ops->pci_ops || !lkl_ops->pci_ops->msi_init)
		return -ENODEV;

	switch (type) {
	case PCI_CAP_ID_MSI:
		return lkl_pci_setup_msi_irqs(dev, nvec);
	case PCI_CAP_ID_MSIX:
		return lkl_pci_setup_msix_irqs(dev);
	default:
		return -EINVAL;
	}
}

void arch_teardown_msi_irqs(struct pci_dev *dev)
{
	struct msi_desc *desc;

	if (lkl_ops->pci_ops && lkl_ops->pci_ops->msi_teardown) {
		if (dev->msix_enabled)
			lkl_ops->pci_ops->msi_teardown(dev->sysdata,
						       LKL_PCI_IRQ_MSIX);
		else if (dev->msi_enabled)
			lkl_ops->pci_ops->msi_teardown(dev->sysdata,
						       LKL_PCI_IRQ_MSI);
	}

	msi_for_each_desc(desc, &dev->dev, MSI_DESC_ASSOCIATED) {
		if (desc->pci.msi_attrib.is_msix) {
			unsigned int irq = desc->irq;

			lkl_pci_clear_msi_desc(desc);
			lkl_put_irq(irq, LKL_MSIX_USER);
		} else {
			unsigned int irq = desc->irq;

			lkl_pci_clear_msi_desc(desc);
			lkl_put_irq_block(irq, desc->nvec_used, LKL_MSI_USER);
		}
	}
}

bool arch_restore_msi_irqs(struct pci_dev *dev)
{
	return false;
}
