#include "app.h"
#include "mt_af.h"
#include "mt_sys.h"
#include "mt_zdo.h"
#include "dbgPrint.h"
#include <stdlib.h>

uv_async_t state_flag;

void app_register_callbacks()
{
    mt_af_register_callbacks();
    mt_sys_register_callbacks();
    mt_zdo_register_callbacks();
}

void state_machine_cb(uv_async_t *state_data)
{
    if (!state_data)
    {
        LOG_CRI("Cannot read current app state : state variable is empty");
        return;
    }
    AppState current_state = *((AppState *)state_data->data);
    LOG_DBG("State machine has been woken up");
    switch(current_state)
    {
        case APP_STATE_INIT:
            mt_sys_nv_write_clear_flag();
            break;
        case APP_STATE_NV_CLEAR_FLAG_WRITTEN:
            mt_sys_reset_dongle();
            break;
        case APP_STATE_DONGLE_UP:
            mt_sys_nv_write_coord_flag();
            break;
        case APP_STATE_NV_COORD_FLAG_WRITTEN:
            mt_sys_ping_dongle();
            break;
        case APP_STATE_DONGLE_PRESENT:
            mt_af_register_zll_endpoint();
            break;
        case APP_STATE_ZLL_REGISTERED:
            mt_af_send_zll_scan_request();
            break;
        case APP_STATE_ZLL_SCAN_REQUEST_SENT:
            LOG_DBG("End of state machine");
            break;
        default:
            LOG_CRI("Received state is unknown (%02d)", current_state);
            break;
    }
}

void run_state_machine()
{
    AppState *state = calloc(1, sizeof(AppState));
    if(state)
    {
        *state = APP_STATE_INIT;
        state_flag.data = state;
    }
    uv_async_send(&state_flag);
}

