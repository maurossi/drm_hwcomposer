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

#define LOG_TAG "hwc-drm-device"

#include "DrmDevice.h"

#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <sstream>
#include <string>

#include "compositor/DrmDisplayCompositor.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmPlane.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android {

DrmDevice::DrmDevice() {
  self.reset(this);
  mDrmFbImporter = std::make_unique<DrmFbImporter>(self);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
auto DrmDevice::Init(const char *path) -> int {
  /* TODO: Use drmOpenControl here instead */
  fd_ = UniqueFd(open(path, O_RDWR | O_CLOEXEC));
  if (fd() < 0) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe): Fixme
    ALOGE("Failed to open dri %s: %s", path, strerror(errno));
    return -ENODEV;
  }

  int ret = drmSetClientCap(fd(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret) {
    ALOGE("Failed to set universal plane cap %d", ret);
    return ret;
  }

  ret = drmSetClientCap(fd(), DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret) {
    ALOGE("Failed to set atomic cap %d", ret);
    return ret;
  }

#ifdef DRM_CLIENT_CAP_WRITEBACK_CONNECTORS
  ret = drmSetClientCap(fd(), DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
  if (ret) {
    ALOGI("Failed to set writeback cap %d", ret);
    ret = 0;
  }
#endif

  uint64_t cap_value = 0;
  if (drmGetCap(fd(), DRM_CAP_ADDFB2_MODIFIERS, &cap_value)) {
    ALOGW("drmGetCap failed. Fallback to no modifier support.");
    cap_value = 0;
  }
  HasAddFb2ModifiersSupport_ = cap_value != 0;

  drmSetMaster(fd());
  if (!drmIsMaster(fd())) {
    ALOGE("DRM/KMS master access required");
    return -EACCES;
  }

  auto res = MakeDrmModeResUnique(fd());
  if (!res) {
    ALOGE("Failed to get DrmDevice resources");
    return -ENODEV;
  }

  min_resolution_ = std::pair<uint32_t, uint32_t>(res->min_width,
                                                  res->min_height);
  max_resolution_ = std::pair<uint32_t, uint32_t>(res->max_width,
                                                  res->max_height);

  for (int i = 0; !ret && i < res->count_crtcs; ++i) {
    auto crtc = DrmCrtc::CreateInstance(*this, res->crtcs[i], i);
    if (crtc) {
      crtcs_.emplace_back(std::move(crtc));
    }
  }

  std::vector<uint32_t> possible_clones;
  for (int i = 0; !ret && i < res->count_encoders; ++i) {
    auto enc = DrmEncoder::CreateInstance(*this, res->encoders[i], i);
    if (enc) {
      encoders_.emplace_back(std::move(enc));
    }
  }

  for (int i = 0; !ret && i < res->count_connectors; ++i) {
    auto conn = DrmConnector::CreateInstance(*this, res->connectors[i], i);

    if (!conn) {
      continue;
    }

    if (conn->writeback())
      writeback_connectors_.emplace_back(std::move(conn));
    else
      connectors_.emplace_back(std::move(conn));
  }

  // Catch-all for the above loops
  if (ret)
    return ret;

  auto plane_res = MakeDrmModePlaneResUnique(fd());
  if (!plane_res) {
    ALOGE("Failed to get plane resources");
    return -ENOENT;
  }

  for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
    auto p = MakeDrmModePlaneUnique(fd(), plane_res->planes[i]);
    if (!p) {
      ALOGE("Failed to get plane %d", plane_res->planes[i]);
      ret = -ENODEV;
      break;
    }

    std::unique_ptr<DrmPlane> plane(new DrmPlane(this, p.get()));

    ret = plane->Init();
    if (ret) {
      ALOGE("Init plane %d failed", plane_res->planes[i]);
      break;
    }

    planes_.emplace_back(std::move(plane));
  }

  return ret;
}

auto DrmDevice::RegisterUserPropertyBlob(void *data, size_t length) const
    -> DrmModeUserPropertyBlobUnique {
  struct drm_mode_create_blob create_blob {};
  create_blob.length = length;
  create_blob.data = (__u64)data;

  int ret = drmIoctl(fd(), DRM_IOCTL_MODE_CREATEPROPBLOB, &create_blob);
  if (ret) {
    ALOGE("Failed to create mode property blob %d", ret);
    return {};
  }

  return DrmModeUserPropertyBlobUnique(
      new uint32_t(create_blob.blob_id), [this](const uint32_t *it) {
        struct drm_mode_destroy_blob destroy_blob {};
        destroy_blob.blob_id = (__u32)*it;
        int err = drmIoctl(fd(), DRM_IOCTL_MODE_DESTROYPROPBLOB, &destroy_blob);
        if (err != 0) {
          ALOGE("Failed to destroy mode property blob %" PRIu32 "/%d", *it,
                err);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete it;
      });
}

int DrmDevice::GetProperty(uint32_t obj_id, uint32_t obj_type,
                           const char *prop_name, DrmProperty *property) const {
  drmModeObjectPropertiesPtr props = nullptr;

  props = drmModeObjectGetProperties(fd(), obj_id, obj_type);
  if (!props) {
    ALOGE("Failed to get properties for %d/%x", obj_id, obj_type);
    return -ENODEV;
  }

  bool found = false;
  for (int i = 0; !found && (size_t)i < props->count_props; ++i) {
    drmModePropertyPtr p = drmModeGetProperty(fd(), props->props[i]);
    if (!strcmp(p->name, prop_name)) {
      property->Init(obj_id, p, props->prop_values[i]);
      found = true;
    }
    drmModeFreeProperty(p);
  }

  drmModeFreeObjectProperties(props);
  return found ? 0 : -ENOENT;
}

std::string DrmDevice::GetName() const {
  auto *ver = drmGetVersion(fd());
  if (!ver) {
    ALOGW("Failed to get drm version for fd=%d", fd());
    return "generic";
  }

  std::string name(ver->name);
  drmFreeVersion(ver);
  return name;
}

auto DrmDevice::IsKMSDev(const char *path) -> bool {
  auto fd = UniqueFd(open(path, O_RDWR | O_CLOEXEC));
  if (!fd) {
    return false;
  }

  auto res = MakeDrmModeResUnique(fd.Get());
  if (!res) {
    return false;
  }

  bool is_kms = res->count_crtcs > 0 && res->count_connectors > 0 &&
                res->count_encoders > 0;

  return is_kms;
}

auto DrmDevice::GetConnectors()
    -> const std::vector<std::unique_ptr<DrmConnector>> & {
  return connectors_;
}

auto DrmDevice::GetPlanes() -> const std::vector<std::unique_ptr<DrmPlane>> & {
  return planes_;
}

auto DrmDevice::GetCrtcs() -> const std::vector<std::unique_ptr<DrmCrtc>> & {
  return crtcs_;
}

auto DrmDevice::GetEncoders()
    -> const std::vector<std::unique_ptr<DrmEncoder>> & {
  return encoders_;
}

}  // namespace android
