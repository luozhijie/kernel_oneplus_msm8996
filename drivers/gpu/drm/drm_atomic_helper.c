/*
 * Copyright (C) 2014 Red Hat
 * Copyright (C) 2014 Intel Corp.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 * Daniel Vetter <daniel.vetter@ffwll.ch>
 */

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <linux/fence.h>

/**
 * DOC: overview
 *
 * This helper library provides implementations of check and commit functions on
 * top of the CRTC modeset helper callbacks and the plane helper callbacks. It
 * also provides convenience implementations for the atomic state handling
 * callbacks for drivers which don't need to subclass the drm core structures to
 * add their own additional internal state.
 *
 * This library also provides default implementations for the check callback in
 * drm_atomic_helper_check and for the commit callback with
 * drm_atomic_helper_commit. But the individual stages and callbacks are expose
 * to allow drivers to mix and match and e.g. use the plane helpers only
 * together with a driver private modeset implementation.
 *
 * This library also provides implementations for all the legacy driver
 * interfaces on top of the atomic interface. See drm_atomic_helper_set_config,
 * drm_atomic_helper_disable_plane, drm_atomic_helper_disable_plane and the
 * various functions to implement set_property callbacks. New drivers must not
 * implement these functions themselves but must use the provided helpers.
 */
static void
drm_atomic_helper_plane_changed(struct drm_atomic_state *state,
				struct drm_plane_state *plane_state,
				struct drm_plane *plane)
{
	struct drm_crtc_state *crtc_state;

	if (plane->state->crtc) {
		crtc_state = state->crtc_states[drm_crtc_index(plane->crtc)];

		if (WARN_ON(!crtc_state))
			return;

		crtc_state->planes_changed = true;
	}

	if (plane_state->crtc) {
		crtc_state =
			state->crtc_states[drm_crtc_index(plane_state->crtc)];

		if (WARN_ON(!crtc_state))
			return;

		crtc_state->planes_changed = true;
	}
}

static struct drm_crtc *
get_current_crtc_for_encoder(struct drm_device *dev,
			     struct drm_encoder *encoder)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_connector *connector;

	WARN_ON(!drm_modeset_is_locked(&config->connection_mutex));

	list_for_each_entry(connector, &config->connector_list, head) {
		if (connector->state->best_encoder != encoder)
			continue;

		return connector->state->crtc;
	}

	return NULL;
}

static int
steal_encoder(struct drm_atomic_state *state,
	      struct drm_encoder *encoder,
	      struct drm_crtc *encoder_crtc)
{
	struct drm_mode_config *config = &state->dev->mode_config;
	struct drm_crtc_state *crtc_state;
	struct drm_connector *connector;
	struct drm_connector_state *connector_state;
	int ret;

	/*
	 * We can only steal an encoder coming from a connector, which means we
	 * must already hold the connection_mutex.
	 */
	WARN_ON(!drm_modeset_is_locked(&config->connection_mutex));

	DRM_DEBUG_KMS("[ENCODER:%d:%s] in use on [CRTC:%d], stealing it\n",
		      encoder->base.id, encoder->name,
		      encoder_crtc->base.id);

	crtc_state = drm_atomic_get_crtc_state(state, encoder_crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);

	crtc_state->mode_changed = true;

	list_for_each_entry(connector, &config->connector_list, head) {
		if (connector->state->best_encoder != encoder)
			continue;

		DRM_DEBUG_KMS("Stealing encoder from [CONNECTOR:%d:%s]\n",
			      connector->base.id,
			      connector->name);

		connector_state = drm_atomic_get_connector_state(state,
								 connector);
		if (IS_ERR(connector_state))
			return PTR_ERR(connector_state);

		ret = drm_atomic_set_crtc_for_connector(connector_state, NULL);
		if (ret)
			return ret;
		connector_state->best_encoder = NULL;
	}

	return 0;
}

static int
update_connector_routing(struct drm_atomic_state *state, int conn_idx)
{
	struct drm_connector_helper_funcs *funcs;
	struct drm_encoder *new_encoder;
	struct drm_crtc *encoder_crtc;
	struct drm_connector *connector;
	struct drm_connector_state *connector_state;
	struct drm_crtc_state *crtc_state;
	int idx, ret;

	connector = state->connectors[conn_idx];
	connector_state = state->connector_states[conn_idx];

	if (!connector)
		return 0;

	DRM_DEBUG_KMS("Updating routing for [CONNECTOR:%d:%s]\n",
			connector->base.id,
			connector->name);

	if (connector->state->crtc != connector_state->crtc) {
		if (connector->state->crtc) {
			idx = drm_crtc_index(connector->state->crtc);

			crtc_state = state->crtc_states[idx];
			crtc_state->mode_changed = true;
		}

		if (connector_state->crtc) {
			idx = drm_crtc_index(connector_state->crtc);

			crtc_state = state->crtc_states[idx];
			crtc_state->mode_changed = true;
		}
	}

	if (!connector_state->crtc) {
		DRM_DEBUG_KMS("Disabling [CONNECTOR:%d:%s]\n",
				connector->base.id,
				connector->name);

		connector_state->best_encoder = NULL;

		return 0;
	}

	funcs = connector->helper_private;
	new_encoder = funcs->best_encoder(connector);

	if (!new_encoder) {
		DRM_DEBUG_KMS("No suitable encoder found for [CONNECTOR:%d:%s]\n",
			      connector->base.id,
			      connector->name);
		return -EINVAL;
	}

	if (new_encoder == connector_state->best_encoder) {
		DRM_DEBUG_KMS("[CONNECTOR:%d:%s] keeps [ENCODER:%d:%s], now on [CRTC:%d]\n",
			      connector->base.id,
			      connector->name,
			      new_encoder->base.id,
			      new_encoder->name,
			      connector_state->crtc->base.id);

		return 0;
	}

	encoder_crtc = get_current_crtc_for_encoder(state->dev,
						    new_encoder);

	if (encoder_crtc) {
		ret = steal_encoder(state, new_encoder, encoder_crtc);
		if (ret) {
			DRM_DEBUG_KMS("Encoder stealing failed for [CONNECTOR:%d:%s]\n",
				      connector->base.id,
				      connector->name);
			return ret;
		}
	}

	connector_state->best_encoder = new_encoder;
	idx = drm_crtc_index(connector_state->crtc);

	crtc_state = state->crtc_states[idx];
	crtc_state->mode_changed = true;

	DRM_DEBUG_KMS("[CONNECTOR:%d:%s] using [ENCODER:%d:%s] on [CRTC:%d]\n",
		      connector->base.id,
		      connector->name,
		      new_encoder->base.id,
		      new_encoder->name,
		      connector_state->crtc->base.id);

	return 0;
}

static int
mode_fixup(struct drm_atomic_state *state)
{
	int ncrtcs = state->dev->mode_config.num_crtc;
	int nconnectors = state->dev->mode_config.num_connector;
	struct drm_crtc_state *crtc_state;
	struct drm_connector_state *conn_state;
	int i;
	bool ret;

	for (i = 0; i < ncrtcs; i++) {
		crtc_state = state->crtc_states[i];

		if (!crtc_state || !crtc_state->mode_changed)
			continue;

		drm_mode_copy(&crtc_state->adjusted_mode, &crtc_state->mode);
	}

	for (i = 0; i < nconnectors; i++) {
		struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;

		conn_state = state->connector_states[i];

		if (!conn_state)
			continue;

		WARN_ON(!!conn_state->best_encoder != !!conn_state->crtc);

		if (!conn_state->crtc || !conn_state->best_encoder)
			continue;

		crtc_state =
			state->crtc_states[drm_crtc_index(conn_state->crtc)];

		/*
		 * Each encoder has at most one connector (since we always steal
		 * it away), so we won't call ->mode_fixup twice.
		 */
		encoder = conn_state->best_encoder;
		funcs = encoder->helper_private;

		if (encoder->bridge && encoder->bridge->funcs->mode_fixup) {
			ret = encoder->bridge->funcs->mode_fixup(
					encoder->bridge, &crtc_state->mode,
					&crtc_state->adjusted_mode);
			if (!ret) {
				DRM_DEBUG_KMS("Bridge fixup failed\n");
				return -EINVAL;
			}
		}


		ret = funcs->mode_fixup(encoder, &crtc_state->mode,
					&crtc_state->adjusted_mode);
		if (!ret) {
			DRM_DEBUG_KMS("[ENCODER:%d:%s] fixup failed\n",
				      encoder->base.id, encoder->name);
			return -EINVAL;
		}
	}

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc_helper_funcs *funcs;
		struct drm_crtc *crtc;

		crtc_state = state->crtc_states[i];
		crtc = state->crtcs[i];

		if (!crtc_state || !crtc_state->mode_changed)
			continue;

		funcs = crtc->helper_private;
		ret = funcs->mode_fixup(crtc, &crtc_state->mode,
					&crtc_state->adjusted_mode);
		if (!ret) {
			DRM_DEBUG_KMS("[CRTC:%d] fixup failed\n",
				      crtc->base.id);
			return -EINVAL;
		}
	}

	return 0;
}

static int
drm_atomic_helper_check_prepare(struct drm_device *dev,
				struct drm_atomic_state *state)
{
	int ncrtcs = dev->mode_config.num_crtc;
	int nconnectors = dev->mode_config.num_connector;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	int i, ret;

	for (i = 0; i < ncrtcs; i++) {
		crtc = state->crtcs[i];
		crtc_state = state->crtc_states[i];

		if (!crtc)
			continue;

		if (!drm_mode_equal(&crtc->state->mode, &crtc_state->mode)) {
			DRM_DEBUG_KMS("[CRTC:%d] mode changed\n",
				      crtc->base.id);
			crtc_state->mode_changed = true;
		}

		if (crtc->state->enable != crtc_state->enable) {
			DRM_DEBUG_KMS("[CRTC:%d] enable changed\n",
				      crtc->base.id);
			crtc_state->mode_changed = true;
		}
	}

	for (i = 0; i < nconnectors; i++) {
		/*
		 * This only sets crtc->mode_changed for routing changes,
		 * drivers must set crtc->mode_changed themselves when connector
		 * properties need to be updated.
		 */
		ret = update_connector_routing(state, i);
		if (ret)
			return ret;
	}

	/*
	 * After all the routing has been prepared we need to add in any
	 * connector which is itself unchanged, but who's crtc changes it's
	 * configuration. This must be done before calling mode_fixup in case a
	 * crtc only changed its mode but has the same set of connectors.
	 */
	for (i = 0; i < ncrtcs; i++) {
		int num_connectors;

		crtc = state->crtcs[i];
		crtc_state = state->crtc_states[i];

		if (!crtc || !crtc_state->mode_changed)
			continue;

		DRM_DEBUG_KMS("[CRTC:%d] needs full modeset, enable: %c\n",
			      crtc->base.id,
			      crtc_state->enable ? 'y' : 'n');

		ret = drm_atomic_add_affected_connectors(state, crtc);
		if (ret != 0)
			return ret;

		num_connectors = drm_atomic_connectors_for_crtc(state,
								crtc);

		if (crtc_state->enable != !!num_connectors) {
			DRM_DEBUG_KMS("[CRTC:%d] enabled/connectors mismatch\n",
				      crtc->base.id);

			return -EINVAL;
		}
	}

	return mode_fixup(state);
}

/**
 * drm_atomic_helper_check - validate state object
 * @dev: DRM device
 * @state: the driver state object
 *
 * Check the state object to see if the requested state is physically possible.
 * Only crtcs and planes have check callbacks, so for any additional (global)
 * checking that a driver needs it can simply wrap that around this function.
 * Drivers without such needs can directly use this as their ->atomic_check()
 * callback.
 *
 * RETURNS
 * Zero for success or -errno
 */
int drm_atomic_helper_check(struct drm_device *dev,
			    struct drm_atomic_state *state)
{
	int nplanes = dev->mode_config.num_total_plane;
	int ncrtcs = dev->mode_config.num_crtc;
	int i, ret = 0;

	ret = drm_atomic_helper_check_prepare(dev, state);
	if (ret)
		return ret;

	for (i = 0; i < nplanes; i++) {
		struct drm_plane_helper_funcs *funcs;
		struct drm_plane *plane = state->planes[i];
		struct drm_plane_state *plane_state = state->plane_states[i];

		if (!plane)
			continue;

		funcs = plane->helper_private;

		drm_atomic_helper_plane_changed(state, plane_state, plane);

		if (!funcs || !funcs->atomic_check)
			continue;

		ret = funcs->atomic_check(plane, plane_state);
		if (ret) {
			DRM_DEBUG_KMS("[PLANE:%d] atomic check failed\n",
				      plane->base.id);
			return ret;
		}
	}

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc_helper_funcs *funcs;
		struct drm_crtc *crtc = state->crtcs[i];

		if (!crtc)
			continue;

		funcs = crtc->helper_private;

		if (!funcs || !funcs->atomic_check)
			continue;

		ret = funcs->atomic_check(crtc, state->crtc_states[i]);
		if (ret) {
			DRM_DEBUG_KMS("[CRTC:%d] atomic check failed\n",
				      crtc->base.id);
			return ret;
		}
	}

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_check);

static void
disable_outputs(struct drm_device *dev, struct drm_atomic_state *old_state)
{
	int ncrtcs = old_state->dev->mode_config.num_crtc;
	int nconnectors = old_state->dev->mode_config.num_connector;
	int i;

	for (i = 0; i < nconnectors; i++) {
		struct drm_connector_state *old_conn_state;
		struct drm_connector *connector;
		struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;

		old_conn_state = old_state->connector_states[i];
		connector = old_state->connectors[i];

		/* Shut down everything that's in the changeset and currently
		 * still on. So need to check the old, saved state. */
		if (!old_conn_state || !old_conn_state->crtc)
			continue;

		encoder = connector->state->best_encoder;

		if (!encoder)
			continue;

		funcs = encoder->helper_private;

		/*
		 * Each encoder has at most one connector (since we always steal
		 * it away), so we won't call call disable hooks twice.
		 */
		if (encoder->bridge)
			encoder->bridge->funcs->disable(encoder->bridge);

		/* Right function depends upon target state. */
		if (connector->state->crtc)
			funcs->prepare(encoder);
		else if (funcs->disable)
			funcs->disable(encoder);
		else
			funcs->dpms(encoder, DRM_MODE_DPMS_OFF);

		if (encoder->bridge)
			encoder->bridge->funcs->post_disable(encoder->bridge);
	}

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc_helper_funcs *funcs;
		struct drm_crtc *crtc;

		crtc = old_state->crtcs[i];

		/* Shut down everything that needs a full modeset. */
		if (!crtc || !crtc->state->mode_changed)
			continue;

		funcs = crtc->helper_private;

		/* Right function depends upon target state. */
		if (crtc->state->enable)
			funcs->prepare(crtc);
		else if (funcs->disable)
			funcs->disable(crtc);
		else
			funcs->dpms(crtc, DRM_MODE_DPMS_OFF);
	}
}

static void
set_routing_links(struct drm_device *dev, struct drm_atomic_state *old_state)
{
	int nconnectors = dev->mode_config.num_connector;
	int ncrtcs = old_state->dev->mode_config.num_crtc;
	int i;

	/* clear out existing links */
	for (i = 0; i < nconnectors; i++) {
		struct drm_connector *connector;

		connector = old_state->connectors[i];

		if (!connector || !connector->encoder)
			continue;

		WARN_ON(!connector->encoder->crtc);

		connector->encoder->crtc = NULL;
		connector->encoder = NULL;
	}

	/* set new links */
	for (i = 0; i < nconnectors; i++) {
		struct drm_connector *connector;

		connector = old_state->connectors[i];

		if (!connector || !connector->state->crtc)
			continue;

		if (WARN_ON(!connector->state->best_encoder))
			continue;

		connector->encoder = connector->state->best_encoder;
		connector->encoder->crtc = connector->state->crtc;
	}

	/* set legacy state in the crtc structure */
	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc *crtc;

		crtc = old_state->crtcs[i];

		if (!crtc)
			continue;

		crtc->mode = crtc->state->mode;
		crtc->enabled = crtc->state->enable;
		crtc->x = crtc->primary->state->src_x >> 16;
		crtc->y = crtc->primary->state->src_y >> 16;
	}
}

static void
crtc_set_mode(struct drm_device *dev, struct drm_atomic_state *old_state)
{
	int ncrtcs = old_state->dev->mode_config.num_crtc;
	int nconnectors = old_state->dev->mode_config.num_connector;
	int i;

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc_helper_funcs *funcs;
		struct drm_crtc *crtc;

		crtc = old_state->crtcs[i];

		if (!crtc || !crtc->state->mode_changed)
			continue;

		funcs = crtc->helper_private;

		if (crtc->state->enable)
			funcs->mode_set_nofb(crtc);
	}

	for (i = 0; i < nconnectors; i++) {
		struct drm_connector *connector;
		struct drm_crtc_state *new_crtc_state;
		struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;
		struct drm_display_mode *mode, *adjusted_mode;

		connector = old_state->connectors[i];

		if (!connector || !connector->state->best_encoder)
			continue;

		encoder = connector->state->best_encoder;
		funcs = encoder->helper_private;
		new_crtc_state = connector->state->crtc->state;
		mode = &new_crtc_state->mode;
		adjusted_mode = &new_crtc_state->adjusted_mode;

		/*
		 * Each encoder has at most one connector (since we always steal
		 * it away), so we won't call call mode_set hooks twice.
		 */
		funcs->mode_set(encoder, mode, adjusted_mode);

		if (encoder->bridge && encoder->bridge->funcs->mode_set)
			encoder->bridge->funcs->mode_set(encoder->bridge,
							 mode, adjusted_mode);
	}
}

/**
 * drm_atomic_helper_commit_pre_planes - modeset commit before plane updates
 * @dev: DRM device
 * @state: atomic state
 *
 * This function commits the modeset changes that need to be committed before
 * updating planes. It shuts down all the outputs that need to be shut down and
 * prepares them (if required) with the new mode.
 */
void drm_atomic_helper_commit_pre_planes(struct drm_device *dev,
					 struct drm_atomic_state *state)
{
	disable_outputs(dev, state);
	set_routing_links(dev, state);
	crtc_set_mode(dev, state);
}
EXPORT_SYMBOL(drm_atomic_helper_commit_pre_planes);

/**
 * drm_atomic_helper_commit_post_planes - modeset commit after plane updates
 * @dev: DRM device
 * @old_state: atomic state object with old state structures
 *
 * This function commits the modeset changes that need to be committed after
 * updating planes: It enables all the outputs with the new configuration which
 * had to be turned off for the update.
 */
void drm_atomic_helper_commit_post_planes(struct drm_device *dev,
					  struct drm_atomic_state *old_state)
{
	int ncrtcs = old_state->dev->mode_config.num_crtc;
	int nconnectors = old_state->dev->mode_config.num_connector;
	int i;

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc_helper_funcs *funcs;
		struct drm_crtc *crtc;

		crtc = old_state->crtcs[i];

		/* Need to filter out CRTCs where only planes change. */
		if (!crtc || !crtc->state->mode_changed)
			continue;

		funcs = crtc->helper_private;

		if (crtc->state->enable)
			funcs->commit(crtc);
	}

	for (i = 0; i < nconnectors; i++) {
		struct drm_connector *connector;
		struct drm_encoder_helper_funcs *funcs;
		struct drm_encoder *encoder;

		connector = old_state->connectors[i];

		if (!connector || !connector->state->best_encoder)
			continue;

		encoder = connector->state->best_encoder;
		funcs = encoder->helper_private;

		/*
		 * Each encoder has at most one connector (since we always steal
		 * it away), so we won't call call enable hooks twice.
		 */
		if (encoder->bridge)
			encoder->bridge->funcs->pre_enable(encoder->bridge);

		funcs->commit(encoder);

		if (encoder->bridge)
			encoder->bridge->funcs->enable(encoder->bridge);
	}
}
EXPORT_SYMBOL(drm_atomic_helper_commit_post_planes);

static void wait_for_fences(struct drm_device *dev,
			    struct drm_atomic_state *state)
{
	int nplanes = dev->mode_config.num_total_plane;
	int i;

	for (i = 0; i < nplanes; i++) {
		struct drm_plane *plane = state->planes[i];

		if (!plane || !plane->state->fence)
			continue;

		WARN_ON(!plane->state->fb);

		fence_wait(plane->state->fence, false);
		fence_put(plane->state->fence);
		plane->state->fence = NULL;
	}
}

/**
 * drm_atomic_helper_wait_for_vblanks - wait for vblank on crtcs
 * @dev: DRM device
 * @old_state: atomic state object with old state structures
 *
 * Helper to, after atomic commit, wait for vblanks on all effected
 * crtcs (ie. before cleaning up old framebuffers using
 * drm_atomic_helper_cleanup_planes())
 */
void
drm_atomic_helper_wait_for_vblanks(struct drm_device *dev,
		struct drm_atomic_state *old_state)
{
	struct drm_crtc *crtc;
	struct drm_crtc_state *old_crtc_state;
	int ncrtcs = old_state->dev->mode_config.num_crtc;
	int i, ret;

	for (i = 0; i < ncrtcs; i++) {
		crtc = old_state->crtcs[i];
		old_crtc_state = old_state->crtc_states[i];

		if (!crtc)
			continue;

		/* No one cares about the old state, so abuse it for tracking
		 * and store whether we hold a vblank reference (and should do a
		 * vblank wait) in the ->enable boolean. */
		old_crtc_state->enable = false;

		if (!crtc->state->enable)
			continue;

		ret = drm_crtc_vblank_get(crtc);
		if (ret != 0)
			continue;

		old_crtc_state->enable = true;
		old_crtc_state->last_vblank_count = drm_vblank_count(dev, i);
	}

	for (i = 0; i < ncrtcs; i++) {
		crtc = old_state->crtcs[i];
		old_crtc_state = old_state->crtc_states[i];

		if (!crtc || !old_crtc_state->enable)
			continue;

		ret = wait_event_timeout(dev->vblank[i].queue,
				old_crtc_state->last_vblank_count !=
					drm_vblank_count(dev, i),
				msecs_to_jiffies(50));

		drm_crtc_vblank_put(crtc);
	}
}
EXPORT_SYMBOL(drm_atomic_helper_wait_for_vblanks);

/**
 * drm_atomic_helper_commit - commit validated state object
 * @dev: DRM device
 * @state: the driver state object
 * @async: asynchronous commit
 *
 * This function commits a with drm_atomic_helper_check() pre-validated state
 * object. This can still fail when e.g. the framebuffer reservation fails. For
 * now this doesn't implement asynchronous commits.
 *
 * RETURNS
 * Zero for success or -errno.
 */
int drm_atomic_helper_commit(struct drm_device *dev,
			     struct drm_atomic_state *state,
			     bool async)
{
	int ret;

	if (async)
		return -EBUSY;

	ret = drm_atomic_helper_prepare_planes(dev, state);
	if (ret)
		return ret;

	/*
	 * This is the point of no return - everything below never fails except
	 * when the hw goes bonghits. Which means we can commit the new state on
	 * the software side now.
	 */

	drm_atomic_helper_swap_state(dev, state);

	/*
	 * Everything below can be run asynchronously without the need to grab
	 * any modeset locks at all under one conditions: It must be guaranteed
	 * that the asynchronous work has either been cancelled (if the driver
	 * supports it, which at least requires that the framebuffers get
	 * cleaned up with drm_atomic_helper_cleanup_planes()) or completed
	 * before the new state gets committed on the software side with
	 * drm_atomic_helper_swap_state().
	 *
	 * This scheme allows new atomic state updates to be prepared and
	 * checked in parallel to the asynchronous completion of the previous
	 * update. Which is important since compositors need to figure out the
	 * composition of the next frame right after having submitted the
	 * current layout.
	 */

	wait_for_fences(dev, state);

	drm_atomic_helper_commit_pre_planes(dev, state);

	drm_atomic_helper_commit_planes(dev, state);

	drm_atomic_helper_commit_post_planes(dev, state);

	drm_atomic_helper_wait_for_vblanks(dev, state);

	drm_atomic_helper_cleanup_planes(dev, state);

	drm_atomic_state_free(state);

	return 0;
}
EXPORT_SYMBOL(drm_atomic_helper_commit);

/**
 * DOC: implementing async commit
 *
 * For now the atomic helpers don't support async commit directly. If there is
 * real need it could be added though, using the dma-buf fence infrastructure
 * for generic synchronization with outstanding rendering.
 *
 * For now drivers have to implement async commit themselves, with the following
 * sequence being the recommended one:
 *
 * 1. Run drm_atomic_helper_prepare_planes() first. This is the only function
 * which commit needs to call which can fail, so we want to run it first and
 * synchronously.
 *
 * 2. Synchronize with any outstanding asynchronous commit worker threads which
 * might be affected the new state update. This can be done by either cancelling
 * or flushing the work items, depending upon whether the driver can deal with
 * cancelled updates. Note that it is important to ensure that the framebuffer
 * cleanup is still done when cancelling.
 *
 * For sufficient parallelism it is recommended to have a work item per crtc
 * (for updates which don't touch global state) and a global one. Then we only
 * need to synchronize with the crtc work items for changed crtcs and the global
 * work item, which allows nice concurrent updates on disjoint sets of crtcs.
 *
 * 3. The software state is updated synchronously with
 * drm_atomic_helper_swap_state. Doing this under the protection of all modeset
 * locks means concurrent callers never see inconsistent state. And doing this
 * while it's guaranteed that no relevant async worker runs means that async
 * workers do not need grab any locks. Actually they must not grab locks, for
 * otherwise the work flushing will deadlock.
 *
 * 4. Schedule a work item to do all subsequent steps, using the split-out
 * commit helpers: a) pre-plane commit b) plane commit c) post-plane commit and
 * then cleaning up the framebuffers after the old framebuffer is no longer
 * being displayed.
 */

/**
 * drm_atomic_helper_prepare_planes - prepare plane resources after commit
 * @dev: DRM device
 * @state: atomic state object with old state structures
 *
 * This function prepares plane state, specifically framebuffers, for the new
 * configuration. If any failure is encountered this function will call
 * ->cleanup_fb on any already successfully prepared framebuffer.
 *
 * Returns:
 * 0 on success, negative error code on failure.
 */
int drm_atomic_helper_prepare_planes(struct drm_device *dev,
				     struct drm_atomic_state *state)
{
	int nplanes = dev->mode_config.num_total_plane;
	int ret, i;

	for (i = 0; i < nplanes; i++) {
		struct drm_plane_helper_funcs *funcs;
		struct drm_plane *plane = state->planes[i];
		struct drm_framebuffer *fb;

		if (!plane)
			continue;

		funcs = plane->helper_private;

		fb = state->plane_states[i]->fb;

		if (fb && funcs->prepare_fb) {
			ret = funcs->prepare_fb(plane, fb);
			if (ret)
				goto fail;
		}
	}

	return 0;

fail:
	for (i--; i >= 0; i--) {
		struct drm_plane_helper_funcs *funcs;
		struct drm_plane *plane = state->planes[i];
		struct drm_framebuffer *fb;

		if (!plane)
			continue;

		funcs = plane->helper_private;

		fb = state->plane_states[i]->fb;

		if (fb && funcs->cleanup_fb)
			funcs->cleanup_fb(plane, fb);

	}

	return ret;
}
EXPORT_SYMBOL(drm_atomic_helper_prepare_planes);

/**
 * drm_atomic_helper_commit_planes - commit plane state
 * @dev: DRM device
 * @state: atomic state
 *
 * This function commits the new plane state using the plane and atomic helper
 * functions for planes and crtcs. It assumes that the atomic state has already
 * been pushed into the relevant object state pointers, since this step can no
 * longer fail.
 *
 * It still requires the global state object @state to know which planes and
 * crtcs need to be updated though.
 */
void drm_atomic_helper_commit_planes(struct drm_device *dev,
				     struct drm_atomic_state *state)
{
	int nplanes = dev->mode_config.num_total_plane;
	int ncrtcs = dev->mode_config.num_crtc;
	int i;

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc_helper_funcs *funcs;
		struct drm_crtc *crtc = state->crtcs[i];

		if (!crtc)
			continue;

		WARN_ON(!drm_modeset_is_locked(&crtc->mutex));

		funcs = crtc->helper_private;

		if (!funcs || !funcs->atomic_begin)
			continue;

		funcs->atomic_begin(crtc);
	}

	for (i = 0; i < nplanes; i++) {
		struct drm_plane_helper_funcs *funcs;
		struct drm_plane *plane = state->planes[i];

		if (!plane)
			continue;

		WARN_ON(!drm_modeset_is_locked(&plane->mutex));

		funcs = plane->helper_private;

		if (!funcs || !funcs->atomic_update)
			continue;

		funcs->atomic_update(plane);
	}

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc_helper_funcs *funcs;
		struct drm_crtc *crtc = state->crtcs[i];

		if (!crtc)
			continue;

		funcs = crtc->helper_private;

		if (!funcs || !funcs->atomic_flush)
			continue;

		funcs->atomic_flush(crtc);
	}
}
EXPORT_SYMBOL(drm_atomic_helper_commit_planes);

/**
 * drm_atomic_helper_cleanup_planes - cleanup plane resources after commit
 * @dev: DRM device
 * @old_state: atomic state object with old state structures
 *
 * This function cleans up plane state, specifically framebuffers, from the old
 * configuration. Hence the old configuration must be perserved in @old_state to
 * be able to call this function.
 *
 * This function must also be called on the new state when the atomic update
 * fails at any point after calling drm_atomic_helper_prepare_planes().
 */
void drm_atomic_helper_cleanup_planes(struct drm_device *dev,
				      struct drm_atomic_state *old_state)
{
	int nplanes = dev->mode_config.num_total_plane;
	int i;

	for (i = 0; i < nplanes; i++) {
		struct drm_plane_helper_funcs *funcs;
		struct drm_plane *plane = old_state->planes[i];
		struct drm_framebuffer *old_fb;

		if (!plane)
			continue;

		funcs = plane->helper_private;

		old_fb = old_state->plane_states[i]->fb;

		if (old_fb && funcs->cleanup_fb)
			funcs->cleanup_fb(plane, old_fb);
	}
}
EXPORT_SYMBOL(drm_atomic_helper_cleanup_planes);

/**
 * drm_atomic_helper_swap_state - store atomic state into current sw state
 * @dev: DRM device
 * @state: atomic state
 *
 * This function stores the atomic state into the current state pointers in all
 * driver objects. It should be called after all failing steps have been done
 * and succeeded, but before the actual hardware state is committed.
 *
 * For cleanup and error recovery the current state for all changed objects will
 * be swaped into @state.
 *
 * With that sequence it fits perfectly into the plane prepare/cleanup sequence:
 *
 * 1. Call drm_atomic_helper_prepare_planes() with the staged atomic state.
 *
 * 2. Do any other steps that might fail.
 *
 * 3. Put the staged state into the current state pointers with this function.
 *
 * 4. Actually commit the hardware state.
 *
 * 5. Call drm_atomic_helper_cleanup_planes with @state, which since step 3
 * contains the old state. Also do any other cleanup required with that state.
 */
void drm_atomic_helper_swap_state(struct drm_device *dev,
				  struct drm_atomic_state *state)
{
	int i;

	for (i = 0; i < dev->mode_config.num_connector; i++) {
		struct drm_connector *connector = state->connectors[i];

		if (!connector)
			continue;

		connector->state->state = state;
		swap(state->connector_states[i], connector->state);
		connector->state->state = NULL;
	}

	for (i = 0; i < dev->mode_config.num_crtc; i++) {
		struct drm_crtc *crtc = state->crtcs[i];

		if (!crtc)
			continue;

		crtc->state->state = state;
		swap(state->crtc_states[i], crtc->state);
		crtc->state->state = NULL;
	}

	for (i = 0; i < dev->mode_config.num_total_plane; i++) {
		struct drm_plane *plane = state->planes[i];

		if (!plane)
			continue;

		plane->state->state = state;
		swap(state->plane_states[i], plane->state);
		plane->state->state = NULL;
	}
}
EXPORT_SYMBOL(drm_atomic_helper_swap_state);

/**
 * drm_atomic_helper_update_plane - Helper for primary plane update using atomic
 * @plane: plane object to update
 * @crtc: owning CRTC of owning plane
 * @fb: framebuffer to flip onto plane
 * @crtc_x: x offset of primary plane on crtc
 * @crtc_y: y offset of primary plane on crtc
 * @crtc_w: width of primary plane rectangle on crtc
 * @crtc_h: height of primary plane rectangle on crtc
 * @src_x: x offset of @fb for panning
 * @src_y: y offset of @fb for panning
 * @src_w: width of source rectangle in @fb
 * @src_h: height of source rectangle in @fb
 *
 * Provides a default plane update handler using the atomic driver interface.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int drm_atomic_helper_update_plane(struct drm_plane *plane,
				   struct drm_crtc *crtc,
				   struct drm_framebuffer *fb,
				   int crtc_x, int crtc_y,
				   unsigned int crtc_w, unsigned int crtc_h,
				   uint32_t src_x, uint32_t src_y,
				   uint32_t src_w, uint32_t src_h)
{
	struct drm_atomic_state *state;
	struct drm_plane_state *plane_state;
	int ret = 0;

	state = drm_atomic_state_alloc(plane->dev);
	if (!state)
		return -ENOMEM;

	state->acquire_ctx = drm_modeset_legacy_acquire_ctx(crtc);
retry:
	plane_state = drm_atomic_get_plane_state(state, plane);
	if (IS_ERR(plane_state)) {
		ret = PTR_ERR(plane_state);
		goto fail;
	}

	ret = drm_atomic_set_crtc_for_plane(plane_state, crtc);
	if (ret != 0)
		goto fail;
	drm_atomic_set_fb_for_plane(plane_state, fb);
	plane_state->crtc_x = crtc_x;
	plane_state->crtc_y = crtc_y;
	plane_state->crtc_h = crtc_h;
	plane_state->crtc_w = crtc_w;
	plane_state->src_x = src_x;
	plane_state->src_y = src_y;
	plane_state->src_h = src_h;
	plane_state->src_w = src_w;

	ret = drm_atomic_commit(state);
	if (ret != 0)
		goto fail;

	/* Driver takes ownership of state on successful commit. */
	return 0;
fail:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_free(state);

	return ret;
backoff:
	drm_atomic_legacy_backoff(state);
	drm_atomic_state_clear(state);

	/*
	 * Someone might have exchanged the framebuffer while we dropped locks
	 * in the backoff code. We need to fix up the fb refcount tracking the
	 * core does for us.
	 */
	plane->old_fb = plane->fb;

	goto retry;
}
EXPORT_SYMBOL(drm_atomic_helper_update_plane);

/**
 * drm_atomic_helper_disable_plane - Helper for primary plane disable using * atomic
 * @plane: plane to disable
 *
 * Provides a default plane disable handler using the atomic driver interface.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int drm_atomic_helper_disable_plane(struct drm_plane *plane)
{
	struct drm_atomic_state *state;
	struct drm_plane_state *plane_state;
	int ret = 0;

	state = drm_atomic_state_alloc(plane->dev);
	if (!state)
		return -ENOMEM;

	state->acquire_ctx = drm_modeset_legacy_acquire_ctx(plane->crtc);
retry:
	plane_state = drm_atomic_get_plane_state(state, plane);
	if (IS_ERR(plane_state)) {
		ret = PTR_ERR(plane_state);
		goto fail;
	}

	ret = drm_atomic_set_crtc_for_plane(plane_state, NULL);
	if (ret != 0)
		goto fail;
	drm_atomic_set_fb_for_plane(plane_state, NULL);
	plane_state->crtc_x = 0;
	plane_state->crtc_y = 0;
	plane_state->crtc_h = 0;
	plane_state->crtc_w = 0;
	plane_state->src_x = 0;
	plane_state->src_y = 0;
	plane_state->src_h = 0;
	plane_state->src_w = 0;

	ret = drm_atomic_commit(state);
	if (ret != 0)
		goto fail;

	/* Driver takes ownership of state on successful commit. */
	return 0;
fail:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_free(state);

	return ret;
backoff:
	drm_atomic_legacy_backoff(state);
	drm_atomic_state_clear(state);

	/*
	 * Someone might have exchanged the framebuffer while we dropped locks
	 * in the backoff code. We need to fix up the fb refcount tracking the
	 * core does for us.
	 */
	plane->old_fb = plane->fb;

	goto retry;
}
EXPORT_SYMBOL(drm_atomic_helper_disable_plane);

static int update_output_state(struct drm_atomic_state *state,
			       struct drm_mode_set *set)
{
	struct drm_device *dev = set->crtc->dev;
	struct drm_connector_state *conn_state;
	int nconnectors = state->dev->mode_config.num_connector;
	int ncrtcs = state->dev->mode_config.num_crtc;
	int ret, i, j;

	ret = drm_modeset_lock(&dev->mode_config.connection_mutex,
			       state->acquire_ctx);
	if (ret)
		return ret;

	/* First grab all affected connector/crtc states. */
	for (i = 0; i < set->num_connectors; i++) {
		conn_state = drm_atomic_get_connector_state(state,
							    set->connectors[i]);
		if (IS_ERR(conn_state))
			return PTR_ERR(conn_state);
	}

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc *crtc = state->crtcs[i];

		if (!crtc)
			continue;

		ret = drm_atomic_add_affected_connectors(state, crtc);
		if (ret)
			return ret;
	}

	/* Then recompute connector->crtc links and crtc enabling state. */
	for (i = 0; i < nconnectors; i++) {
		struct drm_connector *connector;

		connector = state->connectors[i];
		conn_state = state->connector_states[i];

		if (!connector)
			continue;

		if (conn_state->crtc == set->crtc) {
			ret = drm_atomic_set_crtc_for_connector(conn_state,
								NULL);
			if (ret)
				return ret;
		}

		for (j = 0; j < set->num_connectors; j++) {
			if (set->connectors[j] == connector) {
				ret = drm_atomic_set_crtc_for_connector(conn_state,
									set->crtc);
				if (ret)
					return ret;
				break;
			}
		}
	}

	for (i = 0; i < ncrtcs; i++) {
		struct drm_crtc *crtc = state->crtcs[i];
		struct drm_crtc_state *crtc_state = state->crtc_states[i];

		if (!crtc)
			continue;

		/* Don't update ->enable for the CRTC in the set_config request,
		 * since a mismatch would indicate a bug in the upper layers.
		 * The actual modeset code later on will catch any
		 * inconsistencies here. */
		if (crtc == set->crtc)
			continue;

		crtc_state->enable =
			drm_atomic_connectors_for_crtc(state, crtc);
	}

	return 0;
}

/**
 * drm_atomic_helper_set_config - set a new config from userspace
 * @set: mode set configuration
 *
 * Provides a default crtc set_config handler using the atomic driver interface.
 *
 * Returns:
 * Returns 0 on success, negative errno numbers on failure.
 */
int drm_atomic_helper_set_config(struct drm_mode_set *set)
{
	struct drm_atomic_state *state;
	struct drm_crtc *crtc = set->crtc;
	struct drm_crtc_state *crtc_state;
	struct drm_plane_state *primary_state;
	int ret = 0;

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state)
		return -ENOMEM;

	state->acquire_ctx = drm_modeset_legacy_acquire_ctx(crtc);
retry:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto fail;
	}

	if (!set->mode) {
		WARN_ON(set->fb);
		WARN_ON(set->num_connectors);

		crtc_state->enable = false;
		goto commit;
	}

	WARN_ON(!set->fb);
	WARN_ON(!set->num_connectors);

	crtc_state->enable = true;
	drm_mode_copy(&crtc_state->mode, set->mode);

	primary_state = drm_atomic_get_plane_state(state, crtc->primary);
	if (IS_ERR(primary_state)) {
		ret = PTR_ERR(primary_state);
		goto fail;
	}

	ret = drm_atomic_set_crtc_for_plane(primary_state, crtc);
	if (ret != 0)
		goto fail;
	drm_atomic_set_fb_for_plane(primary_state, set->fb);
	primary_state->crtc_x = 0;
	primary_state->crtc_y = 0;
	primary_state->crtc_h = set->mode->vdisplay;
	primary_state->crtc_w = set->mode->hdisplay;
	primary_state->src_x = set->x << 16;
	primary_state->src_y = set->y << 16;
	primary_state->src_h = set->mode->vdisplay << 16;
	primary_state->src_w = set->mode->hdisplay << 16;

commit:
	ret = update_output_state(state, set);
	if (ret)
		goto fail;

	ret = drm_atomic_commit(state);
	if (ret != 0)
		goto fail;

	/* Driver takes ownership of state on successful commit. */
	return 0;
fail:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_free(state);

	return ret;
backoff:
	drm_atomic_legacy_backoff(state);
	drm_atomic_state_clear(state);

	/*
	 * Someone might have exchanged the framebuffer while we dropped locks
	 * in the backoff code. We need to fix up the fb refcount tracking the
	 * core does for us.
	 */
	crtc->primary->old_fb = crtc->primary->fb;

	goto retry;
}
EXPORT_SYMBOL(drm_atomic_helper_set_config);

/**
 * drm_atomic_helper_crtc_set_property - helper for crtc prorties
 * @crtc: DRM crtc
 * @property: DRM property
 * @val: value of property
 *
 * Provides a default plane disablle handler using the atomic driver interface.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int
drm_atomic_helper_crtc_set_property(struct drm_crtc *crtc,
				    struct drm_property *property,
				    uint64_t val)
{
	struct drm_atomic_state *state;
	struct drm_crtc_state *crtc_state;
	int ret = 0;

	state = drm_atomic_state_alloc(crtc->dev);
	if (!state)
		return -ENOMEM;

	/* ->set_property is always called with all locks held. */
	state->acquire_ctx = crtc->dev->mode_config.acquire_ctx;
retry:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto fail;
	}

	ret = crtc->funcs->atomic_set_property(crtc, crtc_state,
					       property, val);
	if (ret)
		goto fail;

	ret = drm_atomic_commit(state);
	if (ret != 0)
		goto fail;

	/* Driver takes ownership of state on successful commit. */
	return 0;
fail:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_free(state);

	return ret;
backoff:
	drm_atomic_legacy_backoff(state);
	drm_atomic_state_clear(state);

	goto retry;
}
EXPORT_SYMBOL(drm_atomic_helper_crtc_set_property);

/**
 * drm_atomic_helper_plane_set_property - helper for plane prorties
 * @plane: DRM plane
 * @property: DRM property
 * @val: value of property
 *
 * Provides a default plane disable handler using the atomic driver interface.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int
drm_atomic_helper_plane_set_property(struct drm_plane *plane,
				    struct drm_property *property,
				    uint64_t val)
{
	struct drm_atomic_state *state;
	struct drm_plane_state *plane_state;
	int ret = 0;

	state = drm_atomic_state_alloc(plane->dev);
	if (!state)
		return -ENOMEM;

	/* ->set_property is always called with all locks held. */
	state->acquire_ctx = plane->dev->mode_config.acquire_ctx;
retry:
	plane_state = drm_atomic_get_plane_state(state, plane);
	if (IS_ERR(plane_state)) {
		ret = PTR_ERR(plane_state);
		goto fail;
	}

	ret = plane->funcs->atomic_set_property(plane, plane_state,
					       property, val);
	if (ret)
		goto fail;

	ret = drm_atomic_commit(state);
	if (ret != 0)
		goto fail;

	/* Driver takes ownership of state on successful commit. */
	return 0;
fail:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_free(state);

	return ret;
backoff:
	drm_atomic_legacy_backoff(state);
	drm_atomic_state_clear(state);

	goto retry;
}
EXPORT_SYMBOL(drm_atomic_helper_plane_set_property);

/**
 * drm_atomic_helper_connector_set_property - helper for connector prorties
 * @connector: DRM connector
 * @property: DRM property
 * @val: value of property
 *
 * Provides a default plane disablle handler using the atomic driver interface.
 *
 * RETURNS:
 * Zero on success, error code on failure
 */
int
drm_atomic_helper_connector_set_property(struct drm_connector *connector,
				    struct drm_property *property,
				    uint64_t val)
{
	struct drm_atomic_state *state;
	struct drm_connector_state *connector_state;
	int ret = 0;

	state = drm_atomic_state_alloc(connector->dev);
	if (!state)
		return -ENOMEM;

	/* ->set_property is always called with all locks held. */
	state->acquire_ctx = connector->dev->mode_config.acquire_ctx;
retry:
	connector_state = drm_atomic_get_connector_state(state, connector);
	if (IS_ERR(connector_state)) {
		ret = PTR_ERR(connector_state);
		goto fail;
	}

	ret = connector->funcs->atomic_set_property(connector, connector_state,
					       property, val);
	if (ret)
		goto fail;

	ret = drm_atomic_commit(state);
	if (ret != 0)
		goto fail;

	/* Driver takes ownership of state on successful commit. */
	return 0;
fail:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_free(state);

	return ret;
backoff:
	drm_atomic_legacy_backoff(state);
	drm_atomic_state_clear(state);

	goto retry;
}
EXPORT_SYMBOL(drm_atomic_helper_connector_set_property);

/**
 * drm_atomic_helper_page_flip - execute a legacy page flip
 * @crtc: DRM crtc
 * @fb: DRM framebuffer
 * @event: optional DRM event to signal upon completion
 * @flags: flip flags for non-vblank sync'ed updates
 *
 * Provides a default page flip implementation using the atomic driver interface.
 *
 * Note that for now so called async page flips (i.e. updates which are not
 * synchronized to vblank) are not supported, since the atomic interfaces have
 * no provisions for this yet.
 *
 * Returns:
 * Returns 0 on success, negative errno numbers on failure.
 */
int drm_atomic_helper_page_flip(struct drm_crtc *crtc,
				struct drm_framebuffer *fb,
				struct drm_pending_vblank_event *event,
				uint32_t flags)
{
	struct drm_plane *plane = crtc->primary;
	struct drm_atomic_state *state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	int ret = 0;

	if (flags & DRM_MODE_PAGE_FLIP_ASYNC)
		return -EINVAL;

	state = drm_atomic_state_alloc(plane->dev);
	if (!state)
		return -ENOMEM;

	state->acquire_ctx = drm_modeset_legacy_acquire_ctx(crtc);
retry:
	crtc_state = drm_atomic_get_crtc_state(state, crtc);
	if (IS_ERR(crtc_state)) {
		ret = PTR_ERR(crtc_state);
		goto fail;
	}
	crtc_state->event = event;

	plane_state = drm_atomic_get_plane_state(state, plane);
	if (IS_ERR(plane_state)) {
		ret = PTR_ERR(plane_state);
		goto fail;
	}

	ret = drm_atomic_set_crtc_for_plane(plane_state, crtc);
	if (ret != 0)
		goto fail;
	drm_atomic_set_fb_for_plane(plane_state, fb);

	ret = drm_atomic_async_commit(state);
	if (ret != 0)
		goto fail;

	/* TODO: ->page_flip is the only driver callback where the core
	 * doesn't update plane->fb. For now patch it up here. */
	plane->fb = plane->state->fb;

	/* Driver takes ownership of state on successful async commit. */
	return 0;
fail:
	if (ret == -EDEADLK)
		goto backoff;

	drm_atomic_state_free(state);

	return ret;
backoff:
	drm_atomic_legacy_backoff(state);
	drm_atomic_state_clear(state);

	/*
	 * Someone might have exchanged the framebuffer while we dropped locks
	 * in the backoff code. We need to fix up the fb refcount tracking the
	 * core does for us.
	 */
	plane->old_fb = plane->fb;

	goto retry;
}
EXPORT_SYMBOL(drm_atomic_helper_page_flip);

/**
 * DOC: atomic state reset and initialization
 *
 * Both the drm core and the atomic helpers assume that there is always the full
 * and correct atomic software state for all connectors, CRTCs and planes
 * available. Which is a bit a problem on driver load and also after system
 * suspend. One way to solve this is to have a hardware state read-out
 * infrastructure which reconstructs the full software state (e.g. the i915
 * driver).
 *
 * The simpler solution is to just reset the software state to everything off,
 * which is easiest to do by calling drm_mode_config_reset(). To facilitate this
 * the atomic helpers provide default reset implementations for all hooks.
 */

/**
 * drm_atomic_helper_crtc_reset - default ->reset hook for CRTCs
 * @crtc: drm CRTC
 *
 * Resets the atomic state for @crtc by freeing the state pointer (which might
 * be NULL, e.g. at driver load time) and allocating a new empty state object.
 */
void drm_atomic_helper_crtc_reset(struct drm_crtc *crtc)
{
	kfree(crtc->state);
	crtc->state = kzalloc(sizeof(*crtc->state), GFP_KERNEL);
}
EXPORT_SYMBOL(drm_atomic_helper_crtc_reset);

/**
 * drm_atomic_helper_crtc_duplicate_state - default state duplicate hook
 * @crtc: drm CRTC
 *
 * Default CRTC state duplicate hook for drivers which don't have their own
 * subclassed CRTC state structure.
 */
struct drm_crtc_state *
drm_atomic_helper_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct drm_crtc_state *state;

	if (WARN_ON(!crtc->state))
		return NULL;

	state = kmemdup(crtc->state, sizeof(*crtc->state), GFP_KERNEL);

	if (state) {
		state->mode_changed = false;
		state->planes_changed = false;
		state->event = NULL;
	}

	return state;
}
EXPORT_SYMBOL(drm_atomic_helper_crtc_duplicate_state);

/**
 * drm_atomic_helper_crtc_destroy_state - default state destroy hook
 * @crtc: drm CRTC
 * @state: CRTC state object to release
 *
 * Default CRTC state destroy hook for drivers which don't have their own
 * subclassed CRTC state structure.
 */
void drm_atomic_helper_crtc_destroy_state(struct drm_crtc *crtc,
					  struct drm_crtc_state *state)
{
	kfree(state);
}
EXPORT_SYMBOL(drm_atomic_helper_crtc_destroy_state);

/**
 * drm_atomic_helper_plane_reset - default ->reset hook for planes
 * @plane: drm plane
 *
 * Resets the atomic state for @plane by freeing the state pointer (which might
 * be NULL, e.g. at driver load time) and allocating a new empty state object.
 */
void drm_atomic_helper_plane_reset(struct drm_plane *plane)
{
	if (plane->state && plane->state->fb)
		drm_framebuffer_unreference(plane->state->fb);

	kfree(plane->state);
	plane->state = kzalloc(sizeof(*plane->state), GFP_KERNEL);
}
EXPORT_SYMBOL(drm_atomic_helper_plane_reset);

/**
 * drm_atomic_helper_plane_duplicate_state - default state duplicate hook
 * @plane: drm plane
 *
 * Default plane state duplicate hook for drivers which don't have their own
 * subclassed plane state structure.
 */
struct drm_plane_state *
drm_atomic_helper_plane_duplicate_state(struct drm_plane *plane)
{
	struct drm_plane_state *state;

	if (WARN_ON(!plane->state))
		return NULL;

	state = kmemdup(plane->state, sizeof(*plane->state), GFP_KERNEL);

	if (state && state->fb)
		drm_framebuffer_reference(state->fb);

	return state;
}
EXPORT_SYMBOL(drm_atomic_helper_plane_duplicate_state);

/**
 * drm_atomic_helper_plane_destroy_state - default state destroy hook
 * @plane: drm plane
 * @state: plane state object to release
 *
 * Default plane state destroy hook for drivers which don't have their own
 * subclassed plane state structure.
 */
void drm_atomic_helper_plane_destroy_state(struct drm_plane *plane,
					   struct drm_plane_state *state)
{
	if (state->fb)
		drm_framebuffer_unreference(state->fb);

	kfree(state);
}
EXPORT_SYMBOL(drm_atomic_helper_plane_destroy_state);

/**
 * drm_atomic_helper_connector_reset - default ->reset hook for connectors
 * @connector: drm connector
 *
 * Resets the atomic state for @connector by freeing the state pointer (which
 * might be NULL, e.g. at driver load time) and allocating a new empty state
 * object.
 */
void drm_atomic_helper_connector_reset(struct drm_connector *connector)
{
	kfree(connector->state);
	connector->state = kzalloc(sizeof(*connector->state), GFP_KERNEL);
}
EXPORT_SYMBOL(drm_atomic_helper_connector_reset);

/**
 * drm_atomic_helper_connector_duplicate_state - default state duplicate hook
 * @connector: drm connector
 *
 * Default connector state duplicate hook for drivers which don't have their own
 * subclassed connector state structure.
 */
struct drm_connector_state *
drm_atomic_helper_connector_duplicate_state(struct drm_connector *connector)
{
	if (WARN_ON(!connector->state))
		return NULL;

	return kmemdup(connector->state, sizeof(*connector->state), GFP_KERNEL);
}
EXPORT_SYMBOL(drm_atomic_helper_connector_duplicate_state);

/**
 * drm_atomic_helper_connector_destroy_state - default state destroy hook
 * @connector: drm connector
 * @state: connector state object to release
 *
 * Default connector state destroy hook for drivers which don't have their own
 * subclassed connector state structure.
 */
void drm_atomic_helper_connector_destroy_state(struct drm_connector *connector,
					  struct drm_connector_state *state)
{
	kfree(state);
}
EXPORT_SYMBOL(drm_atomic_helper_connector_destroy_state);
