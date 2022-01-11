/*
 * Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "hwc-drm-two"

#include "DrmHwcTwo.h"

#include "backend/Backend.h"
#include "utils/log.h"

namespace android {

DrmHwcTwo::DrmHwcTwo() = default;

HWC2::Error DrmHwcTwo::Init() {
  int err = resource_manager_.Init();
  if (err != 0) {
    ALOGE("Can't initialize the resource manager %d", err);
    return HWC2::Error::NoResources;
  }

  uint32_t disp_handle = kPrimaryDisplay;
  for (auto &conn_pair : resource_manager_.GetAvailableConnectors()) {
    ALOGI("Registering disp for connector %s",
          conn_pair.second->object->name().c_str());
    auto disp = std::make_unique<HwcDisplay>(conn_pair.second->object,
                                             disp_handle,
                                             HWC2::DisplayType::Physical, this);
    displays_[disp_handle] = std::move(disp);
    disp_handle++;
  }

  resource_manager_.GetUEventListener()->RegisterHotplugHandler([this] {
    const std::lock_guard<std::mutex> lock(GetResMan()->GetMasterLock());
    UpdateAllDiaplaysHotplugState();
  });

  return HWC2::Error::None;
}

HWC2::Error DrmHwcTwo::CreateVirtualDisplay(uint32_t /*width*/,
                                            uint32_t /*height*/,
                                            int32_t * /*format*/,
                                            hwc2_display_t * /*display*/) {
  // TODO(nobody): Implement virtual display
  return HWC2::Error::Unsupported;
}

HWC2::Error DrmHwcTwo::DestroyVirtualDisplay(hwc2_display_t /*display*/) {
  // TODO(nobody): Implement virtual display
  return HWC2::Error::Unsupported;
}

void DrmHwcTwo::Dump(uint32_t *outSize, char *outBuffer) {
  if (outBuffer != nullptr) {
    auto copied_bytes = mDumpString.copy(outBuffer, *outSize);
    *outSize = static_cast<uint32_t>(copied_bytes);
    return;
  }

  std::stringstream output;

  output << "-- drm_hwcomposer --\n\n";

  for (auto &disp : displays_)
    output << disp.second->Dump();

  mDumpString = output.str();
  *outSize = static_cast<uint32_t>(mDumpString.size());
}

uint32_t DrmHwcTwo::GetMaxVirtualDisplayCount() {
  // TODO(nobody): Implement virtual display
  return 0;
}

HWC2::Error DrmHwcTwo::RegisterCallback(int32_t descriptor,
                                        hwc2_callback_data_t data,
                                        hwc2_function_pointer_t function) {
  switch (static_cast<HWC2::Callback>(descriptor)) {
    case HWC2::Callback::Hotplug: {
      hotplug_callback_ = std::make_pair(HWC2_PFN_HOTPLUG(function), data);
      if (displays_.empty()) {
        Init();
      }
      UpdateAllDiaplaysHotplugState(/*force_send_connected = */ true);
      break;
    }
    case HWC2::Callback::Refresh: {
      refresh_callback_ = std::make_pair(HWC2_PFN_REFRESH(function), data);
      break;
    }
    case HWC2::Callback::Vsync: {
      vsync_callback_ = std::make_pair(HWC2_PFN_VSYNC(function), data);
      break;
    }
#if PLATFORM_SDK_VERSION > 29
    case HWC2::Callback::Vsync_2_4: {
      vsync_2_4_callback_ = std::make_pair(HWC2_PFN_VSYNC_2_4(function), data);
      break;
    }
#endif
    default:
      break;
  }
  return HWC2::Error::None;
}

void DrmHwcTwo::SendHotplugEventToClient(hwc2_display_t displayid,
                                         bool connected) {
  if (hotplug_callback_.first != nullptr &&
      hotplug_callback_.second != nullptr) {
    resource_manager_.GetMasterLock().unlock();
    hotplug_callback_.first(hotplug_callback_.second, displayid,
                            connected ? HWC2_CONNECTION_CONNECTED
                                      : HWC2_CONNECTION_DISCONNECTED);
    resource_manager_.GetMasterLock().lock();
  }
}

void DrmHwcTwo::UpdateAllDiaplaysHotplugState(bool force_send_connected) {
  for (auto &display : displays_) {
    display.second->HandleHotplug(force_send_connected);
  }
}

}  // namespace android
