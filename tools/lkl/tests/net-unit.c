// SPDX-License-Identifier: GPL-2.0
#include <string.h>
#include <lkl.h>
#include <lkl_host.h>

#include "test.h"

struct lkl_host_operations lkl_host_ops;
char lkl_virtio_devs[4096];

static struct lkl_netdev fake_nd;
static struct lkl_dev_net_ops fake_ops;
static __lkl__u8 current_mac[LKL_ETH_ALEN];
static __lkl__u8 ioctl_mac[LKL_ETH_ALEN];
static __lkl__u8 backend_mac[LKL_ETH_ALEN];
static int backend_ret;
static int backend_present;
static int ioctl_get_name_called;
static int ioctl_get_hwaddr_called;
static int ioctl_set_hwaddr_called;
static int backend_called;

static void reset_fake_net_state(void)
{
	memset(&fake_nd, 0, sizeof(fake_nd));
	memset(current_mac, 0, sizeof(current_mac));
	memset(ioctl_mac, 0, sizeof(ioctl_mac));
	memset(backend_mac, 0, sizeof(backend_mac));
	backend_ret = 0;
	backend_present = 1;
	ioctl_get_name_called = 0;
	ioctl_get_hwaddr_called = 0;
	ioctl_set_hwaddr_called = 0;
	backend_called = 0;
	fake_nd.ops = &fake_ops;
}

static int fake_set_mac(struct lkl_netdev *nd, const unsigned char *addr)
{
	backend_called++;
	memcpy(backend_mac, addr, LKL_ETH_ALEN);
	return backend_ret;
}

long lkl_syscall(long no, long *params)
{
	switch (no) {
	case __lkl__NR_socket:
		return 3;
	case __lkl__NR_ioctl: {
		unsigned long req = params[1];
		struct lkl_ifreq *ifr = (struct lkl_ifreq *)params[2];

		if (req == LKL_SIOCGIFNAME) {
			ioctl_get_name_called++;
			strcpy(ifr->lkl_ifr_name, "eth0");
			return 0;
		}
		if (req == LKL_SIOCGIFHWADDR) {
			ioctl_get_hwaddr_called++;
			memcpy(ifr->lkl_ifr_hwaddr.sa_data, current_mac,
			       LKL_ETH_ALEN);
			return 0;
		}
		if (req == LKL_SIOCSIFHWADDR) {
			ioctl_set_hwaddr_called++;
			memcpy(ioctl_mac, ifr->lkl_ifr_hwaddr.sa_data,
			       LKL_ETH_ALEN);
			return 0;
		}
		return -LKL_EINVAL;
	}
	case __lkl__NR_close:
		return 0;
	default:
		return -LKL_ENOSYS;
	}
}

struct lkl_netdev *lkl_netdev_get_by_ifindex(int ifindex)
{
	if (!backend_present)
		return NULL;

	return &fake_nd;
}

static int lkl_test_if_set_mac_updates_backend_hook(void)
{
	__lkl__u8 wanted[LKL_ETH_ALEN] = {0, 0x12, 0x34, 0x56, 0x78, 0x9a};
	int ret;

	reset_fake_net_state();
	fake_ops.set_mac = fake_set_mac;

	ret = lkl_if_set_mac(5, wanted);
	if (ret < 0)
		return TEST_FAILURE;

	if (ioctl_get_name_called != 1 || ioctl_get_hwaddr_called != 1 ||
	    ioctl_set_hwaddr_called != 1 || backend_called != 1 ||
	    memcmp(ioctl_mac, wanted, LKL_ETH_ALEN) != 0 ||
	    memcmp(backend_mac, wanted, LKL_ETH_ALEN) != 0 ||
	    memcmp(fake_nd.mac, wanted, LKL_ETH_ALEN) != 0) {
		lkl_test_logf("backend MAC hook was not updated correctly\n");
		return TEST_FAILURE;
	}

	return TEST_SUCCESS;
}

static int lkl_test_if_set_mac_skips_backend_when_netdev_missing(void)
{
	__lkl__u8 wanted[LKL_ETH_ALEN] = {0, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
	int ret;

	reset_fake_net_state();
	fake_ops.set_mac = fake_set_mac;
	backend_present = 0;

	ret = lkl_if_set_mac(5, wanted);
	if (ret < 0)
		return TEST_FAILURE;

	if (ioctl_set_hwaddr_called != 1 || backend_called != 0) {
		lkl_test_logf("missing netdev should skip backend update\n");
		return TEST_FAILURE;
	}

	return TEST_SUCCESS;
}

static int lkl_test_if_set_mac_returns_backend_error(void)
{
	__lkl__u8 wanted[LKL_ETH_ALEN] = {0, 0xde, 0xad, 0xbe, 0xef, 0x01};
	int ret;

	reset_fake_net_state();
	fake_ops.set_mac = fake_set_mac;
	backend_ret = -LKL_EIO;

	ret = lkl_if_set_mac(5, wanted);
	if (ret != -LKL_EIO) {
		lkl_test_logf("backend error was not propagated\n");
		return TEST_FAILURE;
	}

	if (backend_called != 1 ||
	    memcmp(fake_nd.mac, "\0\0\0\0\0\0", LKL_ETH_ALEN) != 0) {
		lkl_test_logf("backend failure should leave cached MAC unchanged\n");
		return TEST_FAILURE;
	}

	return TEST_SUCCESS;
}

struct lkl_test tests[] = {
	LKL_TEST(if_set_mac_updates_backend_hook),
	LKL_TEST(if_set_mac_skips_backend_when_netdev_missing),
	LKL_TEST(if_set_mac_returns_backend_error),
};

int main(int argc, const char **argv)
{
	return lkl_test_run(tests, sizeof(tests) / sizeof(struct lkl_test),
			    "net-unit");
}
