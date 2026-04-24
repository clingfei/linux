// SPDX-License-Identifier: GPL-2.0
#define LKL_HOST_CONFIG_VIRTIO_NET_DPDK y

#include <string.h>
#include <stdlib.h>
#include <lkl.h>
#include <lkl_host.h>
#include <lkl_config.h>

#include "test.h"

static const char *dpdk_config_with_mac_json =
"{\n"
"	\"interfaces\": [\n"
"		{\n"
"			\"type\":\"dpdk\",\n"
"			\"param\":\"dpdk0\",\n"
"			\"mac\": \"aa:bb:cc:dd:ee:ff\"\n"
"		}\n"
"	]\n"
"}\n";

static const char *dpdk_config_without_mac_json =
"{\n"
"	\"interfaces\": [\n"
"		{\n"
"			\"type\":\"dpdk\",\n"
"			\"param\":\"dpdk0\"\n"
"		}\n"
"	]\n"
"}\n";

struct lkl_host_operations lkl_host_ops;
char lkl_virtio_devs[4096];

static struct lkl_netdev fake_nd;
static __lkl__u8 detected_mac[LKL_ETH_ALEN];
static __lkl__u8 added_mac[LKL_ETH_ALEN];
static __lkl__u8 applied_mac[LKL_ETH_ALEN];
static int add_called;
static int add_has_mac;
static int create_called;
static int set_mac_called;
static int if_up_called;
static int call_order;
static int set_mac_order;
static int if_up_order;

static void reset_fake_netdev_state(void)
{
	memset(&fake_nd, 0, sizeof(fake_nd));
	memset(detected_mac, 0, sizeof(detected_mac));
	memset(added_mac, 0, sizeof(added_mac));
	memset(applied_mac, 0, sizeof(applied_mac));
	add_called = 0;
	add_has_mac = 0;
	create_called = 0;
	set_mac_called = 0;
	if_up_called = 0;
	call_order = 0;
	set_mac_order = 0;
	if_up_order = 0;
}

static int load_config(struct lkl_config *cfg, const char *json)
{
	memset(cfg, 0, sizeof(*cfg));
	return lkl_load_config_json(cfg, json);
}

struct lkl_netdev *lkl_netdev_dpdk_create(const char *ifname, int offload,
					  unsigned char *mac)
{
	create_called++;
	if (mac)
		memcpy(mac, detected_mac, LKL_ETH_ALEN);
	return &fake_nd;
}

int lkl_netdev_add(struct lkl_netdev *nd, struct lkl_netdev_args *args)
{
	add_called++;
	add_has_mac = !!(args && args->mac);
	if (add_has_mac) {
		memcpy(added_mac, args->mac, LKL_ETH_ALEN);
		memcpy(nd->mac, args->mac, LKL_ETH_ALEN);
	}

	return 0;
}

int lkl_netdev_get_ifindex(int id)
{
	return 17;
}

int lkl_if_set_mac(int ifindex, void *addr)
{
	set_mac_called++;
	set_mac_order = ++call_order;
	memcpy(applied_mac, addr, LKL_ETH_ALEN);
	return 0;
}

int lkl_if_up(int ifindex)
{
	if_up_called++;
	if_up_order = ++call_order;
	return 0;
}

void lkl_netdev_remove(int id)
{
}

void lkl_netdev_free(struct lkl_netdev *nd)
{
}

#include "../lib/config.c"

static int lkl_test_config_pre_prefers_configured_dpdk_mac(void)
{
	struct lkl_config cfg;
	__lkl__u8 expected[LKL_ETH_ALEN] = {0xaa, 0xbb, 0xcc,
					    0xdd, 0xee, 0xff};
	__lkl__u8 fallback[LKL_ETH_ALEN] = {0, 0x11, 0x22,
					    0x33, 0x44, 0x55};

	reset_fake_netdev_state();
	memcpy(detected_mac, fallback, LKL_ETH_ALEN);

	if (load_config(&cfg, dpdk_config_with_mac_json) < 0)
		return TEST_FAILURE;
	if (lkl_load_config_pre(&cfg) < 0)
		return TEST_FAILURE;

	if (!create_called || !add_called || !add_has_mac ||
	    memcmp(added_mac, expected, LKL_ETH_ALEN) != 0) {
		lkl_test_logf("configured MAC did not override detected MAC\n");
		return TEST_FAILURE;
	}

	lkl_unload_config(&cfg);
	return TEST_SUCCESS;
}

static int lkl_test_config_pre_uses_detected_dpdk_mac_as_fallback(void)
{
	struct lkl_config cfg;
	__lkl__u8 expected[LKL_ETH_ALEN] = {0, 0x11, 0x22,
					    0x33, 0x44, 0x55};

	reset_fake_netdev_state();
	memcpy(detected_mac, expected, LKL_ETH_ALEN);

	if (load_config(&cfg, dpdk_config_without_mac_json) < 0)
		return TEST_FAILURE;
	if (lkl_load_config_pre(&cfg) < 0)
		return TEST_FAILURE;

	if (!create_called || !add_called || !add_has_mac ||
	    memcmp(added_mac, expected, LKL_ETH_ALEN) != 0) {
		lkl_test_logf("detected MAC was not used as fallback\n");
		return TEST_FAILURE;
	}

	lkl_unload_config(&cfg);
	return TEST_SUCCESS;
}

static int lkl_test_config_post_applies_configured_mac_before_if_up(void)
{
	struct lkl_config cfg;
	__lkl__u8 expected[LKL_ETH_ALEN] = {0xaa, 0xbb, 0xcc,
					    0xdd, 0xee, 0xff};

	reset_fake_netdev_state();
	memcpy(detected_mac, "\x00\x11\x22\x33\x44\x55", LKL_ETH_ALEN);

	if (load_config(&cfg, dpdk_config_with_mac_json) < 0)
		return TEST_FAILURE;
	if (lkl_load_config_pre(&cfg) < 0)
		return TEST_FAILURE;
	if (lkl_load_config_post(&cfg) < 0)
		return TEST_FAILURE;

	if (set_mac_called != 1 || if_up_called != 1 ||
	    memcmp(applied_mac, expected, LKL_ETH_ALEN) != 0 ||
	    set_mac_order >= if_up_order) {
		lkl_test_logf("configured MAC was not applied before if_up\n");
		return TEST_FAILURE;
	}

	lkl_unload_config(&cfg);
	return TEST_SUCCESS;
}

static int lkl_test_config_post_skips_mac_apply_without_configured_mac(void)
{
	struct lkl_config cfg;
	__lkl__u8 fallback[LKL_ETH_ALEN] = {0, 0x11, 0x22,
					    0x33, 0x44, 0x55};

	reset_fake_netdev_state();
	memcpy(detected_mac, fallback, LKL_ETH_ALEN);

	if (load_config(&cfg, dpdk_config_without_mac_json) < 0)
		return TEST_FAILURE;
	if (lkl_load_config_pre(&cfg) < 0)
		return TEST_FAILURE;
	if (lkl_load_config_post(&cfg) < 0)
		return TEST_FAILURE;

	if (set_mac_called != 0 || if_up_called != 1) {
		lkl_test_logf("detected fallback MAC should not be re-applied\n");
		return TEST_FAILURE;
	}

	lkl_unload_config(&cfg);
	return TEST_SUCCESS;
}

struct lkl_test tests[] = {
	LKL_TEST(config_pre_prefers_configured_dpdk_mac),
	LKL_TEST(config_pre_uses_detected_dpdk_mac_as_fallback),
	LKL_TEST(config_post_applies_configured_mac_before_if_up),
	LKL_TEST(config_post_skips_mac_apply_without_configured_mac),
};

int main(int argc, const char **argv)
{
	return lkl_test_run(tests, sizeof(tests) / sizeof(struct lkl_test),
			    "config-unit");
}
