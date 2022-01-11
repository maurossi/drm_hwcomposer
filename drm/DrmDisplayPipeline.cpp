/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "hwc-drm-display-pipeline"

#include "DrmDisplayPipeline.h"

#include "DrmDevice.h"
#include "DrmPlane.h"
#include "compositor/DrmDisplayCompositor.h"
#include "utils/log.h"

namespace android {

static auto TryCreatePipeline(DrmDevice &dev, DrmConnector &connector,
                              DrmEncoder &enc, DrmCrtc &crtc)
    -> std::unique_ptr<DrmDisplayPipeline> {
  /* Check if resources are available */

  auto enc_holder = OwnDrmObject(&enc, &connector);
  if (!enc_holder) {
    return {};
  }

  auto crtc_holder = OwnDrmObject(&crtc, &connector);
  if (!crtc_holder) {
    return {};
  }

  std::vector<DrmPlane *> primary_planes;
  std::vector<DrmPlane *> overlay_planes;

  /* Attach necessary resources */
  auto display_planes = std::vector<DrmPlane *>();
  for (const auto &plane : dev.GetPlanes()) {
    if (plane->IsCrtcSupported(crtc)) {
      if (plane->GetType() == DRM_PLANE_TYPE_PRIMARY) {
        primary_planes.emplace_back(plane.get());
      } else if (plane->GetType() == DRM_PLANE_TYPE_OVERLAY) {
        overlay_planes.emplace_back(plane.get());
      } else {
        ALOGI("Ignoring cursor plane %d", plane->GetId());
      }
    }
  }

  if (primary_planes.empty()) {
    ALOGE("Primary plane for CRTC %d not found", crtc.GetId());
    return {};
  }

  if (primary_planes.size() > 1) {
    ALOGE("Found more than 1 primary plane for CRTC %d", crtc.GetId());
    return {};
  }

  auto owned_primary_plane = TakeDrmObject(primary_planes[0], &connector);
  if (!owned_primary_plane) {
    ALOGE("Primary plane %d is aleady owned. Internal error.",
          primary_planes[0]->GetId());
    return {};
  }

  std::vector<DrmPlaneOwner> owned_overlay_planes;

  bool use_overlay_planes = true;  // TODO(rsglobal): restore
                                   // strtol(use_overlay_planes_prop, nullptr,
                                   // 10);
  if (use_overlay_planes) {
    for (auto *plane : overlay_planes) {
      auto op = OwnDrmObject(plane, &connector);
      if (op) {
        owned_overlay_planes.emplace_back(op);
      }
    }
  }

  auto pipeline = (DrmDisplayPipeline){
      .device = &dev,
      .connector = &connector,
      .crtc = &crtc,
      .encoder = &enc,
      .enc_holder = enc_holder,
      .crtc_holder = crtc_holder,
      .primary_plane = owned_primary_plane,
      .overlay_planes = owned_overlay_planes,
  };

  auto uniquepipe = std::make_unique<DrmDisplayPipeline>(std::move(pipeline));

  uniquepipe->compositor = std::make_unique<DrmDisplayCompositor>(
      uniquepipe.get());

  return uniquepipe;
}

static auto TryCreatePipelineUsingEncoder(DrmDevice &dev, DrmConnector &conn,
                                          DrmEncoder &enc)
    -> std::unique_ptr<DrmDisplayPipeline> {
  /* First try to use the currently-bound crtc */
  auto *crtc = dev.FindCrtcById(enc.GetCurrentCrtcId());
  if (crtc != nullptr) {
    auto pipeline = TryCreatePipeline(dev, conn, enc, *crtc);
    if (pipeline) {
      return pipeline;
    }
  }

  /* Try to find a possible crtc which will work */
  for (const auto &crtc : dev.GetCrtcs()) {
    if (enc.SupportsCrtc(*crtc)) {
      auto pipeline = TryCreatePipeline(dev, conn, enc, *crtc);
      if (pipeline) {
        return pipeline;
      }
    }
  }

  /* We can't use this encoder, but nothing went wrong, try another one */
  return {};
}

auto DrmDisplayPipeline::CreatePipeline(DrmConnector &connector)
    -> std::unique_ptr<DrmDisplayPipeline> {
  auto &dev = connector.GetDev();
  /* Try to use current setup first */
  auto *encoder = dev.FindEncoderById(connector.GetCurrentEncoderId());

  if (encoder != nullptr) {
    auto pipeline = TryCreatePipelineUsingEncoder(dev, connector, *encoder);
    if (pipeline) {
      return pipeline;
    }
  }

  for (const auto &enc : dev.GetEncoders()) {
    if (connector.SupportsEncoder(*enc)) {
      auto pipeline = TryCreatePipelineUsingEncoder(dev, connector, *enc);
      if (pipeline) {
        return pipeline;
      }
    }
  }

  ALOGE("Could not find a suitable encoder/crtc for connector %s",
        connector.name().c_str());

  return {};
}

}  // namespace android
