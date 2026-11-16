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
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define container_of(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define clamp_t(t, v, lo, hi) ((t)(v) < (lo) ? (lo) : (t)(v) > (hi) ? (hi) : (t)(v))
#define V4L2_CID_FOCUS_ABSOLUTE 1

struct i2c_client { int unused; };
struct device { void *data; };
struct v4l2_subdev { struct device *dev; };
struct mutex { bool locked; };
struct v4l2_ctrl_handler { int unused; };
struct v4l2_ctrl { int id, val; struct v4l2_ctrl_handler *handler; };
struct regulator_bulk_data { const char *supply; };

static struct i2c_client client;
static unsigned int writes, fail_at, fail_from, enables, disables, waits, errors;
static int enable_error, disable_error, pm_state, pm_refs;
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
	if (++writes == fail_at || (fail_from && writes >= fail_from))
		return -EIO;
	assert(position_count < ARRAY_SIZE(positions));
	positions[position_count++] = value;
	last_wait = 0;
	return 0;
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
static void pm_runtime_put_autosuspend(struct device *dev)
{ (void)dev; assert(pm_refs == 1); pm_refs--; }

#include "gt9764-driver.h"

static struct gt9764 lens;
static struct device dev;
static struct v4l2_ctrl focus;

static void fresh(unsigned int position)
{
	memset(&lens, 0, sizeof(lens));
	memset(registers, 0, sizeof(registers));
	focus = (struct v4l2_ctrl){ .id = V4L2_CID_FOCUS_ABSOLUTE,
		.val = 512, .handler = &lens.ctrls };
	dev.data = &lens.sd;
	lens.sd.dev = &dev;
	lens.focus = &focus;
	lens.position = position;
	lens.powered = true;
	for (unsigned int i = 0; i < ARRAY_SIZE(lens.supplies); i++)
		lens.supplies[i].supply = gt9764_supply_names[i];
	writes = fail_at = fail_from = enables = disables = waits = errors = position_count = 0;
	register_write_count = 0;
	enable_error = disable_error = pm_refs = 0;
	pm_state = 1;
	last_wait = 0;
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
		assert(gt9764_suspend(&dev) == -EIO && lens.powered && !disables);
		assert(lens.position == (failed - 1) * 50 && errors == 1);
		fail_at = 0;
		assert(!gt9764_suspend(&dev) && lens.position == 512 && !lens.powered);
	}
	fresh(1023);
	disable_error = -EBUSY;
	assert(gt9764_suspend(&dev) == -EBUSY && lens.powered && lens.position == 512);
	disable_error = 0;
	assert(!gt9764_suspend(&dev) && !lens.powered);
	assert(!gt9764_suspend(&dev) && disables == 2);

	fresh(512);
	lens.powered = false;
	focus.val = 900;
	assert(!gt9764_resume(&dev) && lens.powered && enables == 1);
	assert(registers[2] == 2 && registers[6] == 0x40 && registers[7] == 0x73);
	const unsigned int expected_init[][2] = { {2, 0}, {2, 2}, {6, 0x40}, {7, 0x73} };

	assert(register_write_count == ARRAY_SIZE(expected_init));
	assert(!memcmp(register_writes, expected_init, sizeof(expected_init)));
	assert(positions[0] == 512 && lens.position == 900);
	assert(!gt9764_suspend(&dev) && !lens.powered && lens.position == 512);
	assert(focus.val == 900 && !gt9764_resume(&dev) && lens.position == 900);

	for (unsigned int failed = 1; failed <= 13; failed++) {
		fresh(512);
		lens.powered = false;
		focus.val = 900;
		fail_at = failed;
		assert(gt9764_resume(&dev) == -EIO && !lens.powered && disables == 1);
		assert(lens.position == 512);
	}
	fresh(512);
	lens.powered = false;
	focus.val = 900;
	fail_from = 7;
	assert(gt9764_resume(&dev) == -EIO && lens.powered && !disables);
	assert(lens.position == 562 && enables == 1);
	fail_from = 0;
	assert(!gt9764_resume(&dev) && lens.powered && lens.position == 900 && enables == 1);
	fresh(512);
	lens.powered = false;
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
