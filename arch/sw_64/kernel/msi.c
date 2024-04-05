// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/msi.h>

int msi_compose_msg(unsigned int irq, struct msi_msg *msg)
{
	msg->address_hi = (unsigned int)(MSIX_MSG_ADDR >> 32);
	msg->address_lo = (unsigned int)(MSIX_MSG_ADDR & 0xffffffff);
	msg->data = irq;
	return irq;
}

void sw64_irq_noop(struct irq_data *d)
{
}

void arch_teardown_msi_irq(unsigned int irq)
{
}

static int __init msi_init(void)
{
	return 0;
}

static void __exit msi_exit(void)
{
}

module_init(msi_init);
module_exit(msi_exit);
MODULE_LICENSE("GPL v2");
