// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include "slimbus.h"

struct slim_present_rate_test_entry {
	int rate;
	int expected;
};

static const struct slim_present_rate_test_entry slim_present_rate_test_data[] = {
	{ 0,		0 },
	{ 12000,	1 },
	{ 24000,	2 },
	{ 48000,	3 },
	{ 96000,	4 },
	{ 192000,	5 },
	{ 384000,	6 },
	{ 768000,	7 },
	{ 11025,	9 },
	{ 22050,	10 },
	{ 44100,	11 },
	{ 88200,	12 },
	{ 176400,	13 },
	{ 352800,	14 },
	{ 705600,	15 },
	{ 4000,		16 },
	{ 8000,		17 },
	{ 16000,	18 },
	{ 32000,	19 },
	{ 64000,	20 },
	{ 128000,	21 },
	{ 256000,	22 },
	{ 512000,	23 },
	{ 1,	-EINVAL },
};

static void slim_present_rate(struct kunit *test)
{
	const struct slim_present_rate_test_entry *item;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(slim_present_rate_test_data); i++) {
		item = &slim_present_rate_test_data[i];
		KUNIT_EXPECT_EQ(test, slim_get_prate_code(item->rate),
				item->expected);
	}
}

static struct kunit_case slimbus_stream_test_cases[] = {
	KUNIT_CASE(slim_present_rate),
	{}
};

static struct kunit_suite slimbus_stream_test_suite = {
	.name = "slimbus-stream",
	.test_cases = slimbus_stream_test_cases,
};

kunit_test_suite(slimbus_stream_test_suite);

MODULE_LICENSE("GPL");
