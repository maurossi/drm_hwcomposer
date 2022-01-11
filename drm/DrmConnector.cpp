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

#define LOG_TAG "hwc-drm-connector"

#include "DrmConnector.h"

#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <sstream>

#include "DrmDevice.h"
#include "utils/log.h"

#ifndef DRM_MODE_CONNECTOR_SPI
#define DRM_MODE_CONNECTOR_SPI 19
#endif

#ifndef DRM_MODE_CONNECTOR_USB
#define DRM_MODE_CONNECTOR_USB 20
#endif

namespace android {

constexpr size_t kTypesCount = 21;

static bool GetConnectorProperty(const DrmDevice &dev,
                                 const DrmConnector &connector,
                                 const char *prop_name, DrmProperty *property) {
  int err = dev.GetProperty(connector.GetId(), DRM_MODE_OBJECT_CONNECTOR,
                            prop_name, property);
  if (err != 0) {
    ALOGE("Could not get %s property\n", prop_name);
    return false;
  }
  return true;
}

auto DrmConnector::CreateInstance(DrmDevice &dev, uint32_t connector_id,
                                  uint32_t index)
    -> std::unique_ptr<DrmConnector> {
  auto conn = MakeDrmModeConnectorUnique(dev.fd(), connector_id);
  if (!conn) {
    ALOGE("Failed to get connector %d", connector_id);
    return {};
  }

  auto c = std::unique_ptr<DrmConnector>(
      new DrmConnector(std::move(conn), &dev, index));

  if (!GetConnectorProperty(dev, *c, "DPMS", &c->dpms_property_) ||
      !GetConnectorProperty(dev, *c, "CRTC_ID", &c->crtc_id_property_)) {
    return {};
  }
  c->UpdateEdidProperty();
  if (c->writeback() &&
      (!GetConnectorProperty(dev, *c, "WRITEBACK_PIXEL_FORMATS",
                             &c->writeback_pixel_formats_) ||
       !GetConnectorProperty(dev, *c, "WRITEBACK_FB_ID",
                             &c->writeback_fb_id_) ||
       !GetConnectorProperty(dev, *c, "WRITEBACK_OUT_FENCE_PTR",
                             &c->writeback_out_fence_))) {
    return {};
  }

  return c;
}

int DrmConnector::UpdateEdidProperty() {
  return GetConnectorProperty(*drm_, *this, "EDID", &edid_property_) ? 0
                                                                     : -EINVAL;
}

auto DrmConnector::GetEdidBlob() -> DrmModePropertyBlobUnique {
  uint64_t blob_id = 0;
  int ret = UpdateEdidProperty();
  if (ret != 0) {
    return {};
  }

  std::tie(ret, blob_id) = edid_property().value();
  if (ret != 0) {
    return {};
  }

  return MakeDrmModePropertyBlobUnique(drm_->fd(), blob_id);
}

bool DrmConnector::internal() const {
  auto type = connector_->connector_type;
  return type == DRM_MODE_CONNECTOR_LVDS || type == DRM_MODE_CONNECTOR_eDP ||
         type == DRM_MODE_CONNECTOR_DSI || type == DRM_MODE_CONNECTOR_VIRTUAL ||
         type == DRM_MODE_CONNECTOR_DPI || type == DRM_MODE_CONNECTOR_SPI;
}

bool DrmConnector::external() const {
  auto type = connector_->connector_type;
  return type == DRM_MODE_CONNECTOR_HDMIA ||
         type == DRM_MODE_CONNECTOR_DisplayPort ||
         type == DRM_MODE_CONNECTOR_DVID || type == DRM_MODE_CONNECTOR_DVII ||
         type == DRM_MODE_CONNECTOR_VGA || type == DRM_MODE_CONNECTOR_USB;
}

bool DrmConnector::writeback() const {
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
  return connector_->connector_type == DRM_MODE_CONNECTOR_WRITEBACK;
#else
  return false;
#endif
}

bool DrmConnector::valid_type() const {
  return internal() || external() || writeback();
}

std::string DrmConnector::name() const {
  constexpr std::array<const char *, kTypesCount> kNames =
      {"None",      "VGA",  "DVI-I",     "DVI-D",   "DVI-A", "Composite",
       "SVIDEO",    "LVDS", "Component", "DIN",     "DP",    "HDMI-A",
       "HDMI-B",    "TV",   "eDP",       "Virtual", "DSI",   "DPI",
       "Writeback", "SPI",  "USB"};

  if (connector_->connector_type < kTypesCount) {
    std::ostringstream name_buf;
    name_buf << kNames[connector_->connector_type] << "-"
             << connector_->connector_type_id;
    return name_buf.str();
  }

  ALOGE("Unknown type in connector %d, could not make his name", GetId());
  return "None";
}

int DrmConnector::UpdateModes() {
  auto conn = MakeDrmModeConnectorUnique(drm_->fd(), GetId());
  if (!conn) {
    ALOGE("Failed to get connector %d", GetId());
    return -ENODEV;
  }
  connector_ = std::move(conn);

  modes_.clear();
  for (int i = 0; i < connector_->count_modes; ++i) {
    bool exists = false;
    for (const DrmMode &mode : modes_) {
      if (mode == connector_->modes[i]) {
        exists = true;
        break;
      }
    }

    if (!exists) {
      modes_.emplace_back(DrmMode(&connector_->modes[i]));
    }
  }

  return 0;
}

const DrmMode &DrmConnector::active_mode() const {
  return active_mode_;
}

void DrmConnector::set_active_mode(DrmMode &mode) {
  active_mode_ = mode;
}

const DrmProperty &DrmConnector::dpms_property() const {
  return dpms_property_;
}

const DrmProperty &DrmConnector::crtc_id_property() const {
  return crtc_id_property_;
}

const DrmProperty &DrmConnector::edid_property() const {
  return edid_property_;
}

const DrmProperty &DrmConnector::writeback_pixel_formats() const {
  return writeback_pixel_formats_;
}

const DrmProperty &DrmConnector::writeback_fb_id() const {
  return writeback_fb_id_;
}

const DrmProperty &DrmConnector::writeback_out_fence() const {
  return writeback_out_fence_;
}

uint32_t DrmConnector::mm_width() const {
  return connector_->mmWidth;
}

uint32_t DrmConnector::mm_height() const {
  return connector_->mmHeight;
}
}  // namespace android
