/*
 * Copyright (C) 2015 The Android Open Source Project
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

#ifndef ANDROID_DRM_CRTC_H_
#define ANDROID_DRM_CRTC_H_

#include <xf86drmMode.h>

#include <cstdint>

#include "DrmMode.h"
#include "DrmProperty.h"
#include "DrmUnique.h"

namespace android {

class DrmDevice;

class DrmCrtc {
 public:
  static auto CreateInstance(DrmDevice &dev, uint32_t crtc_id, uint32_t index)
      -> std::unique_ptr<DrmCrtc>;

  DrmCrtc() = delete;
  DrmCrtc(const DrmCrtc &) = delete;
  DrmCrtc &operator=(const DrmCrtc &) = delete;

  auto GetId() const -> uint32_t;

  auto GetIndexInResArray() const -> uint32_t {
    return index_in_res_array_;
  }

  const DrmProperty &active_property() const;
  const DrmProperty &mode_property() const;
  const DrmProperty &out_fence_ptr_property() const;

  DrmCrtcOwnerWeak owned;

 private:
  DrmCrtc(DrmModeCrtcUnique crtc, uint32_t index)
      : crtc_(std::move(crtc)), index_in_res_array_(index){};

  DrmModeCrtcUnique crtc_;

  const uint32_t index_in_res_array_;

  DrmProperty active_property_;
  DrmProperty mode_property_;
  DrmProperty out_fence_ptr_property_;
};
}  // namespace android

#endif  // ANDROID_DRM_CRTC_H_
