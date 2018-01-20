#include <stdlib.h>
#include <znp.h>
#include "core.h"
#include "zdp.h"
#include "zha.h"
#include "zll.h"
#include "action_list.h"
#include "aps.h"
#include "mt_af.h"
#include "mt_sys.h"
#include "mt_zdo.h"
#include "mt_util.h"
#include "ipc.h"
#include "device.h"
#include "keys.h"
#include "sm.h"

/********************************
 *     Constants and macros     *
 *******************************/

#define DEMO_DEVICE_ID              0
#define GATEWAY_ADDR                0x0000
#define GATEWAY_CHANNEL             11

/********************************
 * Initialization state machine *
 *******************************/

static ZgAl *_init_sm = NULL;
static uint8_t _initialized = 0;
static uint8_t _reset_network = 0;

/* Callback triggered when core initialization is complete */

static void _general_init_cb(void)
{
    if(zg_al_continue(_init_sm) != 0)
    {
        LOG_INF("Core application is initialized");
        _initialized = 1;
    }
}

static void _write_clear_flag(SyncActionCb cb)
{
    if(_reset_network)
        mt_sys_nv_write_startup_options(STARTUP_CLEAR_STATE|STARTUP_CLEAR_CONFIG,cb);
    else
        cb();
}

static void _write_channel(SyncActionCb cb)
{
    mt_sys_nv_write_channel(GATEWAY_CHANNEL, cb);
}

static void _announce_gateway(SyncActionCb cb)
{
    mt_zdo_device_annce(GATEWAY_ADDR, mt_sys_get_ext_addr(), cb);
}


static void _get_demo_device_route(SyncActionCb cb)
{
    mt_zdo_ext_route_disc_request(zg_device_get_short_addr(DEMO_DEVICE_ID), cb);
}


static ZgAlState _init_states_reset[] = {
    {_write_clear_flag, _general_init_cb},
    {mt_sys_reset_dongle, _general_init_cb},
    {mt_sys_nv_write_nwk_key, _general_init_cb},
    {mt_sys_reset_dongle, _general_init_cb},
    {mt_sys_check_ext_addr, _general_init_cb},
    {mt_sys_nv_write_coord_flag, _general_init_cb},
    {mt_sys_nv_set_pan_id, _general_init_cb},
    {_write_channel, _general_init_cb},
    {mt_sys_ping_dongle, _general_init_cb},
    {mt_util_af_subscribe_cmd, _general_init_cb},
    {zg_zll_init, _general_init_cb},
    {zg_zha_init, _general_init_cb},
    {zg_zdp_init, _general_init_cb},
    {mt_zdo_startup_from_app, _general_init_cb},
    {mt_sys_nv_write_enable_security, _general_init_cb},
    {_announce_gateway, _general_init_cb},
};

static int _init_reset_nb_states = sizeof(_init_states_reset)/sizeof(ZgAlState);

static ZgAlState _init_states_restart[] = {
    {mt_sys_reset_dongle, _general_init_cb},
    {mt_sys_check_ext_addr, _general_init_cb},
    {mt_sys_ping_dongle, _general_init_cb},
    {mt_util_af_subscribe_cmd, _general_init_cb},
    {zg_zll_init, _general_init_cb},
    {zg_zha_init, _general_init_cb},
    {zg_zdp_init, _general_init_cb},
    {mt_zdo_startup_from_app, _general_init_cb},
    {mt_sys_nv_write_enable_security, _general_init_cb},
    {_get_demo_device_route, _general_init_cb},
    {_announce_gateway, _general_init_cb}
};
static int _init_restart_nb_states = sizeof(_init_states_restart)/sizeof(ZgAlState);

/********************************
 *  New device learning SM      *
 *******************************/

/*** Local variables ***/
static uint16_t _current_learning_device_addr = 0xFFFD;
ZgSm *_new_device_sm = NULL;

enum
{
    STATE_INIT,
    STATE_ACTIVE_ENDPOINT_DISC,
    STATE_SIMPLE_DESC_DISC,
    STATE_SHUTDOWN,
};

enum
{
    EVENT_INIT_DONE,
    EVENT_ACTIVE_ENDPOINTS_RESP,
    EVENT_SIMPLE_DESC_RECEIVED,
    EVENT_ALL_SIMPLE_DESC_RECEIVED,
};

static void _init_new_device_sm(void)
{
    zg_sm_send_event(_new_device_sm, EVENT_INIT_DONE);
}

static void _query_active_endpoints(void)
{
    zg_zdp_query_active_endpoints(_current_learning_device_addr, NULL);
}

static void _query_simple_desc(void)
{
    uint8_t endpoint = zg_device_get_next_empty_endpoint(_current_learning_device_addr);
    zg_zdp_query_simple_descriptor(_current_learning_device_addr, endpoint, NULL);
}

static void _shutdown_new_device_sm(void)
{
    LOG_INF("Learnind device process finished");
    _current_learning_device_addr = 0xFFFD;
    zg_sm_destroy(_new_device_sm);
}

static ZgSmStateData _new_device_states[] = {
    {STATE_INIT, _init_new_device_sm},
    {STATE_ACTIVE_ENDPOINT_DISC, _query_active_endpoints},
    {STATE_SIMPLE_DESC_DISC, _query_simple_desc},
    {STATE_SHUTDOWN, _shutdown_new_device_sm}
};
static ZgSmStateNb _new_device_nb_states = sizeof(_new_device_states)/sizeof(ZgSmStateData);

static ZgSmTransitionData _new_device_transitions[] = {
    {STATE_INIT, EVENT_INIT_DONE, STATE_ACTIVE_ENDPOINT_DISC},
    {STATE_ACTIVE_ENDPOINT_DISC, EVENT_ACTIVE_ENDPOINTS_RESP, STATE_SIMPLE_DESC_DISC},
    {STATE_SIMPLE_DESC_DISC, EVENT_SIMPLE_DESC_RECEIVED, STATE_SIMPLE_DESC_DISC},
    {STATE_SIMPLE_DESC_DISC, EVENT_ALL_SIMPLE_DESC_RECEIVED, STATE_SHUTDOWN},
};
static ZgSmTransitionNb _new_device_nb_transitions = sizeof(_new_device_transitions)/sizeof(ZgSmTransitionData);

/********************************
 *  Network Events processing   *
 *******************************/

static void _new_device_cb(uint16_t short_addr, uint64_t ext_addr)
{
    if(!zg_device_is_device_known(ext_addr))
    {
        LOG_INF("Seen device is a new device");
        zg_add_device(short_addr, ext_addr);
        if(_new_device_sm)
        {
            LOG_WARN("Already learning a new device, cannot learn newly visible device");
            return;
        }
        _new_device_sm = zg_sm_create(  "New device",
                                        _new_device_states,
                                        _new_device_nb_states,
                                        _new_device_transitions,
                                        _new_device_nb_transitions);
        if(!_new_device_sm)
        {
            LOG_ERR("Abort new device learning");
            return;
        }
        LOG_INF("Start learning new device properties");
        _current_learning_device_addr = short_addr;
        zg_sm_start(_new_device_sm);
    }
    else
    {
        LOG_INF("Visible device is already learnt");
    }
}

static void _active_endpoints_cb(uint16_t short_addr, uint8_t nb_ep, uint8_t *ep_list)
{
    uint8_t index = 0;
    LOG_INF("Device 0x%04X has %d active endpoints :", short_addr, nb_ep);
    for(index = 0; index < nb_ep; index++)
    {
        LOG_INF("Active endpoint 0x%02X", ep_list[index]);
    }
    zg_device_update_endpoints(short_addr, nb_ep, ep_list);
    zg_sm_send_event(_new_device_sm, EVENT_ACTIVE_ENDPOINTS_RESP);
}

static void _simple_desc_cb(uint8_t endpoint, uint16_t profile)
{
    uint8_t next_endpoint;
    LOG_INF("Endpoint 0x%02X of device 0x%04X has profile 0x%04X",
            endpoint, _current_learning_device_addr, profile);
    zg_device_update_endpoint_profile(_current_learning_device_addr, endpoint, profile);
    
    next_endpoint = zg_device_get_next_empty_endpoint(_current_learning_device_addr);
    if(next_endpoint)
        zg_sm_send_event(_new_device_sm, EVENT_SIMPLE_DESC_RECEIVED);
    else
        zg_sm_send_event(_new_device_sm, EVENT_ALL_SIMPLE_DESC_RECEIVED);
}




/********************************
 *     Commands processing      *
 *******************************/

static void _process_command_touchlink()
{
    if(_initialized)
        zg_zll_start_touchlink();
    else
        LOG_WARN("Core application has not finished initializing, cannot start touchlink");
}

static void _process_command_switch_light()
{
    uint16_t addr = 0xFFFD;
    if(_initialized)
    {
        addr = zg_device_get_short_addr(DEMO_DEVICE_ID);
        if(addr != 0xFFFD)
            zg_zha_switch_bulb_state(addr);
        else
            LOG_WARN("Device is not installed, cannot switch light");
    }
    else
    {
        LOG_WARN("Core application has not finished initializing, cannot switch bulb state");
    }

}

static void _process_command_open_network(void)
{
    LOG_INF("Opening network to allow new devices to join");
    mt_zdo_permit_join(NULL);
}

static void _process_user_command(IpcCommand cmd)
{
    switch(cmd)
    {
        case ZG_IPC_COMMAND_TOUCHLINK:
            _process_command_touchlink();
            break;
        case ZG_IPC_COMMAND_SWITCH_DEMO_LIGHT:
            _process_command_switch_light();
            break;
        case ZG_IPC_COMMAND_OPEN_NETWORK:
            _process_command_open_network();
            break;
        default:
            LOG_WARN("Unsupported command");
            break;
    }
}


/********************************
 *             API              *
 *******************************/

void zg_core_init(uint8_t reset_network)
{
    LOG_INF("Initializing core application");
    _reset_network = reset_network;
    _reset_network |= !zg_keys_check_network_key_exists();
    mt_af_register_callbacks();
    mt_sys_register_callbacks();
    mt_zdo_register_callbacks();
    mt_util_register_callbacks();
    if(reset_network)
        zg_keys_network_key_del();
    zg_aps_init();
    zg_ipc_register_command_cb(_process_user_command);
    zg_zha_register_device_ind_callback(_new_device_cb);
    zg_zdp_register_active_endpoints_rsp(_active_endpoints_cb);
    zg_zdp_register_simple_desc_rsp(_simple_desc_cb);
    zg_device_init(reset_network);

    if(reset_network)
        _init_sm = zg_al_create(_init_states_reset, _init_reset_nb_states);
    else
        _init_sm = zg_al_create(_init_states_restart, _init_restart_nb_states);
    zg_al_continue(_init_sm);
}

void zg_core_shutdown(void)
{
    zg_device_shutdown();
    zg_zha_shutdown();
    zg_zll_shutdown();
    zg_aps_shutdown();
}

