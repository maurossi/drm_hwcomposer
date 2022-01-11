/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "hwc-resource-manager"

#include "ResourceManager.h"

#include <fcntl.h>
#include <sys/stat.h>

#include <sstream>

#include "bufferinfo/BufferInfoGetter.h"
#include "drm/DrmDevice.h"
#include "drm/DrmPlane.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android {

ResourceManager::ResourceManager() = default;

ResourceManager::~ResourceManager() {
  uevent_listener_.Exit();
}

int ResourceManager::Init() {
  char path_pattern[PROPERTY_VALUE_MAX];
  // Could be a valid path or it can have at the end of it the wildcard %
  // which means that it will try open all devices until an error is met.
  int path_len = property_get("vendor.hwc.drm.device", path_pattern,
                              "/dev/dri/card%");
  int ret = 0;
  if (path_pattern[path_len - 1] != '%') {
    ret = AddDrmDevice(std::string(path_pattern));
  } else {
    path_pattern[path_len - 1] = '\0';
    for (int idx = 0; ret == 0; ++idx) {
      std::ostringstream path;
      path << path_pattern << idx;

      struct stat buf {};
      if (stat(path.str().c_str(), &buf) != 0)
        break;

      if (DrmDevice::IsKMSDev(path.str().c_str()))
        ret = AddDrmDevice(path.str());
    }
  }

  ReorderConnectors();

  if (connectors_.empty()) {
    ALOGE("Failed to initialize any displays");
    return ret != 0 ? -EINVAL : ret;
  }

  char scale_with_gpu[PROPERTY_VALUE_MAX];
  property_get("vendor.hwc.drm.scale_with_gpu", scale_with_gpu, "0");
  scale_with_gpu_ = bool(strncmp(scale_with_gpu, "0", 1));

  if (BufferInfoGetter::GetInstance() == nullptr) {
    ALOGE("Failed to initialize BufferInfoGetter");
    return -EINVAL;
  }

  ret = uevent_listener_.Init();
  if (ret != 0) {
    ALOGE("Can't initialize event listener %d", ret);
    return ret;
  }

  return 0;
}

int ResourceManager::AddDrmDevice(const std::string &path) {
  auto drm = std::make_unique<DrmDevice>();
  int ret = drm->Init(path.c_str());
  drms_.push_back(std::move(drm));
  return ret;
}

static void TrimLeft(std::string *str) {
  str->erase(std::begin(*str),
             std::find_if(std::begin(*str), std::end(*str),
                          [](int ch) { return std::isspace(ch) == 0; }));
}

static void TrimRight(std::string *str) {
  str->erase(std::find_if(std::rbegin(*str), std::rend(*str),
                          [](int ch) { return std::isspace(ch) == 0; })
                 .base(),
             std::end(*str));
}

static void Trim(std::string *str) {
  TrimLeft(str);
  TrimRight(str);
}

static std::vector<std::string> ReadPrimaryDisplayOrderProp() {
  std::array<char, PROPERTY_VALUE_MAX> display_order_buf{};
  property_get("vendor.hwc.drm.primary_display_order", display_order_buf.data(),
               "...");

  std::vector<std::string> display_order;
  std::istringstream str(display_order_buf.data());
  for (std::string conn_name; std::getline(str, conn_name, ',');) {
    Trim(&conn_name);
    display_order.push_back(std::move(conn_name));
  }
  return display_order;
}

static std::vector<DrmConnector *> MakePrimaryDisplayCandidates(
    const std::vector<DrmConnector *> &connectors) {
  std::vector<DrmConnector *> primary_candidates(connectors);
  primary_candidates.erase(std::remove_if(std::begin(primary_candidates),
                                          std::end(primary_candidates),
                                          [](const DrmConnector *conn) {
                                            return conn->state() !=
                                                   DRM_MODE_CONNECTED;
                                          }),
                           std::end(primary_candidates));

  std::vector<std::string> display_order = ReadPrimaryDisplayOrderProp();
  bool use_other = display_order.back() == "...";

  // putting connectors from primary_display_order first
  auto curr_connector = std::begin(primary_candidates);
  for (const std::string &display_name : display_order) {
    auto it = std::find_if(std::begin(primary_candidates),
                           std::end(primary_candidates),
                           [&display_name](const DrmConnector *conn) {
                             return conn->name() == display_name;
                           });
    if (it != std::end(primary_candidates)) {
      std::iter_swap(it, curr_connector);
      ++curr_connector;
    }
  }

  if (use_other) {
    // then putting internal connectors second, everything else afterwards
    std::partition(curr_connector, std::end(primary_candidates),
                   [](const DrmConnector *conn) { return conn->internal(); });
  } else {
    primary_candidates.erase(curr_connector, std::end(primary_candidates));
  }

  return primary_candidates;
}

void ResourceManager::ReorderConnectors() {
  bool found_primary = false;
  int num_displays = 1;

  std::vector<DrmConnector *> all_connectors;

  for (auto &drm : drms_) {
    for (const auto &conn : drm->GetConnectors()) {
      all_connectors.emplace_back(conn.get());
    }
  }

  // Primary display priority:
  // 1) vendor.hwc.drm.primary_display_order property
  // 2) internal connectors
  // 3) anything else
  auto primary_candidates = MakePrimaryDisplayCandidates(all_connectors);
  if (!primary_candidates.empty() && !found_primary) {
    DrmConnector &conn = **std::begin(primary_candidates);
    auto owned_conn = OwnDrmObject(&conn, this);
    if (owned_conn) {
      connectors_[num_displays] = owned_conn;
      found_primary = true;
      ++num_displays;
    }
  } else {
    ALOGE(
        "Failed to find primary display from "
        "\"vendor.hwc.drm.primary_display_order\" property");
  }

  // If no priority display were found then pick first available as primary and
  // for the others assign consecutive display_numbers.
  for (auto *conn : all_connectors) {
    if (conn->external() || conn->internal()) {
      if (!found_primary) {
        auto owned_conn = OwnDrmObject(conn, this);
        if (owned_conn) {
          connectors_[num_displays] = owned_conn;
          found_primary = true;
          ++num_displays;
        }
      } else {
        auto owned_conn = OwnDrmObject(conn, this);
        if (owned_conn) {
          connectors_[num_displays] = owned_conn;
          ++num_displays;
        }
      }
    }
  }
}

}  // namespace android
