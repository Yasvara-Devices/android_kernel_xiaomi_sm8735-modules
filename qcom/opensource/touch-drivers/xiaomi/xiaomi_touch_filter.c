// SPDX-License-Identifier: GPL-2.0-only
/*
 * Input-side edge and grip rejection for GameSpace.
 *
 * On this device TouchReport converts controller frames into points in
 * userspace, then submits those points through UPDATE_REPORT_POINT.  This is
 * the last point at which the OSS stack can reject an accidental touch before
 * it becomes an Android input event.  Keeping the filter here makes its
 * effect independent of the closed touchfeature HAL, which accepts mode 7/15
 * but does not forward either mode to the NT36532 driver under THP.
 */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

#include "xiaomi_touch.h"

#define GAMESPACE_FILTER_MAX_LEVEL 3
#define GAMESPACE_PERMILLE 1000

enum gamespace_reject_reason {
	GAMESPACE_REJECT_NONE,
	GAMESPACE_REJECT_EDGE,
	GAMESPACE_REJECT_GRIP,
};

struct gamespace_touch_filter_state {
	u8 edge_filter;
	u8 grip_suppression;
	u8 orientation;
	unsigned long reported_slots;
	unsigned long suppressed_slots;
	u64 edge_rejected;
	u64 grip_rejected;
};

/*
 * The values are expressed relative to the actual input axis.  For onyx's
 * 1280 px short edge the edge levels resolve to roughly 28/51/81 px.  The
 * balanced grip level mirrors the calibrated 114 x 365 px corner envelope
 * already present in the NT36532 source, without depending on its currently
 * disconnected firmware-grid callback.
 */
static const u16 edge_width_permille[] = { 0, 22, 40, 63 };
static const u16 grip_depth_permille[] = { 0, 132, 180, 250 };
static const u16 grip_width_permille[] = { 0, 89, 120, 160 };

static DEFINE_MUTEX(gamespace_filter_lock);
static struct gamespace_touch_filter_state
	gamespace_filter_state[MAX_TOUCH_PANEL_COUNT];

static bool gamespace_is_landscape(u8 orientation)
{
	return orientation == 1 || orientation == 3;
}

static int gamespace_scaled_extent(int extent, u16 permille)
{
	int value;

	if (extent <= 0 || !permille)
		return 0;

	value = DIV_ROUND_CLOSEST(extent * permille, GAMESPACE_PERMILLE);
	return clamp_val(value, 1, extent / 2);
}

static enum gamespace_reject_reason
gamespace_reject_reason_for_point(const struct gamespace_touch_filter_state *state,
				  int x, int y, int max_x, int max_y)
{
	bool landscape = gamespace_is_landscape(state->orientation);
	int primary_extent = (landscape ? max_y : max_x) + 1;
	int secondary_extent = (landscape ? max_x : max_y) + 1;
	int primary = landscape ? y : x;
	int secondary = landscape ? x : y;
	int depth;
	int width;

	if (primary_extent <= 1 || secondary_extent <= 1)
		return GAMESPACE_REJECT_NONE;

	/* Reject the smaller corner grip zones before full edge strips. */
	if (state->grip_suppression) {
		depth = gamespace_scaled_extent(
			primary_extent,
			grip_depth_permille[state->grip_suppression]);
		width = gamespace_scaled_extent(
			secondary_extent,
			grip_width_permille[state->grip_suppression]);
		if ((primary < depth || primary >= primary_extent - depth) &&
		    (secondary < width || secondary >= secondary_extent - width))
			return GAMESPACE_REJECT_GRIP;
	}

	if (state->edge_filter) {
		width = gamespace_scaled_extent(
			primary_extent,
			edge_width_permille[state->edge_filter]);
		if (primary < width || primary >= primary_extent - width)
			return GAMESPACE_REJECT_EDGE;
	}

	return GAMESPACE_REJECT_NONE;
}

bool xiaomi_touch_filter_should_suppress(s8 touch_id, int slot, bool is_up,
					 int x, int y, int max_x, int max_y)
{
	struct gamespace_touch_filter_state *state;
	enum gamespace_reject_reason reason;
	unsigned long slot_mask;
	bool suppress = false;

	if (IS_TOUCH_ID_INVALID(touch_id) || slot < 0 || slot >= MAX_TOUCH_ID)
		return false;

	state = &gamespace_filter_state[touch_id];
	slot_mask = BIT(slot);

	mutex_lock(&gamespace_filter_lock);
	if (is_up) {
		suppress = !!(state->suppressed_slots & slot_mask);
		state->suppressed_slots &= ~slot_mask;
		state->reported_slots &= ~slot_mask;
		goto out;
	}

	/* Never cut off a contact that was already delivered to Android. */
	if (state->reported_slots & slot_mask)
		goto out;
	if (state->suppressed_slots & slot_mask) {
		suppress = true;
		goto out;
	}

	reason = gamespace_reject_reason_for_point(state, x, y, max_x, max_y);
	if (reason == GAMESPACE_REJECT_EDGE) {
		state->suppressed_slots |= slot_mask;
		state->edge_rejected++;
		suppress = true;
	} else if (reason == GAMESPACE_REJECT_GRIP) {
		state->suppressed_slots |= slot_mask;
		state->grip_rejected++;
		suppress = true;
	} else {
		state->reported_slots |= slot_mask;
	}

out:
	mutex_unlock(&gamespace_filter_lock);
	return suppress;
}

int xiaomi_touch_filter_set_profile(int edge_filter, int grip_suppression,
				    int orientation)
{
	struct gamespace_touch_filter_state *state =
		&gamespace_filter_state[0];

	if (edge_filter < 0 || edge_filter > GAMESPACE_FILTER_MAX_LEVEL ||
	    grip_suppression < 0 || grip_suppression > GAMESPACE_FILTER_MAX_LEVEL ||
	    orientation < 0 || orientation > 3)
		return -EINVAL;

	mutex_lock(&gamespace_filter_lock);
	state->edge_filter = edge_filter;
	state->grip_suppression = grip_suppression;
	state->orientation = orientation;
	mutex_unlock(&gamespace_filter_lock);

	LOG_INFO("GameSpace touch profile edge=%d grip=%d orientation=%d",
		 edge_filter, grip_suppression, orientation);
	return 0;
}

void xiaomi_touch_filter_get_profile(int *edge_filter, int *grip_suppression,
				     int *orientation)
{
	struct gamespace_touch_filter_state *state =
		&gamespace_filter_state[0];

	mutex_lock(&gamespace_filter_lock);
	if (edge_filter)
		*edge_filter = state->edge_filter;
	if (grip_suppression)
		*grip_suppression = state->grip_suppression;
	if (orientation)
		*orientation = state->orientation;
	mutex_unlock(&gamespace_filter_lock);
}

void xiaomi_touch_filter_get_stats(s8 touch_id, u64 *edge_rejected,
				   u64 *grip_rejected)
{
	struct gamespace_touch_filter_state *state;

	if (IS_TOUCH_ID_INVALID(touch_id))
		return;

	state = &gamespace_filter_state[touch_id];
	mutex_lock(&gamespace_filter_lock);
	if (edge_rejected)
		*edge_rejected = state->edge_rejected;
	if (grip_rejected)
		*grip_rejected = state->grip_rejected;
	mutex_unlock(&gamespace_filter_lock);
}
