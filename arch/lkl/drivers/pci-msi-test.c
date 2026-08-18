// SPDX-License-Identifier: GPL-2.0
#include <kunit/test.h>
#include <linux/bitfield.h>
#include <linux/irq.h>
#include <linux/log2.h>
#include <linux/msi.h>
#include <linux/pci.h>
#include <asm/host_ops.h>

#define LKL_PCI_MSI_TEST_MAX_VECTORS	8

struct lkl_pci_msi_test_ctx {
	struct pci_dev pdev;
	struct pci_bus bus;
	u8 config[PCI_CFG_SPACE_SIZE];
	bool dev_added;
	struct lkl_dev_pci_ops ops;
	struct lkl_dev_pci_ops *saved_ops;
	unsigned long fake_dev;

	int fail_msi_init;
	int init_calls;
	int teardown_calls;
	int last_type;
	int last_nvec;
	int last_irqs[LKL_PCI_MSI_TEST_MAX_VECTORS];
	struct lkl_pci_dev *last_dev;
};

static int lkl_pci_msi_test_config_bounds(unsigned int devfn, int where,
					   int size)
{
	if (devfn != 0 || where < 0 || size <= 0 || size > 4)
		return PCIBIOS_BAD_REGISTER_NUMBER;
	if ((unsigned int)where > PCI_CFG_SPACE_SIZE - size)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	return PCIBIOS_SUCCESSFUL;
}

static int lkl_pci_msi_test_config_read(struct pci_bus *bus, unsigned int devfn,
					 int where, int size, u32 *val)
{
	struct lkl_pci_msi_test_ctx *ctx = bus->sysdata;
	int ret;

	ret = lkl_pci_msi_test_config_bounds(devfn, where, size);
	if (ret)
		return ret;

	*val = 0;
	memcpy(val, &ctx->config[where], size);
	return PCIBIOS_SUCCESSFUL;
}

static int lkl_pci_msi_test_config_write(struct pci_bus *bus,
					  unsigned int devfn, int where,
					  int size, u32 val)
{
	struct lkl_pci_msi_test_ctx *ctx = bus->sysdata;
	int ret;

	ret = lkl_pci_msi_test_config_bounds(devfn, where, size);
	if (ret)
		return ret;

	memcpy(&ctx->config[where], &val, size);
	return PCIBIOS_SUCCESSFUL;
}

static struct pci_ops lkl_pci_msi_test_pci_ops = {
	.read = lkl_pci_msi_test_config_read,
	.write = lkl_pci_msi_test_config_write,
};

static void lkl_pci_msi_test_config_write_word(struct lkl_pci_msi_test_ctx *ctx,
					       int where, u16 val)
{
	memcpy(&ctx->config[where], &val, sizeof(val));
}

static u16 lkl_pci_msi_test_config_read_word(struct lkl_pci_msi_test_ctx *ctx,
					     int where)
{
	u16 val;

	memcpy(&val, &ctx->config[where], sizeof(val));
	return val;
}

static void lkl_pci_msi_test_release(struct device *dev)
{
}

static int lkl_pci_msi_test_init_op(struct lkl_pci_dev *dev, int type,
				    int nvec, int *irqs)
{
	struct lkl_pci_msi_test_ctx *ctx = (void *)dev;
	int i;

	ctx->init_calls++;
	ctx->last_dev = dev;
	ctx->last_type = type;
	ctx->last_nvec = nvec;
	memset(ctx->last_irqs, 0, sizeof(ctx->last_irqs));

	for (i = 0; i < nvec && i < LKL_PCI_MSI_TEST_MAX_VECTORS; i++)
		ctx->last_irqs[i] = irqs[i];

	return ctx->fail_msi_init;
}

static void lkl_pci_msi_test_teardown_op(struct lkl_pci_dev *dev, int type)
{
	struct lkl_pci_msi_test_ctx *ctx = (void *)dev;

	ctx->teardown_calls++;
	ctx->last_type = type;
}

static int lkl_pci_msi_test_suite_init(struct kunit *test)
{
	struct lkl_pci_msi_test_ctx *ctx;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, lkl_ops);

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ctx);

	ctx->saved_ops = lkl_ops->pci_ops;
	if (ctx->saved_ops)
		ctx->ops = *ctx->saved_ops;
	ctx->ops.msi_init = lkl_pci_msi_test_init_op;
	ctx->ops.msi_teardown = lkl_pci_msi_test_teardown_op;
	lkl_ops->pci_ops = &ctx->ops;

	ctx->bus.ops = &lkl_pci_msi_test_pci_ops;
	ctx->bus.sysdata = ctx;

	device_initialize(&ctx->pdev.dev);
	ctx->pdev.dev.bus = &pci_bus_type;
	ctx->pdev.dev.release = lkl_pci_msi_test_release;
	ctx->pdev.bus = &ctx->bus;
	ctx->pdev.devfn = 0;
	ctx->pdev.msi_cap = 0x50;
	ctx->pdev.sysdata = ctx;
	lkl_pci_msi_test_config_write_word(
		ctx, ctx->pdev.msi_cap + PCI_MSI_FLAGS,
		FIELD_PREP(PCI_MSI_FLAGS_QMASK, 3));

	ret = dev_set_name(&ctx->pdev.dev, "lkl-pci-msi-%p", ctx);
	KUNIT_ASSERT_EQ(test, ret, 0);

	ret = device_add(&ctx->pdev.dev);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ctx->dev_added = true;

	ret = msi_setup_device_data(&ctx->pdev.dev);
	KUNIT_ASSERT_EQ(test, ret, 0);

	test->priv = ctx;
	return 0;
}

static void lkl_pci_msi_test_suite_exit(struct kunit *test)
{
	struct lkl_pci_msi_test_ctx *ctx = test->priv;

	if (ctx->pdev.dev.msi.data) {
		msi_lock_descs(&ctx->pdev.dev);
		arch_teardown_msi_irqs(&ctx->pdev);
		msi_free_msi_descs(&ctx->pdev.dev);
		msi_unlock_descs(&ctx->pdev.dev);
	}

	if (ctx->dev_added)
		device_del(&ctx->pdev.dev);

	lkl_ops->pci_ops = ctx->saved_ops;
	put_device(&ctx->pdev.dev);
}

static void lkl_pci_msi_insert_desc(struct kunit *test,
				    struct lkl_pci_msi_test_ctx *ctx,
				    unsigned int index, unsigned int nvec,
				    bool is_msix)
{
	struct msi_desc desc;
	int ret;

	memset(&desc, 0, sizeof(desc));
	desc.msi_index = index;
	desc.nvec_used = nvec;
	desc.pci.msi_attrib.is_msix = is_msix;
	desc.pci.msi_attrib.multiple = order_base_2(nvec);

	ret = msi_insert_msi_desc(&ctx->pdev.dev, &desc);
	KUNIT_ASSERT_EQ(test, ret, 0);
}

static void lkl_pci_msi_expect_irq_chip_ready(struct kunit *test,
					      unsigned int irq)
{
	struct irq_data *data = irq_get_irq_data(irq);

	KUNIT_ASSERT_NOT_NULL(test, data);
	KUNIT_ASSERT_NOT_NULL(test, data->chip);
	KUNIT_EXPECT_TRUE(test, data->chip->irq_ack != NULL);
	KUNIT_EXPECT_TRUE(test, data->chip->irq_mask != NULL);
	KUNIT_EXPECT_TRUE(test, data->chip->irq_unmask != NULL);
}

static void lkl_pci_msi_allocates_contiguous_irqs(struct kunit *test)
{
	struct lkl_pci_msi_test_ctx *ctx = test->priv;
	struct msi_desc *desc;
	u16 control;
	int ret, i, base;

	msi_lock_descs(&ctx->pdev.dev);
	lkl_pci_msi_insert_desc(test, ctx, 0, 4, false);

	ret = arch_setup_msi_irqs(&ctx->pdev, 4, PCI_CAP_ID_MSI);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->init_calls, 1);
	KUNIT_EXPECT_EQ(test, ctx->last_type, LKL_PCI_IRQ_MSI);
	KUNIT_EXPECT_EQ(test, ctx->last_nvec, 4);
	KUNIT_EXPECT_PTR_EQ(test, ctx->last_dev, (struct lkl_pci_dev *)ctx);

	base = ctx->last_irqs[0];
	KUNIT_EXPECT_GT(test, base, 0);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_EQ(test, ctx->last_irqs[i], base + i);
	lkl_pci_msi_expect_irq_chip_ready(test, base);

	desc = msi_first_desc(&ctx->pdev.dev, MSI_DESC_ASSOCIATED);
	KUNIT_ASSERT_NOT_NULL(test, desc);
	KUNIT_EXPECT_EQ(test, desc->irq, base);
	control = lkl_pci_msi_test_config_read_word(
		ctx, ctx->pdev.msi_cap + PCI_MSI_FLAGS);
	KUNIT_EXPECT_EQ(test, (int)FIELD_GET(PCI_MSI_FLAGS_QSIZE, control),
			(int)desc->pci.msi_attrib.multiple);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_PTR_EQ(test, irq_get_msi_desc(base + i), desc);

	arch_teardown_msi_irqs(&ctx->pdev);
	KUNIT_EXPECT_EQ(test, ctx->teardown_calls, 1);
	KUNIT_EXPECT_EQ(test, ctx->last_type, LKL_PCI_IRQ_MSI);
	for (i = 0; i < 4; i++)
		KUNIT_EXPECT_NULL(test, irq_get_msi_desc(base + i));

	msi_free_msi_descs(&ctx->pdev.dev);
	msi_unlock_descs(&ctx->pdev.dev);
}

static void lkl_pci_msix_preserves_vector_indexes(struct kunit *test)
{
	struct lkl_pci_msi_test_ctx *ctx = test->priv;
	struct msi_desc *desc0, *desc2, *desc5;
	int ret, irq0, irq2, irq5;

	msi_lock_descs(&ctx->pdev.dev);
	lkl_pci_msi_insert_desc(test, ctx, 0, 1, true);
	lkl_pci_msi_insert_desc(test, ctx, 2, 1, true);
	lkl_pci_msi_insert_desc(test, ctx, 5, 1, true);

	ret = arch_setup_msi_irqs(&ctx->pdev, 3, PCI_CAP_ID_MSIX);
	KUNIT_EXPECT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, ctx->init_calls, 1);
	KUNIT_EXPECT_EQ(test, ctx->last_type, LKL_PCI_IRQ_MSIX);
	KUNIT_EXPECT_EQ(test, ctx->last_nvec, 6);

	irq0 = ctx->last_irqs[0];
	irq2 = ctx->last_irqs[2];
	irq5 = ctx->last_irqs[5];
	KUNIT_EXPECT_GT(test, irq0, 0);
	KUNIT_EXPECT_EQ(test, ctx->last_irqs[1], 0);
	KUNIT_EXPECT_GT(test, irq2, 0);
	KUNIT_EXPECT_EQ(test, ctx->last_irqs[3], 0);
	KUNIT_EXPECT_EQ(test, ctx->last_irqs[4], 0);
	KUNIT_EXPECT_GT(test, irq5, 0);
	lkl_pci_msi_expect_irq_chip_ready(test, irq0);
	lkl_pci_msi_expect_irq_chip_ready(test, irq2);
	lkl_pci_msi_expect_irq_chip_ready(test, irq5);

	desc0 = irq_get_msi_desc(irq0);
	desc2 = irq_get_msi_desc(irq2);
	desc5 = irq_get_msi_desc(irq5);
	KUNIT_ASSERT_NOT_NULL(test, desc0);
	KUNIT_ASSERT_NOT_NULL(test, desc2);
	KUNIT_ASSERT_NOT_NULL(test, desc5);
	KUNIT_EXPECT_EQ(test, desc0->msi_index, 0);
	KUNIT_EXPECT_EQ(test, desc2->msi_index, 2);
	KUNIT_EXPECT_EQ(test, desc5->msi_index, 5);

	arch_teardown_msi_irqs(&ctx->pdev);
	KUNIT_EXPECT_EQ(test, ctx->teardown_calls, 1);
	KUNIT_EXPECT_EQ(test, ctx->last_type, LKL_PCI_IRQ_MSIX);
	KUNIT_EXPECT_NULL(test, irq_get_msi_desc(irq0));
	KUNIT_EXPECT_NULL(test, irq_get_msi_desc(irq2));
	KUNIT_EXPECT_NULL(test, irq_get_msi_desc(irq5));

	msi_free_msi_descs(&ctx->pdev.dev);
	msi_unlock_descs(&ctx->pdev.dev);
}

static void lkl_pci_msi_failure_unwinds_associations(struct kunit *test)
{
	struct lkl_pci_msi_test_ctx *ctx = test->priv;
	struct msi_desc *desc;
	int ret;

	ctx->fail_msi_init = -EIO;

	msi_lock_descs(&ctx->pdev.dev);
	lkl_pci_msi_insert_desc(test, ctx, 0, 1, true);
	lkl_pci_msi_insert_desc(test, ctx, 1, 1, true);

	ret = arch_setup_msi_irqs(&ctx->pdev, 2, PCI_CAP_ID_MSIX);
	KUNIT_EXPECT_EQ(test, ret, -EIO);
	KUNIT_EXPECT_EQ(test, ctx->init_calls, 1);
	KUNIT_EXPECT_EQ(test, ctx->teardown_calls, 0);

	desc = msi_first_desc(&ctx->pdev.dev, MSI_DESC_ASSOCIATED);
	KUNIT_EXPECT_NULL(test, desc);
	if (ctx->last_irqs[0] > 0)
		KUNIT_EXPECT_NULL(test, irq_get_msi_desc(ctx->last_irqs[0]));
	if (ctx->last_irqs[1] > 0)
		KUNIT_EXPECT_NULL(test, irq_get_msi_desc(ctx->last_irqs[1]));

	msi_free_msi_descs(&ctx->pdev.dev);
	msi_unlock_descs(&ctx->pdev.dev);
}

static struct kunit_case lkl_pci_msi_test_cases[] = {
	KUNIT_CASE(lkl_pci_msi_allocates_contiguous_irqs),
	KUNIT_CASE(lkl_pci_msix_preserves_vector_indexes),
	KUNIT_CASE(lkl_pci_msi_failure_unwinds_associations),
	{}
};

static struct kunit_suite lkl_pci_msi_test_suite = {
	.name = "lkl_pci_msi",
	.init = lkl_pci_msi_test_suite_init,
	.exit = lkl_pci_msi_test_suite_exit,
	.test_cases = lkl_pci_msi_test_cases,
};

kunit_test_suite(lkl_pci_msi_test_suite);
