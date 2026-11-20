// SPDX-License-Identifier: GPL-2.0-only
// Run production lens functions at the I2C and regulator call boundaries.
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define clamp_t(t, v, lo, hi) ((t)(v) < (lo) ? (lo) : (t)(v) > (hi) ? (hi) : (t)(v))
#define V4L2_CID_FOCUS_ABSOLUTE 1

struct i2c_client { int unused; };
struct device { void *data; };
struct v4l2_subdev { struct device *dev; };
struct v4l2_subdev_fh;
struct mutex { bool locked; };
struct v4l2_ctrl_handler { int unused; };
struct v4l2_ctrl {
	int id, val;
	struct { int val; } cur;
	struct v4l2_ctrl_handler *handler;
};
struct regulator_bulk_data { const char *supply; };

static struct i2c_client client;
static unsigned int writes, fail_at, fail_from, enables, disables, waits, errors;
static int enable_error, disable_error, pm_state, pm_refs;
static unsigned int hardware_position, reads;
static int read_error;
static bool write_accepted_on_error, high_byte_accepted_on_error;
static unsigned int positions[64], position_count, registers[16];
static unsigned int register_writes[8][2], register_write_count;
static unsigned long last_wait;

static void mutex_lock(struct mutex *lock) { assert(!lock->locked); lock->locked = true; }
static void mutex_unlock(struct mutex *lock) { assert(lock->locked); lock->locked = false; }
static void *dev_get_drvdata(struct device *dev) { return dev->data; }
static struct i2c_client *v4l2_get_subdevdata(struct v4l2_subdev *sd)
{ (void)sd; return &client; }

static int i2c_smbus_write_word_swapped(struct i2c_client *c, u8 reg, u16 value)
{
	assert(c == &client && reg == 3 && value <= 1023);
	if (++writes == fail_at || (fail_from && writes >= fail_from)) {
		if (writes == fail_at || writes == fail_from) {
			if (write_accepted_on_error)
				hardware_position = value;
			if (high_byte_accepted_on_error)
				hardware_position = (value & 0x300) | (hardware_position & 0xff);
		}
		return -EIO;
	}
	hardware_position = value;
	assert(position_count < ARRAY_SIZE(positions));
	positions[position_count++] = value;
	last_wait = 0;
	return 0;
}

static int i2c_smbus_read_word_swapped(struct i2c_client *c, u8 reg)
{
	assert(c == &client && reg == 3 && last_wait >= 10000);
	reads++;
	if (read_error || (fail_from && writes >= fail_from))
		return read_error ? read_error : -EIO;
	return hardware_position;
}

static int i2c_smbus_write_byte_data(struct i2c_client *c, u8 reg, u8 value)
{
	assert(c == &client && reg < ARRAY_SIZE(registers));
	if (++writes == fail_at || (fail_from && writes >= fail_from))
		return -EIO;
	assert(register_write_count < ARRAY_SIZE(register_writes));
	register_writes[register_write_count][0] = reg;
	register_writes[register_write_count++][1] = value;
	registers[reg] = value;
	return 0;
}

static int regulator_bulk_enable(unsigned int count, struct regulator_bulk_data *s)
{
	assert(count == 2 && !strcmp(s[0].supply, "vin") && !strcmp(s[1].supply, "vdd"));
	enables++;
	return enable_error;
}

static int regulator_bulk_disable(unsigned int count, struct regulator_bulk_data *s)
{
	assert(count == 2 && s && last_wait >= 10000);
	disables++;
	return disable_error;
}

static void usleep_range(unsigned long min, unsigned long max)
{ assert(max >= min); last_wait = min; waits++; }
static void dev_err(struct device *dev, const char *format, ...)
{ (void)dev; (void)format; errors++; }
static int pm_runtime_get_if_active(struct device *dev)
{ (void)dev; if (pm_state > 0) pm_refs++; return pm_state; }
static int pm_runtime_resume_and_get(struct device *dev)
{ (void)dev; assert(pm_state == 1); pm_refs++; return 0; }
static void pm_runtime_put_autosuspend(struct device *dev)
{ (void)dev; assert(pm_refs == 1); pm_refs--; }

#include "gt9764-driver.h"

static struct dw9768 lens;
static struct device dev;
static struct v4l2_ctrl focus;

static void fresh(unsigned int position)
{
	memset(&lens, 0, sizeof(lens));
	memset(registers, 0, sizeof(registers));
	focus = (struct v4l2_ctrl){ .id = V4L2_CID_FOCUS_ABSOLUTE,
		.val = 512, .cur.val = 512, .handler = &lens.ctrls };
	dev.data = &lens.sd;
	lens.sd.dev = &dev;
	lens.focus = &focus;
	lens.position = position;
	lens.position_valid = true;
	lens.configured = true;
	lens.powered = true;
	hardware_position = position;
	lens.supplies[0].supply = "vin";
	lens.supplies[1].supply = "vdd";
	writes = fail_at = fail_from = enables = disables = waits = errors = position_count = 0;
	register_write_count = 0;
	enable_error = disable_error = pm_refs = 0;
	read_error = reads = 0;
	write_accepted_on_error = high_byte_accepted_on_error = false;
	pm_state = 1;
	last_wait = 0;
}

static void fresh_unpowered(void)
{
	fresh(512);
	lens.powered = false;
	lens.configured = false;
	lens.position_valid = false;
}

int main(void)
{
	/* Both sides of the bidirectional DAC must approach neutral monotonically. */
	for (unsigned int start = 0; start <= 1023; start++) {
		unsigned int previous = start;

		fresh(start);
		assert(!gt9764_suspend(&dev));
		assert(!lens.powered && lens.position == 512 && disables == 1);
		for (unsigned int i = 0; i < position_count; i++) {
			unsigned int value = positions[i];

			assert(value >= (start < 512 ? previous : 512));
			assert(value <= (start < 512 ? 512 : previous));
			assert((value > previous ? value - previous : previous - value) <= 50);
			previous = value;
		}
		assert(!lens.lock.locked && waits >= position_count);
	}
	for (unsigned int failed = 1; failed <= 11; failed++) {
		fresh(0);
		fail_at = failed;
		assert(gt9764_suspend(&dev) == -EBUSY && lens.powered && !disables);
		assert(lens.position == (failed - 1) * 50 && errors == 1);
		assert(!lens.position_valid && lens.configured);
		fail_at = 0;
		assert(!gt9764_suspend(&dev) && lens.position == 512 && !lens.powered);
	}
	fresh(1023);
	disable_error = -EBUSY;
	assert(gt9764_suspend(&dev) == -EBUSY && lens.powered && lens.position == 512);
	disable_error = 0;
	assert(!gt9764_suspend(&dev) && !lens.powered);
	assert(!gt9764_suspend(&dev) && disables == 2);

	fresh_unpowered();
	focus.cur.val = 900;
	focus.val = 700; /* A trial or failed write can leave a pending value. */
	assert(!gt9764_resume(&dev) && lens.powered && enables == 1);
	assert(registers[2] == 2 && registers[6] == 0x40 && registers[7] == 0x73);
	const unsigned int expected_init[][2] = { {2, 0}, {2, 2}, {6, 0x40}, {7, 0x73} };

	assert(register_write_count == ARRAY_SIZE(expected_init));
	assert(!memcmp(register_writes, expected_init, sizeof(expected_init)));
	assert(positions[0] == 512 && lens.position == 900);
	assert(!gt9764_suspend(&dev) && !lens.powered && lens.position == 512);
	assert(focus.cur.val == 900 && !gt9764_resume(&dev) && lens.position == 900);

	for (unsigned int failed = 1; failed <= 13; failed++) {
		fresh_unpowered();
		focus.cur.val = 900;
		fail_at = failed;
		assert(gt9764_resume(&dev) == -EIO && !lens.powered && disables == 1);
		assert(hardware_position == 512);
	}

	/* A late failure must not cause another power-on write of 512. */
	fresh_unpowered();
	focus.cur.val = 900;
	fail_from = 13;
	assert(gt9764_resume(&dev) == -EIO && lens.powered && !disables);
	assert(lens.position == 862 && enables == 1 && !lens.position_valid);
	unsigned int before_retry = position_count;

	read_error = -EIO;
	fail_from = 0;
	assert(gt9764_resume(&dev) == -EIO && lens.powered && !disables);
	assert(position_count == before_retry && register_write_count == 4);
	read_error = 0;
	assert(!gt9764_resume(&dev) && lens.powered && lens.position == 900 && enables == 1);
	assert(positions[before_retry] == 900 && register_write_count == 4);

	/* The high byte of a failed word may have reached the chip. */
	fresh_unpowered();
	focus.cur.val = 900;
	fail_from = 11; /* 762 (0x2fa) -> 812 (0x32c). */
	high_byte_accepted_on_error = true;
	assert(gt9764_resume(&dev) == -EIO && lens.powered && !disables);
	assert(lens.position == 762 && hardware_position == 1018);
	before_retry = position_count;
	fail_from = 0;
	assert(!gt9764_resume(&dev) && hardware_position == 900 && reads == 2);
	assert(positions[before_retry] == 968 && positions[before_retry + 1] == 918);

	/* Reopen may skip runtime_resume when a failed park left PM active. */
	fresh(862);
	focus.cur.val = 862;
	fail_at = 1;
	write_accepted_on_error = true;
	assert(gt9764_suspend(&dev) == -EBUSY && hardware_position == 812);
	assert(!gt9764_open(&lens.sd, NULL) && pm_refs == 1);
	assert(reads == 1 && hardware_position == 862 && register_write_count == 0);
	pm_runtime_put_autosuspend(&dev);

	/* A bulk-disable failure may have reset configuration and position. */
	fresh(512);
	focus.cur.val = 800;
	disable_error = -EIO;
	assert(gt9764_suspend(&dev) == -EBUSY && lens.powered);
	assert(!lens.configured && !lens.position_valid);
	hardware_position = 768;
	assert(!gt9764_open(&lens.sd, NULL) && pm_refs == 1);
	assert(reads == 1 && hardware_position == 800 && register_write_count == 4);
	assert(positions[0] == 800);
	pm_runtime_put_autosuspend(&dev);

	fresh_unpowered();
	enable_error = -EIO;
	assert(gt9764_resume(&dev) == -EIO && !lens.powered && !writes && !disables);

	fresh(512);
	focus.val = 700;
	assert(!gt9764_set_ctrl(&focus) && lens.position == 700 && !pm_refs);
	fail_at = writes + 1;
	focus.val = 800;
	assert(gt9764_set_ctrl(&focus) == -EIO && lens.position == 700 && !pm_refs);
	fail_at = 0;
	pm_state = 0;
	unsigned int before = writes;

	assert(!gt9764_set_ctrl(&focus) && writes == before && lens.position == 700);
	pm_state = -EINVAL;
	assert(gt9764_set_ctrl(&focus) == -EINVAL && writes == before);
	puts("GT9764: all DAC positions park at 512; I2C failures retain power and position, "
	     "controls respect runtime PM.");
	return 0;
}
