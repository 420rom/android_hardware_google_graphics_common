/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include <drm/drm_mode.h>
#include "ExynosDeviceDrmInterface.h"
#include "ExynosDisplayDrmInterface.h"
#include "ExynosHWCDebug.h"
#include "ExynosDevice.h"
#include "ExynosDisplay.h"
#include "ExynosExternalDisplayModule.h"
#include <hardware/hwcomposer_defs.h>
#include "DeconDrmHeader.h"

static void set_dpp_ch_restriction(struct dpp_ch_restriction &common_restriction,
        struct drm_dpp_ch_restriction &drm_restriction)
{
    common_restriction.id = drm_restriction.id;
    common_restriction.attr = drm_restriction.attr;
    common_restriction.restriction.src_f_w = drm_restriction.restriction.src_f_w;
    common_restriction.restriction.src_f_h = drm_restriction.restriction.src_f_h;
    common_restriction.restriction.src_w = drm_restriction.restriction.src_w;
    common_restriction.restriction.src_h = drm_restriction.restriction.src_h;
    common_restriction.restriction.src_x_align = drm_restriction.restriction.src_x_align;
    common_restriction.restriction.src_y_align = drm_restriction.restriction.src_y_align;
    common_restriction.restriction.dst_f_w = drm_restriction.restriction.dst_f_w;
    common_restriction.restriction.dst_f_h = drm_restriction.restriction.dst_f_h;
    common_restriction.restriction.dst_w = drm_restriction.restriction.dst_w;
    common_restriction.restriction.dst_h = drm_restriction.restriction.dst_h;
    common_restriction.restriction.dst_x_align = drm_restriction.restriction.dst_x_align;
    common_restriction.restriction.dst_y_align = drm_restriction.restriction.dst_y_align;
    common_restriction.restriction.blk_w = drm_restriction.restriction.blk_w;
    common_restriction.restriction.blk_h = drm_restriction.restriction.blk_h;
    common_restriction.restriction.blk_x_align = drm_restriction.restriction.blk_x_align;
    common_restriction.restriction.blk_y_align = drm_restriction.restriction.blk_y_align;
    common_restriction.restriction.src_h_rot_max = drm_restriction.restriction.src_h_rot_max;
    common_restriction.restriction.scale_down = drm_restriction.restriction.scale_down;
    common_restriction.restriction.scale_up = drm_restriction.restriction.scale_up;

    /* scale ratio can't be 0 */
    if (common_restriction.restriction.scale_down == 0)
        common_restriction.restriction.scale_down = 1;
    if (common_restriction.restriction.scale_up == 0)
        common_restriction.restriction.scale_up = 1;
}

ExynosDeviceDrmInterface::ExynosDeviceDrmInterface(ExynosDevice *exynosDevice)
{
    mType = INTERFACE_TYPE_DRM;
}

ExynosDeviceDrmInterface::~ExynosDeviceDrmInterface()
{
    mDrmDevice->event_listener()->UnRegisterHotplugHandler(static_cast<DrmEventHandler *>(&mExynosDrmEventHandler));
}

void ExynosDeviceDrmInterface::init(ExynosDevice *exynosDevice)
{
    mUseQuery = false;
    mExynosDevice = exynosDevice;
    mDrmResourceManager.Init();
    mDrmDevice = mDrmResourceManager.GetDrmDevice(HWC_DISPLAY_PRIMARY);
    assert(mDrmDevice != NULL);

    updateRestrictions();

    mExynosDrmEventHandler.init(mExynosDevice);
    mDrmDevice->event_listener()->RegisterHotplugHandler(static_cast<DrmEventHandler *>(&mExynosDrmEventHandler));

    ExynosDisplay *primaryDisplay = mExynosDevice->getDisplay(HWC_DISPLAY_PRIMARY);
    if (primaryDisplay != NULL) {
        ExynosDisplayDrmInterface *displayInterface = static_cast<ExynosDisplayDrmInterface*>(primaryDisplay->mDisplayInterface.get());
        displayInterface->initDrmDevice(mDrmDevice);
    }
    ExynosDisplay *externalDisplay = mExynosDevice->getDisplay(HWC_DISPLAY_EXTERNAL);
    if (externalDisplay != NULL) {
        ExynosDisplayDrmInterface *displayInterface = static_cast<ExynosDisplayDrmInterface*>(externalDisplay->mDisplayInterface.get());
        displayInterface->initDrmDevice(mDrmDevice);
    }

}

void ExynosDeviceDrmInterface::updateRestrictions()
{
    int32_t ret = 0;

    mDPUInfo.dpuInfo.dpp_cnt = mDrmDevice->planes().size();
    uint32_t channelId = 0;

    for (auto &plane : mDrmDevice->planes()) {
        /* Set size restriction information */
        if (plane->hw_restrictions_property().id()) {
            uint64_t blobId;
            std::tie(ret, blobId) = plane->hw_restrictions_property().value();
            if (ret)
                break;
            struct drm_dpp_ch_restriction *res;
            drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(mDrmDevice->fd(), blobId);
            if (!blob) {
                ALOGE("Fail to get blob for hw_restrictions(%" PRId64 ")", blobId);
                ret = HWC2_ERROR_UNSUPPORTED;
                break;
            }
            res = (struct drm_dpp_ch_restriction *)blob->data;
            set_dpp_ch_restriction(mDPUInfo.dpuInfo.dpp_ch[channelId], *res);
            drmModeFreePropertyBlob(blob);
        } else {
            ALOGI("plane[%d] There is no hw restriction information", channelId);
            ret = HWC2_ERROR_UNSUPPORTED;
            break;
        }
        /* Set supported format information */
        for (auto format : plane->formats()) {
            std::vector<uint32_t> halFormats;
            if (drmFormatToHalFormats(format, &halFormats) != NO_ERROR) {
                ALOGE("Fail to convert drm format(%d)", format);
                continue;
            }
            int &formatIndex = mDPUInfo.dpuInfo.dpp_ch[channelId].restriction.format_cnt;
            for (auto halFormat : halFormats) {
                mDPUInfo.dpuInfo.dpp_ch[channelId].restriction.format[formatIndex] =
                    halFormat;
                formatIndex++;
            }
        }
        if (hwcCheckDebugMessages(eDebugDefault))
            printDppRestriction(mDPUInfo.dpuInfo.dpp_ch[channelId]);

        channelId++;
    }

    if (ret != NO_ERROR) {
        ALOGI("Fail to get restriction (ret: %d)", ret);
        mUseQuery = false;
        return;
    }

    if ((ret = makeDPURestrictions()) != NO_ERROR) {
        ALOGE("makeDPURestrictions fail");
    } else if ((ret = updateFeatureTable()) != NO_ERROR) {
        ALOGE("updateFeatureTable fail");
    }

    if (ret == NO_ERROR)
        mUseQuery = true;
    else {
        ALOGI("There is no hw restriction information, use default values");
        mUseQuery = false;
    }
}

void ExynosDeviceDrmInterface::ExynosDrmEventHandler::init(ExynosDevice *exynosDevice)
{
    mExynosDevice = exynosDevice;
}

void ExynosDeviceDrmInterface::ExynosDrmEventHandler::HandleEvent(uint64_t timestamp_us)
{
    /* TODO: Check plug status hear or ExynosExternalDisplay::handleHotplugEvent() */
    ExynosExternalDisplayModule *display = static_cast<ExynosExternalDisplayModule*>(mExynosDevice->getDisplay(HWC_DISPLAY_EXTERNAL));
    if (display != NULL)
        display->handleHotplugEvent();
}
