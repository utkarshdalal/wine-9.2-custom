/*  DirectInput Gamepad device
 *
 * Copyright 2024 BrunoSX
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winternl.h"
#include "winuser.h"
#include "winerror.h"
#include "winreg.h"
#include "dinput.h"
#include "winsock2.h"
#include "devguid.h"
#include "hidusage.h"

#include "dinput_private.h"
#include "device_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(dinput);

#define SERVER_PORT 7948
#define CLIENT_PORT 7947
#define BUFFER_SIZE 64

#define REQUEST_CODE_GET_GAMEPAD 8
#define REQUEST_CODE_GET_GAMEPAD_STATE 9
#define REQUEST_CODE_RELEASE_GAMEPAD 10

#define MAPPER_TYPE_STANDARD 0
#define MAPPER_TYPE_XINPUT 1

#define IDX_BUTTON_A 0
#define IDX_BUTTON_B 1
#define IDX_BUTTON_X 2
#define IDX_BUTTON_Y 3
#define IDX_BUTTON_L1 4
#define IDX_BUTTON_R1 5
#define IDX_BUTTON_L2 10
#define IDX_BUTTON_R2 11
#define IDX_BUTTON_SELECT 6
#define IDX_BUTTON_START 7
#define IDX_BUTTON_L3 8
#define IDX_BUTTON_R3 9

struct gamepad_state 
{
    short buttons;
    char dpad;
    short thumb_lx;
    short thumb_ly;
    short thumb_rx;
    short thumb_ry;    
};

struct gamepad
{
    struct dinput_device base;
    struct gamepad_state state;
    char* name;
    int id;
    char mapper_type;
};

static struct gamepad gamepad = 
{
    .name = NULL,
    .id = 0,
    .mapper_type = MAPPER_TYPE_XINPUT
};

static const struct dinput_device_vtbl gamepad_vtbl;
static SOCKET server_sock = INVALID_SOCKET;
static BOOL winsock_loaded = FALSE;

static void init_gamepad( void ) {
    gamepad.id = 0;
    if (gamepad.name)
    {
        free( gamepad.name );
        gamepad.name = NULL;
    }
    memset( &gamepad.state, 0, sizeof(gamepad.state) );
    gamepad.mapper_type = MAPPER_TYPE_XINPUT;
}

static void close_server_socket( void ) 
{
    if (server_sock != INVALID_SOCKET) 
    {
        closesocket( server_sock );
        server_sock = INVALID_SOCKET;
    }
    
    if (winsock_loaded) 
    {
        WSACleanup();
        winsock_loaded = FALSE;
    }    
}

static BOOL create_server_socket( void )
{    
    WSADATA wsa_data;
    struct sockaddr_in server_addr;
    DWORD timeout;
    int res;
    
    close_server_socket();
    
    winsock_loaded = WSAStartup( MAKEWORD(2,2), &wsa_data ) == NO_ERROR;
    if (!winsock_loaded) return FALSE;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    server_addr.sin_port = htons( SERVER_PORT );
    
    server_sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if (server_sock == INVALID_SOCKET) return FALSE;
    
    timeout = 2000;    
    res = setsockopt( server_sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout) );
    if (res < 0) return FALSE;    

    res = bind( server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr) );
    if (res == SOCKET_ERROR) return FALSE;
    
    return TRUE;
}

static BOOL get_gamepad_request( BOOL notify ) 
{
    int res, gamepad_id, name_len;
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    DWORD size;
    char *gamepad_name;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    client_addr.sin_port = htons( CLIENT_PORT );
    
    buffer[0] = REQUEST_CODE_GET_GAMEPAD;
    buffer[1] = 0;
    buffer[2] = notify ? 1 : 0;
    res = sendto( server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, sizeof(client_addr) );
    if (res == SOCKET_ERROR) return FALSE;
    
    res = recvfrom( server_sock, buffer, BUFFER_SIZE, 0, NULL, 0 );
    if (res == SOCKET_ERROR || buffer[0] != REQUEST_CODE_GET_GAMEPAD) return FALSE;
    
    init_gamepad();
    gamepad_id = *(int*)(buffer + 1);
    if (gamepad_id == 0) return FALSE;
    
    gamepad.id = gamepad_id;
    gamepad.mapper_type = buffer[5];
    
    name_len = *(int*)(buffer + 6);
    
    gamepad_name = calloc( 1, name_len + 1 );
    gamepad_name[name_len] = '\0';
    memcpy( gamepad_name, buffer + 10, name_len );
    gamepad.name = gamepad_name;
    
    return TRUE;
}

static LONG scale_value( LONG value, struct object_properties *properties )
{
    LONG log_min, log_max, phy_min, phy_max;
    log_min = properties->logical_min;
    log_max = properties->logical_max;
    phy_min = properties->range_min;
    phy_max = properties->range_max;

    return phy_min + MulDiv( value - log_min, phy_max - phy_min, log_max - log_min );
}

static LONG scale_axis_value( LONG value, struct object_properties *properties )
{
    LONG log_ctr, log_min, log_max, phy_ctr, phy_min, phy_max;
    log_min = properties->logical_min;
    log_max = properties->logical_max;
    phy_min = properties->range_min;
    phy_max = properties->range_max;

    if (phy_min == 0) phy_ctr = phy_max >> 1;
    else phy_ctr = round( (phy_min + phy_max) / 2.0 );
    if (log_min == 0) log_ctr = log_max >> 1;
    else log_ctr = round( (log_min + log_max) / 2.0 );

    value -= log_ctr;
    if (value <= 0)
    {
        log_max = MulDiv( log_min - log_ctr, properties->deadzone, 10000 );
        log_min = MulDiv( log_min - log_ctr, properties->saturation, 10000 );
        phy_max = phy_ctr;
    }
    else
    {
        log_min = MulDiv( log_max - log_ctr, properties->deadzone, 10000 );
        log_max = MulDiv( log_max - log_ctr, properties->saturation, 10000 );
        phy_min = phy_ctr;
    }

    if (value <= log_min) return phy_min;
    if (value >= log_max) return phy_max;
    return phy_min + MulDiv( value - log_min, phy_max - phy_min, log_max - log_min );
}

static void handle_gamepad_input( IDirectInputDevice8W *iface, short thumb_lx, short thumb_ly, short thumb_rx, short thumb_ry, short buttons, char dpad ) 
{
    int i, j;
    DWORD time, seq;
    BOOL notify = FALSE;
    DIJOYSTATE *state = (DIJOYSTATE *)gamepad.base.device_state;
    
    time = GetCurrentTime();
    seq = gamepad.base.dinput->evsequence++;    
    
    if (gamepad.mapper_type == MAPPER_TYPE_STANDARD) 
    {
        if (thumb_lx != gamepad.state.thumb_lx)
        {
            gamepad.state.thumb_lx = thumb_lx;
            state->lX = scale_axis_value(thumb_lx, &gamepad.base.object_properties[0]);
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ), state->lX, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ly != gamepad.state.thumb_ly) 
        {
            gamepad.state.thumb_ly = thumb_ly;
            state->lY = scale_axis_value(thumb_ly, &gamepad.base.object_properties[1]);        
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ), state->lY, time, seq );
            notify = TRUE;
        }
        
        if (thumb_rx != gamepad.state.thumb_rx) 
        {
            gamepad.state.thumb_rx = thumb_rx;
            state->lZ = scale_axis_value(thumb_rx, &gamepad.base.object_properties[2]);
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ), state->lZ, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ry != gamepad.state.thumb_ry) 
        {
            gamepad.state.thumb_ry = thumb_ry;
            state->lRz = scale_axis_value(thumb_ry, &gamepad.base.object_properties[5]);
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 ), state->lRz, time, seq );
            notify = TRUE;
        }
        
        if (buttons != gamepad.state.buttons) 
        {
            gamepad.state.buttons = buttons;
            for (i = 0, j = 0; i < 12; i++)
            {
                switch (i)
                {
                case IDX_BUTTON_A: j = 1; break;
                case IDX_BUTTON_B: j = 2; break;
                case IDX_BUTTON_X: j = 0; break;
                case IDX_BUTTON_Y: j = 3; break;
                case IDX_BUTTON_L1: j = 4; break;
                case IDX_BUTTON_R1: j = 5; break;
                case IDX_BUTTON_L2: j = 6; break;
                case IDX_BUTTON_R2: j = 7; break;
                case IDX_BUTTON_SELECT: j = 8; break;
                case IDX_BUTTON_START: j = 9; break;
                case IDX_BUTTON_L3: j = 10; break;
                case IDX_BUTTON_R3: j = 11; break;                
                }
                
                state->rgbButtons[j] = (buttons & (1<<i)) ? 0x80 : 0x00;
                queue_event( iface, DIDFT_BUTTON | DIDFT_MAKEINSTANCE( j ), state->rgbButtons[j], time, seq );    
            }
            notify = TRUE;        
        }
        
        if (dpad != gamepad.state.dpad) 
        {
            gamepad.state.dpad = dpad;
            state->rgdwPOV[0] = dpad != -1 ? dpad * 4500 : -1;
            queue_event( iface, DIDFT_POV | DIDFT_MAKEINSTANCE( 0 ), state->rgdwPOV[0], time, seq );
            notify = TRUE;
        }        
    }
    else if (gamepad.mapper_type == MAPPER_TYPE_XINPUT) 
    {
        if (thumb_lx != gamepad.state.thumb_lx) 
        {
            gamepad.state.thumb_lx = thumb_lx;
            state->lX = scale_axis_value(thumb_lx, &gamepad.base.object_properties[0]);
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ), state->lX, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ly != gamepad.state.thumb_ly) 
        {
            gamepad.state.thumb_ly = thumb_ly;
            state->lY = scale_axis_value(thumb_ly, &gamepad.base.object_properties[1]);        
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ), state->lY, time, seq );
            notify = TRUE;
        }
        
        if (thumb_rx != gamepad.state.thumb_rx) 
        {
            gamepad.state.thumb_rx = thumb_rx;
            state->lRx = scale_axis_value(thumb_rx, &gamepad.base.object_properties[3]);
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ), state->lRx, time, seq );
            notify = TRUE;
        }
        
        if (thumb_ry != gamepad.state.thumb_ry) 
        {
            gamepad.state.thumb_ry = thumb_ry;
            state->lRy = scale_axis_value(thumb_ry, &gamepad.base.object_properties[4]);
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 4 ), state->lRy, time, seq );
            notify = TRUE;
        }
        
        if (buttons != gamepad.state.buttons) 
        {
            gamepad.state.buttons = buttons;
            for (i = 0; i < 10; i++)
            {
                state->rgbButtons[i] = (buttons & (1<<i)) ? 0x80 : 0x00;
                queue_event( iface, DIDFT_BUTTON | DIDFT_MAKEINSTANCE( i ), state->rgbButtons[i], time, seq );    
            }
            
            state->lZ = scale_value((buttons & (1<<10)) ? 32767 : ((buttons & (1<<11)) ? -32768 : 0), &gamepad.base.object_properties[2]);
            queue_event( iface, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ), state->lZ, time, seq );
            notify = TRUE;        
        }
        
        if (dpad != gamepad.state.dpad) 
        {
            gamepad.state.dpad = dpad;
            state->rgdwPOV[0] = dpad != -1 ? dpad * 4500 : -1;
            queue_event( iface, DIDFT_POV | DIDFT_MAKEINSTANCE( 0 ), state->rgdwPOV[0], time, seq );
            notify = TRUE;
        }        
    }
    
    if (notify && gamepad.base.hEvent) SetEvent( gamepad.base.hEvent );
}

static void release_gamepad_request( void ) 
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    int client_addr_len;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    client_addr.sin_port = htons( CLIENT_PORT );
    client_addr_len = sizeof(client_addr);
    
    buffer[0] = REQUEST_CODE_RELEASE_GAMEPAD;
    sendto( server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, client_addr_len );
}

HRESULT gamepad_enum_device( DWORD type, DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version )
{   
    DWORD size;

    TRACE( "type %#lx, flags %#lx, instance %p, version %#lx.\n", type, flags, instance, version );
    
    if (!create_server_socket() || !get_gamepad_request( FALSE )) return DIERR_INPUTLOST;
    
    size = instance->dwSize;
    memset( instance, 0, size );
    instance->dwSize = size;
    instance->guidInstance = GUID_Joystick;
    instance->guidProduct = GUID_Joystick;
    instance->guidProduct.Data1 = MAKELONG( 0x045e, 0x028e );
    if (version >= 0x0800) instance->dwDevType = DIDEVTYPE_HID | DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
    else instance->dwDevType = DIDEVTYPE_HID | DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8);
    instance->wUsagePage = HID_USAGE_PAGE_GENERIC;
    instance->wUsage = HID_USAGE_GENERIC_GAMEPAD;
    MultiByteToWideChar( CP_ACP, 0, gamepad.name, -1, instance->tszInstanceName, MAX_PATH );
    MultiByteToWideChar( CP_ACP, 0, gamepad.name, -1, instance->tszProductName, MAX_PATH );
    
    return DI_OK;
}

static BOOL init_object_properties( struct dinput_device *device, UINT index, struct hid_value_caps *caps,
                                    const DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    struct object_properties *properties;
    
    if (index == -1) return DIENUM_STOP;
    properties = device->object_properties + index;

    properties->logical_min = -32768;
    properties->logical_max = 32767;
    properties->range_min = 0;
    properties->range_max = 65535;
    properties->saturation = 10000;
    properties->granularity = 1;

    return DIENUM_CONTINUE;
}

static void gamepad_release( IDirectInputDevice8W *iface )
{
    init_gamepad();
    CloseHandle( gamepad.base.read_event );
}

static HRESULT gamepad_read( IDirectInputDevice8W *iface )
{   
    int res;
    char buffer[BUFFER_SIZE];
    
    if (server_sock == INVALID_SOCKET) return DI_OK;
    res = recvfrom( server_sock, buffer, BUFFER_SIZE, 0, NULL, 0 );
    if (res == SOCKET_ERROR) return DI_OK;
    
    if (buffer[0] == REQUEST_CODE_GET_GAMEPAD_STATE && buffer[1] == 1) 
    {
        int gamepad_id;
        char dpad;
        short buttons, thumb_lx, thumb_ly, thumb_rx, thumb_ry;        
        
        gamepad_id = *(int*)(buffer + 2);
        if (gamepad_id != gamepad.id) return DI_OK;
    
        buttons = *(short*)(buffer + 6);
        dpad = buffer[8];
    
        thumb_lx = *(short*)(buffer + 9);
        thumb_ly = *(short*)(buffer + 11);
        thumb_rx = *(short*)(buffer + 13);
        thumb_ry = *(short*)(buffer + 15);
    
        handle_gamepad_input( iface, thumb_lx, thumb_ly, thumb_rx, thumb_ry, buttons, dpad );
    }
    return DI_OK;
}

static HRESULT gamepad_acquire( IDirectInputDevice8W *iface )
{
    get_gamepad_request( TRUE );
    SetEvent( gamepad.base.read_event );
    return DI_OK;
}

static HRESULT gamepad_unacquire( IDirectInputDevice8W *iface )
{
    WaitForSingleObject( gamepad.base.read_event, INFINITE );
    
    init_gamepad();
    release_gamepad_request();
    close_server_socket();
    return DI_OK;
}

static BOOL try_enum_object( struct dinput_device *impl, const DIPROPHEADER *filter, DWORD flags, enum_object_callback callback,
                             UINT index, DIDEVICEOBJECTINSTANCEW *instance, void *data )
{
    if (flags != DIDFT_ALL && !(flags & DIDFT_GETTYPE( instance->dwType ))) return DIENUM_CONTINUE;

    switch (filter->dwHow)
    {
    case DIPH_DEVICE:
        return callback( impl, index, NULL, instance, data );
    case DIPH_BYOFFSET:
        if (filter->dwObj != instance->dwOfs) return DIENUM_CONTINUE;
        return callback( impl, index, NULL, instance, data );
    case DIPH_BYID:
        if ((filter->dwObj & 0x00ffffff) != (instance->dwType & 0x00ffffff)) return DIENUM_CONTINUE;
        return callback( impl, index, NULL, instance, data );
    }

    return DIENUM_CONTINUE;
}

static void get_device_objects( int *instance_count, DIDEVICEOBJECTINSTANCEW **out ) 
{
    int i, index = 0;
    
    *instance_count = 0;
    *out = NULL;

    if (gamepad.mapper_type == MAPPER_TYPE_STANDARD) 
    {
        *instance_count = 17;
        DIDEVICEOBJECTINSTANCEW instances[*instance_count];
        
        instances[index].guidType = GUID_XAxis;
        instances[index].dwOfs = DIJOFS_X;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"X Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_X;
        index++;
        
        instances[index].guidType = GUID_YAxis;
        instances[index].dwOfs = DIJOFS_Y;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"Y Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Y;    
        index++;
        
        instances[index].guidType = GUID_ZAxis;
        instances[index].dwOfs = DIJOFS_Z;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"Z Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Z;    
        index++;    

        instances[index].guidType = GUID_RzAxis;
        instances[index].dwOfs = DIJOFS_RZ;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"Rz Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_RZ;    
        index++;
        
        for (i = 0; i < 12; i++) 
        {
            instances[index].guidType = GUID_Button,
            instances[index].dwOfs = DIJOFS_BUTTON( i ),
            instances[index].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE( i ),
            swprintf( instances[index].tszName, MAX_PATH, L"Button %d", i );
            instances[index].wUsagePage = HID_USAGE_PAGE_BUTTON;
            instances[index].wUsage = i + 1;
            index++;
        }
        
        instances[index].guidType = GUID_POV;
        instances[index].dwOfs = DIJOFS_POV( 0 );
        instances[index].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE( 0 );
        swprintf( instances[index].tszName, MAX_PATH, L"POV" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_HATSWITCH;
        
        *out = instances;
    }
    else if (gamepad.mapper_type == MAPPER_TYPE_XINPUT) 
    {
        *instance_count = 16;
        DIDEVICEOBJECTINSTANCEW instances[*instance_count];
        
        instances[index].guidType = GUID_XAxis;
        instances[index].dwOfs = DIJOFS_X;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"X Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_X;
        index++;
        
        instances[index].guidType = GUID_YAxis;
        instances[index].dwOfs = DIJOFS_Y;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"Y Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Y;
        index++;
        
        instances[index].guidType = GUID_ZAxis;
        instances[index].dwOfs = DIJOFS_Z;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"Z Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_Z;
        index++;    

        instances[index].guidType = GUID_RxAxis;
        instances[index].dwOfs = DIJOFS_RX;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"Rx Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_RX;
        index++;

        instances[index].guidType = GUID_RyAxis;
        instances[index].dwOfs = DIJOFS_RY;
        instances[index].dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 4 );
        instances[index].dwFlags = DIDOI_ASPECTPOSITION;
        swprintf( instances[index].tszName, MAX_PATH, L"Ry Axis" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_RY;    
        index++;
        
        for (i = 0; i < 10; i++) 
        {
            instances[index].guidType = GUID_Button,
            instances[index].dwOfs = DIJOFS_BUTTON( i ),
            instances[index].dwType = DIDFT_BUTTON | DIDFT_MAKEINSTANCE( i ),
            swprintf( instances[index].tszName, MAX_PATH, L"Button %d", i );
            instances[index].wUsagePage = HID_USAGE_PAGE_BUTTON;
            instances[index].wUsage = i + 1;
            index++;
        }
        
        instances[index].guidType = GUID_POV;
        instances[index].dwOfs = DIJOFS_POV( 0 );
        instances[index].dwType = DIDFT_POV | DIDFT_MAKEINSTANCE( 0 );
        swprintf( instances[index].tszName, MAX_PATH, L"POV" );
        instances[index].wUsagePage = HID_USAGE_PAGE_GENERIC;
        instances[index].wUsage = HID_USAGE_GENERIC_HATSWITCH;
        
        *out = instances;
    }
}

static HRESULT gamepad_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                     DWORD flags, enum_object_callback callback, void *context )
{    
    int instance_count;
    DIDEVICEOBJECTINSTANCEW* instances;
    BOOL ret;
    DWORD i;
    
    get_device_objects( &instance_count, &instances );

    for (i = 0; i < instance_count; i++)
    {
        instances[i].dwSize = sizeof(DIDEVICEOBJECTINSTANCEW);
        instances[i].wReportId = 1;
        
        ret = try_enum_object( &gamepad.base, filter, flags, callback, i, instances + i, context );
        if (ret != DIENUM_CONTINUE) return DIENUM_STOP;
    }

    return DIENUM_CONTINUE;
}

static HRESULT gamepad_get_property( IDirectInputDevice8W *iface, DWORD property,
                                     DIPROPHEADER *header, const DIDEVICEOBJECTINSTANCEW *instance )
{
    switch (property)
    {
    case (DWORD_PTR)DIPROP_PRODUCTNAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, gamepad.base.instance.tszProductName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_INSTANCENAME:
    {
        DIPROPSTRING *value = (DIPROPSTRING *)header;
        lstrcpynW( value->wsz, gamepad.base.instance.tszInstanceName, MAX_PATH );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_VIDPID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = MAKELONG( 0x045e, 0x028e );
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_JOYSTICKID:
    {
        DIPROPDWORD *value = (DIPROPDWORD *)header;
        value->dwData = gamepad.id;
        return DI_OK;
    }
    case (DWORD_PTR)DIPROP_GUIDANDPATH:
    {
        DIPROPGUIDANDPATH *value = (DIPROPGUIDANDPATH *)header;
        value->guidClass = GUID_DEVCLASS_HIDCLASS;
        lstrcpynW( value->wszPath, L"virtual#vid_045e&pid_028e&ig_00", MAX_PATH );
        return DI_OK;
    }
    }

    return DIERR_UNSUPPORTED;
}

HRESULT gamepad_create_device( struct dinput *dinput, const GUID *guid, IDirectInputDevice8W **out )
{
    static const DIPROPHEADER filter =
    {
        .dwSize = sizeof(filter),
        .dwHeaderSize = sizeof(filter),
        .dwHow = DIPH_DEVICE,
    };    
    HRESULT hr;
    
    TRACE( "dinput %p, guid %s, out %p.\n", dinput, debugstr_guid( guid ), out );

    *out = NULL;
    if (!IsEqualGUID( &GUID_Joystick, guid )) return DIERR_DEVICENOTREG;

    dinput_device_init( &gamepad.base, &gamepad_vtbl, guid, dinput );
    gamepad.base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": struct gamepad*->base.crit");
    gamepad.base.read_event = CreateEventW( NULL, TRUE, FALSE, NULL );

    gamepad_enum_device( 0, 0, &gamepad.base.instance, dinput->dwVersion );
    gamepad.base.caps.dwDevType = gamepad.base.instance.dwDevType;
    gamepad.base.caps.dwFirmwareRevision = 100;
    gamepad.base.caps.dwHardwareRevision = 100;
    gamepad.base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    
    if (FAILED(hr = dinput_device_init_device_format( &gamepad.base.IDirectInputDevice8W_iface ))) goto failed;
    gamepad_enum_objects( &gamepad.base.IDirectInputDevice8W_iface, &filter, DIDFT_ABSAXIS, init_object_properties, NULL );

    *out = &gamepad.base.IDirectInputDevice8W_iface;
    return DI_OK;
    
failed:
    IDirectInputDevice_Release( &gamepad.base.IDirectInputDevice8W_iface );
    return hr;    
}

static const struct dinput_device_vtbl gamepad_vtbl =
{
    gamepad_release,
    NULL,
    gamepad_read,
    gamepad_acquire,
    gamepad_unacquire,
    gamepad_enum_objects,
    gamepad_get_property,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};