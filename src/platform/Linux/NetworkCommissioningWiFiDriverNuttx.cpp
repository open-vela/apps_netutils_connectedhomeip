/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include <cstdint>
#include <cstring>
#include <limits>

#include <lib/core/CHIPError.h>
#include <lib/core/ErrorStr.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/Span.h>
#include <lib/support/logging/CHIPLogging.h>
#include <platform/CHIPDeviceBuildConfig.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KeyValueStoreManager.h>
#include <platform/NetworkCommissioning.h>

#include "NetworkCommissioningDriver.h"

#include <netutils/netlib.h>
#include <wireless/wapi.h>

namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI && defined(__NuttX__)
namespace {
constexpr char kWiFiSSIDKeyName[]        = "wifi-ssid";
constexpr char kWiFiCredentialsKeyName[] = "wifi-pass";
} // namespace

CHIP_ERROR NuttxWiFiDriver::Init(BaseDriver::NetworkStatusChangeCallback * networkStatusChangeCallback)
{
    CHIP_ERROR err;
    size_t ssidLen        = 0;
    size_t credentialsLen = 0;

    err = PersistedStorage::KeyValueStoreMgr().Get(kWiFiCredentialsKeyName, mSavedNetwork.credentials,
                                                   sizeof(mSavedNetwork.credentials), &credentialsLen);
    if (err == CHIP_ERROR_NOT_FOUND)
    {
        return CHIP_NO_ERROR;
    }

    err = PersistedStorage::KeyValueStoreMgr().Get(kWiFiSSIDKeyName, mSavedNetwork.ssid, sizeof(mSavedNetwork.ssid), &ssidLen);
    if (err == CHIP_ERROR_NOT_FOUND)
    {
        return CHIP_NO_ERROR;
    }
    static_assert(sizeof(mSavedNetwork.credentials) <= UINT8_MAX, "credentialsLen might not fit");
    mSavedNetwork.credentialsLen = static_cast<uint8_t>(credentialsLen);

    static_assert(sizeof(mSavedNetwork.ssid) <= UINT8_MAX, "ssidLen might not fit");
    mSavedNetwork.ssidLen = static_cast<uint8_t>(ssidLen);

    mStagingNetwork = mSavedNetwork;
    return err;
}

CHIP_ERROR NuttxWiFiDriver::CommitConfiguration()
{
    ReturnErrorOnFailure(PersistedStorage::KeyValueStoreMgr().Put(kWiFiSSIDKeyName, mStagingNetwork.ssid, mStagingNetwork.ssidLen));
    ReturnErrorOnFailure(PersistedStorage::KeyValueStoreMgr().Put(kWiFiCredentialsKeyName, mStagingNetwork.credentials,
                                                                  mStagingNetwork.credentialsLen));
    mSavedNetwork = mStagingNetwork;
    return CHIP_NO_ERROR;
}

CHIP_ERROR NuttxWiFiDriver::RevertConfiguration()
{
    mStagingNetwork = mSavedNetwork;
    return CHIP_NO_ERROR;
}

bool NuttxWiFiDriver::NetworkMatch(const WiFiNetwork & network, ByteSpan networkId)
{
    return networkId.size() == network.ssidLen && memcmp(networkId.data(), network.ssid, network.ssidLen) == 0;
}

Status NuttxWiFiDriver::AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan & outDebugText,
                                           uint8_t & outNetworkIndex)
{
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;
    VerifyOrReturnError(mStagingNetwork.ssidLen == 0 || NetworkMatch(mStagingNetwork, ssid), Status::kBoundsExceeded);

    static_assert(sizeof(WiFiNetwork::ssid) <= std::numeric_limits<decltype(WiFiNetwork::ssidLen)>::max(),
                  "Max length of WiFi ssid exceeds the limit of ssidLen field");
    static_assert(sizeof(WiFiNetwork::credentials) <= std::numeric_limits<decltype(WiFiNetwork::credentialsLen)>::max(),
                  "Max length of WiFi credentials exceeds the limit of credentialsLen field");

    VerifyOrReturnError(credentials.size() <= sizeof(mStagingNetwork.credentials), Status::kOutOfRange);
    VerifyOrReturnError(ssid.size() <= sizeof(mStagingNetwork.ssid), Status::kOutOfRange);

    memcpy(mStagingNetwork.credentials, credentials.data(), credentials.size());
    mStagingNetwork.credentialsLen = static_cast<decltype(mStagingNetwork.credentialsLen)>(credentials.size());

    memcpy(mStagingNetwork.ssid, ssid.data(), ssid.size());
    mStagingNetwork.ssidLen = static_cast<decltype(mStagingNetwork.ssidLen)>(ssid.size());

    return Status::kSuccess;
}

Status NuttxWiFiDriver::RemoveNetwork(ByteSpan networkId, MutableCharSpan & outDebugText, uint8_t & outNetworkIndex)
{
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;
    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);

    // Use empty ssid for representing invalid network
    mStagingNetwork.ssidLen = 0;
    return Status::kSuccess;
}

Status NuttxWiFiDriver::ReorderNetwork(ByteSpan networkId, uint8_t index, MutableCharSpan & outDebugText)
{
    outDebugText.reduce_size(0);
    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);
    // We only support one network, so reorder is actually no-op.

    return Status::kSuccess;
}

static CHIP_ERROR ConnectWiFiNetwork(uint8_t * ssid, uint8_t ssidLen, uint8_t * key, uint8_t keyLen)
{
    int ret                                                   = 0;
    char ssidArray[DeviceLayer::Internal::kMaxWiFiSSIDLength] = { 0 };
    char keyArray[DeviceLayer::Internal::kMaxWiFiKeyLength]   = { 0 };

    const char * wifi_name = ConnectivityMgrImpl().GetWiFiIfName();
    VerifyOrExit(wifi_name != nullptr, ChipLogError(DeviceLayer, "Failed to got wifi interface name"));

    VerifyOrExit(ssid != nullptr && ssidLen > 0, ChipLogError(DeviceLayer, "Connect wifi network, SSID is NULL"));
    VerifyOrExit(key != nullptr, ChipLogError(DeviceLayer, "Connect wifi network, KEY is NULL"));

    VerifyOrExit(netlib_ifup(wifi_name) >= 0, ChipLogError(DeviceLayer, "Failed to up interface, name: %s", wifi_name));

    memcpy(ssidArray, ssid, ssidLen);
    memcpy(keyArray, key, keyLen);

    struct wpa_wconfig_s conf;
    conf.ifname      = wifi_name;
    conf.sta_mode    = WAPI_MODE_MANAGED;
    conf.auth_wpa    = IW_AUTH_WPA_VERSION_WPA2;
    conf.cipher_mode = IW_AUTH_CIPHER_CCMP;
    conf.ssid        = ssidArray;
    conf.passphrase  = keyArray;
    conf.ssidlen     = ssidLen;
    conf.phraselen   = keyLen;
    conf.bssid       = NULL;
    conf.alg         = ((keyLen == 0) ? WPA_ALG_NONE : WPA_ALG_CCMP);

    ret = wpa_driver_wext_associate(&conf);
    VerifyOrExit(ret >= 0, ChipLogError(DeviceLayer, "Failed to connect to wifi network, ret: %d", ret));

    ret = netlib_obtain_ipv6addr(wifi_name);
    if (ret)
    {
        ChipLogError(DeviceLayer, "DHCPv6 failed to obtain address, ret: %d", ret);
    }

    return CHIP_NO_ERROR;

exit:
    return CHIP_ERROR_INTERNAL;
}

void NuttxWiFiDriver::ConnectNetwork(ByteSpan networkId, ConnectCallback * callback)
{
    CHIP_ERROR err          = CHIP_NO_ERROR;
    Status networkingStatus = Status::kSuccess;

    VerifyOrExit(NetworkMatch(mStagingNetwork, networkId), networkingStatus = Status::kNetworkIDNotFound);

    ChipLogProgress(NetworkProvisioning, "NetworkCommissioningDelegate: SSID: %.*s", static_cast<int>(sizeof(mStagingNetwork.ssid)),
                    reinterpret_cast<char *>(mStagingNetwork.ssid));

    err = ConnectWiFiNetwork(mStagingNetwork.ssid, mStagingNetwork.ssidLen, mStagingNetwork.credentials,
                             mStagingNetwork.credentialsLen);

    if (err == CHIP_NO_ERROR)
    {
        ChipDeviceEvent event;
        event.Type                          = DeviceEventType::kWiFiConnectivityChange;
        event.WiFiConnectivityChange.Result = kConnectivity_Established;
        PlatformMgr().PostEventOrDie(&event);
    }

exit:
    if (err != CHIP_NO_ERROR)
    {
        networkingStatus = Status::kUnknownError;
    }

    if (callback)
    {
        ChipLogError(NetworkProvisioning, "Connect to WiFi network: %s", chip::ErrorStr(err));
        callback->OnResult(networkingStatus, CharSpan(), 0);
    }
}

static CHIP_ERROR StartWiFiScan(ByteSpan ssid, NetworkCommissioning::WiFiDriver::ScanCallback * callback)
{
    return CHIP_NO_ERROR;
}

void NuttxWiFiDriver::ScanNetworks(ByteSpan ssid, WiFiDriver::ScanCallback * callback)
{
    CHIP_ERROR err = StartWiFiScan(ssid, callback);
    if (err != CHIP_NO_ERROR)
    {
        callback->OnFinished(Status::kUnknownError, CharSpan(), nullptr);
    }
}

size_t NuttxWiFiDriver::WiFiNetworkIterator::Count()
{
    return driver->mStagingNetwork.ssidLen == 0 ? 0 : 1;
}

static CHIP_ERROR GetConfiguredNetwork(NetworkCommissioning::Network & network)
{
    return CHIP_NO_ERROR;
}

bool NuttxWiFiDriver::WiFiNetworkIterator::Next(Network & item)
{
    if (exhausted || driver->mStagingNetwork.ssidLen == 0)
    {
        return false;
    }
    memcpy(item.networkID, driver->mStagingNetwork.ssid, driver->mStagingNetwork.ssidLen);
    item.networkIDLen = driver->mStagingNetwork.ssidLen;
    item.connected    = false;
    exhausted         = true;

    Network configuredNetwork;
    CHIP_ERROR err = GetConfiguredNetwork(configuredNetwork);
    if (err == CHIP_NO_ERROR)
    {
        if (configuredNetwork.networkIDLen == item.networkIDLen &&
            memcmp(configuredNetwork.networkID, item.networkID, item.networkIDLen) == 0)
        {
            item.connected = true;
        }
    }

    return true;
}
#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI && defined(__NuttX__)

} // namespace NetworkCommissioning
} // namespace DeviceLayer
} // namespace chip
