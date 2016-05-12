/******************************************************************
*
* Copyright 2014 Samsung Electronics All Rights Reserved.
*
*
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
*
******************************************************************/

#include "caleinterface.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include <bluetooth.h>
#include <bluetooth_internal.h>
#include <bluetooth_type.h>

#include "camutex.h"
#include "caleadapter.h"
#include "caadapterutils.h"
#include "oic_string.h"
#include "oic_malloc.h"

/**
 * Logging tag for module name
 */
#define TAG "OIC_CA_LE_MONITOR"

#define MICROSECS_PER_SEC 1000000

static uint64_t const INITIALIZE_TIMEOUT = 1 * MICROSECS_PER_SEC;

static GMainLoop *g_mainloop = NULL;
static ca_thread_pool_t g_threadPoolHandle = NULL;

/**
 * Maintains the callback to be notified on device state changed.
 */
static CALEDeviceStateChangedCallback g_bleDeviceStateChangedCallback = NULL;

/**
 * Maintains the callback to be notified on device state changed.
 */
static CALEConnectionStateChangedCallback g_bleConnectionStateChangedCallback = NULL;

/**
 * Mutex to synchronize access to the deviceStateChanged Callback when the state
 *           of the LE adapter gets change.
 */
static ca_mutex g_bleDeviceStateChangedCbMutex = NULL;

/**
 * Mutex to synchronize access to the ConnectionStateChanged Callback when the state
 * of the LE adapter gets change.
 */
static ca_mutex g_bleConnectionStateChangedCbMutex = NULL;

/**
 * Mutex to synchronize access to
 */
static ca_mutex g_btinitializeMutex = NULL;

/**
 * Condition for gmainloop to run.
 */
static ca_cond g_initializeCond = NULL;

/**
 * Flag to check if BT stack is initialised.
 */
static bool g_isBTStackInitialised = false;

/**
* This is the callback which will be called when the adapter state gets changed.
*
* @param result         [IN] Result of the query done to the platform.
* @param adapter_state  [IN] State of the LE adapter.
* @param user_data      [IN] User data passed by the caller when querying for the state changed cb.
*
* @return  None.
*/
void CALEAdapterStateChangedCb(int result, bt_adapter_state_e adapter_state,
                               void *user_data);

/**
* This is the callback which will be called when the connection state gets changed.
*
* @param result         [IN] Result of the query done to the platform.
* @param connected      [IN] State of connection.
* @param remoteAddress  [IN] LE address of the device to be notified.
* @param user_data      [IN] User data passed by the caller when querying for the state changed cb.
*
* @return  None.
*/
void CALENWConnectionStateChangedCb(int result, bool connected,
                                    const char *remoteAddress, void *userData);

void CALEMainLoopThread(void *param)
{
    OIC_LOG(DEBUG, TAG, "IN");

    int ret = bt_initialize();
    if (BT_ERROR_NONE != ret)
    {
        OIC_LOG(ERROR, TAG, "bt_initialize failed");
        return;
    }

    bt_adapter_state_e adapterState = BT_ADAPTER_DISABLED;
    //Get Bluetooth adapter state
    ret = bt_adapter_get_state(&adapterState);

    if (BT_ERROR_NONE != ret && BT_ADAPTER_ENABLED == adapterState)
    {
        ret = bt_adapter_set_visibility(BT_ADAPTER_VISIBILITY_MODE_GENERAL_DISCOVERABLE, 0);
        if (BT_ERROR_NONE != ret)
        {
            OIC_LOG(ERROR, TAG, "bt_adapter_set_visibility failed");
            return;
        }
    }

    ret = bt_adapter_set_state_changed_cb(CALEAdapterStateChangedCb, NULL);
    if (BT_ERROR_NONE != ret)
    {
        OIC_LOG(DEBUG, TAG, "bt_adapter_set_state_changed_cb failed");
        return;
    }

    ret = bt_gatt_set_connection_state_changed_cb(CALENWConnectionStateChangedCb, NULL);
    if (BT_ERROR_NONE != ret)
    {
        OIC_LOG_V(ERROR, TAG,
                  "bt_gatt_set_connection_state_changed_cb has failed");
        return;
    }

    g_mainloop = g_main_loop_new(g_main_context_default(), FALSE);

    ca_mutex_lock(g_btinitializeMutex);
    g_isBTStackInitialised = true;
    ca_mutex_unlock(g_btinitializeMutex);
    ca_cond_signal(g_initializeCond);

    // Run gmainloop to handle the events from bt stack
    g_main_loop_run(g_mainloop);
    OIC_LOG(DEBUG, TAG, "OUT");
}

CAResult_t CAInitializeLENetworkMonitor()
{
    OIC_LOG(DEBUG, TAG, "IN");

    if (NULL == g_bleDeviceStateChangedCbMutex)
    {
        g_bleDeviceStateChangedCbMutex = ca_mutex_new();
        if (NULL == g_bleDeviceStateChangedCbMutex)
        {
            OIC_LOG(ERROR, TAG, "ca_mutex_new failed");
            return CA_STATUS_FAILED;
        }
    }

    if (NULL == g_bleConnectionStateChangedCbMutex)
    {
        g_bleConnectionStateChangedCbMutex = ca_mutex_new();
        if (NULL == g_bleConnectionStateChangedCbMutex)
        {
            OIC_LOG(ERROR, TAG, "ca_mutex_new failed");
            ca_mutex_free(g_bleDeviceStateChangedCbMutex);
            return CA_STATUS_FAILED;
        }
    }

    if (NULL == g_btinitializeMutex)
    {
        g_btinitializeMutex = ca_mutex_new();
        if (NULL == g_btinitializeMutex)
        {
            OIC_LOG(ERROR, TAG, "ca_mutex_new failed");
            ca_mutex_free(g_bleDeviceStateChangedCbMutex);
            ca_mutex_free(g_bleConnectionStateChangedCbMutex);
            return CA_STATUS_FAILED;
        }
    }

    if (NULL == g_initializeCond)
    {
        g_initializeCond = ca_cond_new();
        if (NULL == g_initializeCond)
        {
            OIC_LOG(ERROR, TAG, "ca_cond_new failed");
            ca_mutex_free(g_bleDeviceStateChangedCbMutex);
            ca_mutex_free(g_bleConnectionStateChangedCbMutex);
            ca_mutex_free(g_btinitializeMutex);
            return CA_STATUS_FAILED;
        }
    }
    OIC_LOG(DEBUG, TAG, "OUT");

    return CA_STATUS_OK;
}

void CATerminateLENetworkMonitor()
{
    OIC_LOG(DEBUG, TAG, "IN");

    ca_mutex_free(g_bleDeviceStateChangedCbMutex);
    g_bleDeviceStateChangedCbMutex = NULL;

    ca_mutex_free(g_bleConnectionStateChangedCbMutex);
    g_bleConnectionStateChangedCbMutex = NULL;

    ca_mutex_free(g_btinitializeMutex);
    g_btinitializeMutex = NULL;

    ca_cond_free(g_initializeCond);
    g_initializeCond = NULL;
    OIC_LOG(DEBUG, TAG, "OUT");
}

CAResult_t CAInitializeLEAdapter(const ca_thread_pool_t threadPool)
{
    OIC_LOG(DEBUG, TAG, "IN");
    g_threadPoolHandle = threadPool;
    OIC_LOG(DEBUG, TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CAStartLEAdapter()
{
    OIC_LOG(DEBUG, TAG, "IN");

    if (CA_STATUS_OK != ca_thread_pool_add_task(g_threadPoolHandle, CALEMainLoopThread, (void *) NULL))
    {
        OIC_LOG(ERROR, TAG, "Failed to create thread!");
        return CA_STATUS_FAILED;
    }

    OIC_LOG(DEBUG, TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CAStopLEAdapter()
{

    int ret = bt_adapter_unset_state_changed_cb();
    if (BT_ERROR_NONE != ret)
    {
        OIC_LOG(DEBUG, TAG, "bt_adapter_unset_state_changed_cb failed");
        return CA_STATUS_FAILED;
    }

    ret = bt_deinitialize();
    if (BT_ERROR_NONE != ret)
    {
        OIC_LOG(ERROR, TAG, "bt_deinitialize failed");
        return CA_STATUS_FAILED;
    }

    if (g_mainloop)
    {
        g_main_loop_quit(g_mainloop);
    }
    return CA_STATUS_OK;
}

CAResult_t CAGetLEAdapterState()
{
    OIC_LOG(DEBUG, TAG, "IN");

    ca_mutex_lock(g_btinitializeMutex);
    if (!g_isBTStackInitialised)
    {
        OIC_LOG(INFO, TAG, "Wait for BT initialization");
        CAWaitResult_t ret = ca_cond_wait_for(g_initializeCond, g_btinitializeMutex,
                                              INITIALIZE_TIMEOUT);
        if (CA_WAIT_TIMEDOUT == ret)
        {
            OIC_LOG(ERROR, TAG, "Timeout before BT initialize");
            ca_mutex_unlock(g_btinitializeMutex);
            return CA_STATUS_FAILED;
        }
    }
    ca_mutex_unlock(g_btinitializeMutex);

    bt_adapter_state_e adapterState = BT_ADAPTER_DISABLED;

    //Get Bluetooth adapter state
    int ret = bt_adapter_get_state(&adapterState);
    if (BT_ERROR_NONE != ret)
    {
        OIC_LOG_V(ERROR, TAG, "Bluetooth get state failed!, error num [%x]",
                  ret);
        return CA_STATUS_FAILED;
    }

    if (BT_ADAPTER_ENABLED != adapterState)
    {
        OIC_LOG(DEBUG, TAG, "BT Adapter is not enabled");
        return CA_ADAPTER_NOT_ENABLED;
    }

    OIC_LOG(DEBUG, TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CAGetLEAddress(char **local_address)
{
    OIC_LOG(DEBUG, TAG, "IN");

    VERIFY_NON_NULL(local_address, TAG, "local_address is null")

    char *address = NULL;

    int ret = bt_adapter_get_address(&address);
    if (BT_ERROR_NONE != ret || !address)
    {
        OIC_LOG_V(ERROR, TAG, "bt_adapter_get_address failed!, error num [%x]",
                  ret);
        return CA_STATUS_FAILED;
    }

    OIC_LOG_V(DEBUG, TAG, "bd address[%s]", address);

    *local_address = address;

    OIC_LOG(DEBUG, TAG, "OUT");

    return CA_STATUS_OK;
}

CAResult_t CASetLEAdapterStateChangedCb(CALEDeviceStateChangedCallback callback)
{
    OIC_LOG(DEBUG, TAG, "IN");
    ca_mutex_lock(g_bleDeviceStateChangedCbMutex);
    g_bleDeviceStateChangedCallback = callback;
    ca_mutex_unlock(g_bleDeviceStateChangedCbMutex);
    OIC_LOG(DEBUG, TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CAUnSetLEAdapterStateChangedCb()
{
    OIC_LOG(DEBUG, TAG, "IN");
    ca_mutex_lock(g_bleDeviceStateChangedCbMutex);
    g_bleDeviceStateChangedCallback = NULL;
    ca_mutex_unlock(g_bleDeviceStateChangedCbMutex);
    OIC_LOG(DEBUG, TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CASetLENWConnectionStateChangedCb(CALEConnectionStateChangedCallback callback)
{
    OIC_LOG(DEBUG, TAG, "IN");
    ca_mutex_lock(g_bleConnectionStateChangedCbMutex);
    g_bleConnectionStateChangedCallback = callback;
    ca_mutex_unlock(g_bleConnectionStateChangedCbMutex);
    OIC_LOG(DEBUG, TAG, "OUT");
    return CA_STATUS_OK;
}

CAResult_t CAUnsetLENWConnectionStateChangedCb()
{
    OIC_LOG(DEBUG, TAG, "IN");
    ca_mutex_lock(g_bleConnectionStateChangedCbMutex);
    g_bleConnectionStateChangedCallback = NULL;
    ca_mutex_unlock(g_bleConnectionStateChangedCbMutex);
    OIC_LOG(DEBUG, TAG, "OUT");
    return CA_STATUS_OK;
}

void CALEAdapterStateChangedCb(int result, bt_adapter_state_e adapter_state,
                                          void *user_data)
{
    OIC_LOG(DEBUG, TAG, "IN");

    ca_mutex_lock(g_bleDeviceStateChangedCbMutex);

    if (NULL == g_bleDeviceStateChangedCallback)
    {
        OIC_LOG(ERROR, TAG, "g_bleDeviceStateChangedCallback is NULL!");
        ca_mutex_unlock(g_bleDeviceStateChangedCbMutex);
        return;
    }

    if (BT_ADAPTER_DISABLED == adapter_state)
    {
        OIC_LOG(DEBUG, TAG, "Adapter is disabled");
        g_bleDeviceStateChangedCallback(CA_ADAPTER_DISABLED);
        ca_mutex_unlock(g_bleDeviceStateChangedCbMutex);
        return;
    }

    OIC_LOG(DEBUG, TAG, "Adapter is Enabled");

    int ret = bt_adapter_set_visibility(BT_ADAPTER_VISIBILITY_MODE_GENERAL_DISCOVERABLE, 0);
    if (BT_ERROR_NONE != ret)
    {
        OIC_LOG(ERROR, TAG, "bt_adapter_set_visibility failed");
        return;
    }

    g_bleDeviceStateChangedCallback(CA_ADAPTER_ENABLED);
    ca_mutex_unlock(g_bleDeviceStateChangedCbMutex);

    OIC_LOG(DEBUG, TAG, "OUT");
}

void CALENWConnectionStateChangedCb(int result, bool connected,
                                    const char *remoteAddress, void *userData)
{
    OIC_LOG(DEBUG, TAG, "IN");

    VERIFY_NON_NULL_VOID(remoteAddress, TAG, "remote address is NULL");

    ca_mutex_lock(g_bleConnectionStateChangedCbMutex);
    char *addr = OICStrdup(remoteAddress);
    if (NULL == addr)
    {
        OIC_LOG(ERROR, TAG, "addr is NULL");
        ca_mutex_unlock(g_bleConnectionStateChangedCbMutex);
        return;
    }
    g_bleConnectionStateChangedCallback(CA_ADAPTER_GATT_BTLE, addr, connected);
    OICFree(addr);
    ca_mutex_unlock(g_bleConnectionStateChangedCbMutex);

    OIC_LOG(DEBUG, TAG, "OUT");
}
