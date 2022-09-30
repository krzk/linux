// SPDX-License-Identifier: GPL-2.0-only

#include <kunit/test.h>
#include <linux/errno.h>

#include "slimbus.h"

struct slim_present_rate_test_entry {
	int rate;
	int expected;
};

static const struct slim_present_rate_test_entry slim_present_rate_test_data[] = {
	{ 0,		-EINVAL },
	{ 12000,	0x80 | 0x01 },
	{ 24000,	0x80 | 0x02 },
	{ 48000,	0x80 | 0x03 },
	{ 96000,	0x80 | 0x04 },
	{ 192000,	0x80 | 0x05 },
	{ 384000,	0x80 | 0x06 },
	{ 768000,	0x80 | 0x07 },
	{ 11025,	0x80 | 0x09 },
	{ 22050,	0x80 | 0x0a },
	{ 44100,	0x80 | 0x0b },
	{ 88200,	0x80 | 0x0c },
	{ 176400,	0x80 | 0x0d },
	{ 352800,	0x80 | 0x0e },
	{ 705600,	0x80 | 0x0f },
	{ 4000,		0x80 | 0x10 },
	{ 8000,		0x80 | 0x11 },
	{ 16000,	0x80 | 0x12 },
	{ 32000,	0x80 | 0x13 },
	{ 64000,	0x80 | 0x14 },
	{ 128000,	0x80 | 0x15 },
	{ 256000,	0x80 | 0x16 },
	{ 512000,	0x80 | 0x17 },
	{ 4000+1,	0x80 | 0x10 },
	{ 4000+1000,	0x80 | 0x10 },
	{ 12000-1,	0x80 | 0x01 },
	{ 12000+1,	0x80 | 0x01 },
	{ 24000-1,	0x80 | 0x02 },
	{ 24000+1,	0x80 | 0x02 },
	{ 48000-1000,	0x80 | 0x03 },
	{ 48000-1,	0x80 | 0x03 },
	{ 48000+1,	0x80 | 0x03 },
	{ 48000+1000,	0x80 | 0x03 },
	{ 96000-1,	0x80 | 0x04 },
	{ 96000+1,	0x80 | 0x04 },
	{ 768000-5000,	0x80 | 0x07 },
	{ 768000+5000,	0x80 | 0x07 },
	{ 11025-1,	0x80 | 0x09 },
	{ 11025+1,	0x80 | 0x09 },
	{ 22050-1,	0x80 | 0x0a },
	{ 22050+1,	0x80 | 0x0a },
	{ 44100-1,	0x80 | 0x0b },
	{ 44100+1,	0x80 | 0x0b },
};

static void slim_present_rate(struct kunit *test)
{
	const struct slim_present_rate_test_entry *item;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(slim_present_rate_test_data); i++) {
		item = &slim_present_rate_test_data[i];
		KUNIT_EXPECT_EQ_MSG(test, slim_get_prate_code(item->rate),
				    item->expected, "rate %d Hz does not match\n",
				    item->rate);
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
