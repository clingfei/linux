#ifndef _ASM_LKL_IRQ_H
#define _ASM_LKL_IRQ_H

#define IRQ_STATUS_BITS		(sizeof(long) * 8)
#define NR_IRQS			((int)(IRQ_STATUS_BITS * IRQ_STATUS_BITS))

void run_irqs(void);
void set_irq_pending(int irq);
int lkl_get_free_irq_block(const char *user, int nr_irqs);
void lkl_put_irq_block(int irq, int nr_irqs, const char *user);

#include <uapi/asm/irq.h>

#endif
