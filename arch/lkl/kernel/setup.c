#include <linux/binfmts.h>
#include <linux/init.h>
#include <linux/init_task.h>
#include <linux/personality.h>
#include <linux/reboot.h>
#include <linux/fs.h>
#include <linux/start_kernel.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/syscalls.h>
#include <linux/sysinfo.h>
#include <linux/tick.h>
#include <asm/host_ops.h>
#include <asm/irq.h>
#include <asm/page.h>
#include <asm/unistd.h>
#include <asm/setup.h>
#include <asm/syscalls.h>
#include <asm/cpu.h>
#include <linux/screen_info.h>

struct lkl_host_operations *lkl_ops;
static char cmd_line[COMMAND_LINE_SIZE];
static void *init_sem;
static int is_running;
void (*pm_power_off)(void) = NULL;
static unsigned long mem_size = 64 * 1024 * 1024;

struct screen_info screen_info;

static long lkl_panic_blink(int state)
{
	lkl_ops->panic();
	return 0;
}

static int __init setup_mem_size(char *str)
{
	mem_size = memparse(str, NULL);
	return 0;
}
early_param("mem", setup_mem_size);

void __init setup_arch(char **cl)
{
	*cl = cmd_line;
	panic_blink = lkl_panic_blink;
	parse_early_param();
	bootmem_init(mem_size);
}

static void __init lkl_run_kernel(void *arg)
{
	threads_init();
	lkl_cpu_get();
	start_kernel();
}

static char *cmd_line_ptr __initdata = boot_command_line;
static int cmd_line_len __initdata = COMMAND_LINE_SIZE;

static __init void cmd_line_append_va(const char *fmt, va_list ap)
{
	int ret;

	ret = vsnprintf(cmd_line_ptr, cmd_line_len, fmt, ap);

	if (ret > 0) {
		cmd_line_ptr += ret;
		cmd_line_len -= ret;
	}
}

static inline __init void cmd_line_append(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	cmd_line_append_va(fmt, ap);
	va_end(ap);
}

static void __init lkl_report_memory(void)
{
	struct sysinfo info = { 0 };
	u64 image_size = (unsigned long)__lkl_image_size;
	u64 internal_size = lkl_get_internal_memory_size();
	u64 managed_size;
	u64 free_size;
	u64 reserved_size;
	u64 managed_used;
	u64 ram_used;
	u64 total_used;

	si_meminfo(&info);
	if (!info.mem_unit ||
	    check_mul_overflow((u64)info.totalram, (u64)info.mem_unit,
			       &managed_size) ||
	    check_mul_overflow((u64)info.freeram, (u64)info.mem_unit,
			       &free_size) ||
	    free_size > managed_size || managed_size > internal_size) {
		pr_warn("LKL memory accounting failed\n");
		return;
	}

	reserved_size = internal_size - managed_size;
	managed_used = managed_size - free_size;
	ram_used = reserved_size + managed_used;
	if (check_add_overflow(image_size, ram_used, &total_used)) {
		pr_warn("LKL memory accounting overflow\n");
		return;
	}

	pr_info("LKL memory after boot: image_bytes=%llu internal_ram_bytes=%llu reserved_ram_bytes=%llu managed_used_bytes=%llu free_ram_bytes=%llu ram_used_bytes=%llu total_used_bytes=%llu\n",
		(unsigned long long)image_size,
		(unsigned long long)internal_size,
		(unsigned long long)reserved_size,
		(unsigned long long)managed_used,
		(unsigned long long)free_size,
		(unsigned long long)ram_used,
		(unsigned long long)total_used);
	pr_info("LKL memory KiB: image=%llu ram_used=%llu total=%llu\n",
		(unsigned long long)(image_size >> 10),
		(unsigned long long)(ram_used >> 10),
		(unsigned long long)(total_used >> 10));
}

int __init lkl_start_kernel(const char *fmt, ...)
{
	int ret;
	va_list ap;

	cmd_line_append("%s ", CONFIG_BUILTIN_CMDLINE);

	va_start(ap, fmt);
	cmd_line_append_va(fmt, ap);
	va_end(ap);

	if (lkl_ops->virtio_devices)
		cmd_line_append("%s", lkl_ops->virtio_devices);

	memcpy(cmd_line, boot_command_line, COMMAND_LINE_SIZE);

	init_sem = lkl_ops->sem_alloc(0);
	if (!init_sem)
		return -ENOMEM;

	ret = lkl_cpu_init();
	if (ret)
		goto out_free_init_sem;

	ret = lkl_ops->thread_create(lkl_run_kernel, NULL);
	if (!ret) {
		ret = -ENOMEM;
		goto out_free_init_sem;
	}

	lkl_ops->sem_down(init_sem);
	lkl_ops->sem_free(init_sem);
	current_thread_info()->tid = lkl_ops->thread_self();
	lkl_cpu_change_owner(current_thread_info()->tid);

	is_running = 1;
	lkl_report_memory();
	lkl_cpu_put();

	return 0;

out_free_init_sem:
	lkl_ops->sem_free(init_sem);

	return ret;
}

int lkl_is_running(void)
{
	return is_running;
}

void machine_halt(void)
{
	lkl_cpu_shutdown();
}

void machine_power_off(void)
{
	machine_halt();
}

void machine_restart(char *unused)
{
	machine_halt();
}

long lkl_sys_halt(void)
{
	long err;
	long params[6] = {LINUX_REBOOT_MAGIC1,
		LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART, };

	err = lkl_syscall(__NR_reboot, params);
	if (err < 0)
		return err;

	is_running = false;

	lkl_cpu_wait_shutdown();

	syscalls_cleanup();
	threads_cleanup();
	/* Shutdown the clockevents source. */
	tick_suspend_local();
	free_mem();
	lkl_ops->thread_join(current_thread_info()->tid);

	return 0;
}

static int lkl_run_init(struct linux_binprm *bprm);

static struct linux_binfmt lkl_run_init_binfmt = {
	.module		= THIS_MODULE,
	.load_binary	= lkl_run_init,
};

static int lkl_run_init(struct linux_binprm *bprm)
{
	int ret;

	if (strcmp("/init", bprm->filename) != 0)
		return -EINVAL;

	ret = begin_new_exec(bprm);
	if (ret)
		return ret;
	set_personality(PER_LINUX);
	setup_new_exec(bprm);

	set_binfmt(&lkl_run_init_binfmt);

	init_pid_ns.child_reaper = 0;

	syscalls_init();

	lkl_ops->sem_up(init_sem);
	lkl_ops->thread_exit();

	return 0;
}


/* skip mounting the "real" rootfs. ramfs is good enough. */
static int __init fs_setup(void)
{
	int fd;

	// Pad '/init' to make sure it's 8 bytes, otherwise KASan would
	// emit an error. The kernel's strncpy implementation attempts to read
	// 8 bytes at once and, thus, triggers KASan violation for the 6-byte
	// string.
	fd = sys_open("/init\0\0", O_CREAT, 0700);
	WARN_ON(fd < 0);
	sys_close(fd);

	register_binfmt(&lkl_run_init_binfmt);

	return 0;
}
late_initcall(fs_setup);
