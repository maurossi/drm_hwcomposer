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

#ifndef ANDROID_DRMDISPLAYPIPELINE_H_
#define ANDROID_DRMDISPLAYPIPELINE_H_

#include <memory>

#include "DrmConnector.h"
#include "DrmCrtc.h"
#include "DrmDevice.h"
#include "DrmEncoder.h"

namespace android {

class DrmConnector;
class DrmDevice;
class DrmCrtc;
class DrmEncoder;
class DrmDisplayCompositor;

struct DrmDisplayPipeline {
  DrmDisplayPipeline(DrmDisplayPipeline &&) = default;

  static auto CreatePipeline(DrmConnector &connector)
      -> std::unique_ptr<DrmDisplayPipeline>;

  DrmDevice *const device;

  /* At this moment single-connector only pipelines supported */
  DrmConnector *const connector;

  /* Pipeline */
  DrmCrtc *const crtc;
  DrmEncoder *const encoder;

  std::unique_ptr<DrmDisplayCompositor> compositor;

  DrmEncoderOwner enc_holder;
  DrmCrtcOwner crtc_holder;

  DrmPlaneOwner primary_plane;

  std::vector<DrmPlaneOwner> overlay_planes;
};

}  // namespace android

#endif
