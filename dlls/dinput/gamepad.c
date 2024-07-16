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
    CRITICAL_SECTION crit;
    struct dinput_device base;
    struct gamepad_state state;
    BOOL connected;
    BOOL changed;
    BOOL acquired;
    char *name;
    int id;
    char mapper_type;
};

static struct gamepad gamepad;
static CRITICAL_SECTION_DEBUG gamepad_critsect_debug = 
{
    0, 0, &gamepad.crit,
    { &gamepad_critsect_debug.ProcessLocksList, &gamepad_critsect_debug.ProcessLocksList },
    0, 0, { (DWORD_PTR)(__FILE__ ": gamepad.crit") }
};

static struct gamepad gamepad = 
{
    .crit = { &gamepad_critsect_debug, -1, 0, 0, 0, 0 },
    .connected = FALSE,
    .changed = FALSE,
    .acquired = FALSE,
    .name = NULL,
    .id = 0,
    .mapper_type = MAPPER_TYPE_XINPUT
};

static const struct dinput_device_vtbl gamepad_vtbl;
static SOCKET server_sock = INVALID_SOCKET;
static BOOL winsock_loaded = FALSE;

static HANDLE start_event;
static BOOL thread_running = FALSE;
static HANDLE read_thread;

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
    const UINT reuse_addr = 1;
    ULONG non_blocking = 1;
    int res;
    
    close_server_socket();
    
    winsock_loaded = WSAStartup( MAKEWORD(2,2), &wsa_data ) == NO_ERROR;
    if (!winsock_loaded) return FALSE;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    server_addr.sin_port = htons( SERVER_PORT );
    
    server_sock = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
    if (server_sock == INVALID_SOCKET) return FALSE;
    
    res = setsockopt( server_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr, sizeof(reuse_addr) );
    if (res == SOCKET_ERROR) return FALSE;    
     
    ioctlsocket( server_sock, FIONBIO, &non_blocking );

    res = bind( server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr) );
    if (res == SOCKET_ERROR) return FALSE;
    
    return TRUE;
}

static void get_gamepad_request( void ) 
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr( "127.0.0.1" );
    client_addr.sin_port = htons( CLIENT_PORT );
    
    buffer[0] = REQUEST_CODE_GET_GAMEPAD;
    buffer[1] = 0;
    buffer[2] = 1;
    sendto( server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, sizeof(client_addr) );
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

static void set_device_state_axis( IDirectInputDevice8W *iface, DWORD dwOfs, DWORD id, short value, DWORD time, BOOL is_axis_value ) 
{
    struct object_properties *properties;
    int index = dinput_device_object_index_from_id( iface, id );
    properties = gamepad.base.object_properties + index;
    *(LONG *)(gamepad.base.device_state + dwOfs) = is_axis_value ? scale_axis_value( value, properties ) : scale_value( value, properties );
    queue_event( iface, index, *(LONG *)(gamepad.base.device_state + dwOfs), time, gamepad.base.dinput->evsequence );    
}

static void set_device_state_button( IDirectInputDevice8W *iface, DWORD id, BYTE value, DWORD time ) 
{
    DWORD dwOfs = DIJOFS_BUTTON( id );
    int index = dinput_device_object_index_from_id( iface, DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( id ) );
    gamepad.base.device_state[dwOfs] = value;
    queue_event( iface, index, gamepad.base.device_state[dwOfs], time, gamepad.base.dinput->evsequence );    
}

static void set_device_state_pov( IDirectInputDevice8W *iface, short value, DWORD time ) 
{
    DWORD dwOfs = DIJOFS_POV( 0 );
    int index = dinput_device_object_index_from_id( iface, DIDFT_POV | DIDFT_MAKEINSTANCE( 0 ) );
    *(LONG *)(gamepad.base.device_state + dwOfs) = value != -1 ? value * 4500 : -1;
    queue_event( iface, index, *(LONG *)(gamepad.base.device_state + dwOfs), time, gamepad.base.dinput->evsequence );    
}

static int get_standard_mapping_index( int index ) 
{
    switch (index)
    {
    case IDX_BUTTON_A: return 1;
    case IDX_BUTTON_B: return 2;
    case IDX_BUTTON_X: return 0;
    case IDX_BUTTON_Y: return 3;
    case IDX_BUTTON_L1: return 4;
    case IDX_BUTTON_R1: return 5;
    case IDX_BUTTON_L2: return 6;
    case IDX_BUTTON_R2: return 7;
    case IDX_BUTTON_SELECT: return 8;
    case IDX_BUTTON_START: return 9;
    case IDX_BUTTON_L3: return 10;
    case IDX_BUTTON_R3: return 11;
    default: return -1;
    }
}

static void gamepad_update_device_state( IDirectInputDevice8W *iface ) 
{
    int i;
    DWORD time = GetCurrentTime();
    gamepad.base.dinput->evsequence++;    
    
    if (gamepad.mapper_type == MAPPER_TYPE_STANDARD) 
    {
        set_device_state_axis( iface, DIJOFS_X, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ), gamepad.state.thumb_lx, time, TRUE );
        set_device_state_axis( iface, DIJOFS_Y, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ), gamepad.state.thumb_ly, time, TRUE );
        set_device_state_axis( iface, DIJOFS_Z, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ), gamepad.state.thumb_rx, time, TRUE );
        set_device_state_axis( iface, DIJOFS_RZ, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 ), gamepad.state.thumb_ry, time, TRUE );
        
        for (i = 0; i < 12; i++) set_device_state_button( iface, get_standard_mapping_index(i), (gamepad.state.buttons & (1<<i)) ? 0x80 : 0x00, time );            
           
        set_device_state_pov( iface, gamepad.state.dpad, time );
    }
    else if (gamepad.mapper_type == MAPPER_TYPE_XINPUT) 
    {        
        set_device_state_axis( iface, DIJOFS_X, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 0 ), gamepad.state.thumb_lx, time, TRUE );
        set_device_state_axis( iface, DIJOFS_Y, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 1 ), gamepad.state.thumb_ly, time, TRUE );
        set_device_state_axis( iface, DIJOFS_RX, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 3 ), gamepad.state.thumb_rx, time, TRUE );
        set_device_state_axis( iface, DIJOFS_RY, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 4 ), gamepad.state.thumb_ry, time, TRUE );
        
        for (i = 0; i < 10; i++) set_device_state_button( iface, i, (gamepad.state.buttons & (1<<i)) ? 0x80 : 0x00, time );
        
        set_device_state_axis( iface, DIJOFS_Z, DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( 2 ), (gamepad.state.buttons & (1<<10)) ? 32767 : ((gamepad.state.buttons & (1<<11)) ? -32768 : 0), time, FALSE );
        set_device_state_pov( iface, gamepad.state.dpad, time );
    }
    
    if (gamepad.base.hEvent) SetEvent( gamepad.base.hEvent );
}

static void gamepad_init( void ) {
    if (gamepad.name) {
        free( gamepad.name );
        gamepad.name = NULL;
    }
    
    gamepad.id = 0;
    gamepad.connected = FALSE;
    gamepad.mapper_type = MAPPER_TYPE_XINPUT;
}

static DWORD WINAPI gamepad_read_thread_proc(void *param) {
    int res;
    char buffer[BUFFER_SIZE];
    BOOL started = FALSE;
    DWORD curr_time, last_time;
    
    SetThreadDescription( GetCurrentThread(), L"wine_dinput_gamepad_read" );
    if (server_sock == INVALID_SOCKET && !create_server_socket()) 
    {
        SetEvent( start_event );
        return 0;
    }
    
    get_gamepad_request();
    
    last_time = GetCurrentTime();
    while (thread_running)
    {
        res = recvfrom( server_sock, buffer, BUFFER_SIZE, 0, NULL, 0 );
        if (res <= 0)
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK) break;
            
            curr_time = GetCurrentTime();
            if ((curr_time - last_time) >= 2000) {
                get_gamepad_request();
                last_time = curr_time;
            }
            
            Sleep(16);
            continue;
        }
        
        if (buffer[0] == REQUEST_CODE_GET_GAMEPAD) 
        {
            int gamepad_id;
            gamepad_id = *(int*)(buffer + 1);
            
            EnterCriticalSection( &gamepad.crit );
            gamepad_init();
            
            if (gamepad_id > 0) 
            {
                int name_len;
                
                gamepad.id = gamepad_id;
                gamepad.connected = TRUE;
                gamepad.mapper_type = buffer[5];
                
                name_len = *(int*)(buffer + 6);
                gamepad.name = malloc( name_len + 1 );
                memcpy( gamepad.name, buffer + 10, name_len );
                gamepad.name[name_len] = '\0';
            }
            LeaveCriticalSection( &gamepad.crit );
            
            if (!started) 
            {
                started = TRUE;
                SetEvent( start_event );    
            }
        }
        else if (buffer[0] == REQUEST_CODE_GET_GAMEPAD_STATE && gamepad.connected)
        {
            int gamepad_id;

            EnterCriticalSection( &gamepad.crit );            
            
            gamepad.changed = TRUE;
            gamepad_id = *(int*)(buffer + 2);
            if (buffer[1] != 1 || gamepad_id != gamepad.id)
            {
                gamepad_init();
                memset(&gamepad.state, 0, sizeof(gamepad.state));
                LeaveCriticalSection(&gamepad.crit);
                continue;
            }
        
            gamepad.state.buttons = *(short*)(buffer + 6);
            gamepad.state.dpad = buffer[8];
            gamepad.state.thumb_lx = *(short*)(buffer + 9);
            gamepad.state.thumb_ly = *(short*)(buffer + 11);
            gamepad.state.thumb_rx = *(short*)(buffer + 13);
            gamepad.state.thumb_ry = *(short*)(buffer + 15);
            
            LeaveCriticalSection( &gamepad.crit );
        }
    }
    
    return 0;
}

static void start_read_thread_once( void ) {    
    if (read_thread) return;
    thread_running = TRUE;

    start_event = CreateEventA( NULL, FALSE, FALSE, NULL );
    if (!start_event) ERR( "failed to create start event, error %lu\n", GetLastError() );
    
    read_thread = CreateThread( NULL, 0, gamepad_read_thread_proc, NULL, 0, NULL );
    if (!read_thread) ERR( "failed to create read thread, error %lu\n", GetLastError() );
    CloseHandle( read_thread );
    
    WaitForSingleObject( start_event, 2000 );
    CloseHandle( start_event );   
}

HRESULT gamepad_enum_device( DWORD type, DWORD flags, DIDEVICEINSTANCEW *instance, DWORD version )
{   
    DWORD size;

    TRACE( "type %#lx, flags %#lx, instance %p, version %#lx.\n", type, flags, instance, version );
    
    start_read_thread_once();
    
    if (!gamepad.connected) return DIERR_INPUTLOST;
    
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
    
    properties->physical_min = 0;
    properties->physical_max = 10000;

    if (instance->dwType & DIDFT_AXIS) 
    {
        properties->logical_min = -32768;
        properties->logical_max = 32767;
        properties->range_min = 0;
        properties->range_max = 65535;        
    }
    else 
    {
        properties->logical_min = -18000;
        properties->logical_max = 18000;
        properties->range_min = 0;
        properties->range_max = 36000;        
    }

    properties->saturation = 10000;
    properties->granularity = 1;

    return DIENUM_CONTINUE;
}

static void gamepad_release( IDirectInputDevice8W *iface )
{
    EnterCriticalSection( &gamepad.crit );
    CloseHandle( gamepad.base.read_event );
    
    thread_running = FALSE;
    CloseHandle( read_thread );
    read_thread = NULL;    
    
    gamepad_init();
    release_gamepad_request();
    close_server_socket();
    LeaveCriticalSection( &gamepad.crit );
}

static HRESULT gamepad_read( IDirectInputDevice8W *iface )
{   
    if (gamepad.acquired && gamepad.changed) 
    {
        EnterCriticalSection( &gamepad.crit );
        gamepad.changed = FALSE;
        gamepad_update_device_state( iface );
        LeaveCriticalSection( &gamepad.crit );
    }
    
    return DI_OK;
}

static HRESULT gamepad_acquire( IDirectInputDevice8W *iface )
{
    if (!gamepad.connected) return DIERR_UNPLUGGED;
    SetEvent( gamepad.base.read_event );
    gamepad.acquired = TRUE;
    return DI_OK;
}

static HRESULT gamepad_unacquire( IDirectInputDevice8W *iface )
{
    gamepad.acquired = FALSE;
    WaitForSingleObject( gamepad.base.read_event, INFINITE );
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

static void fill_device_object_instance( DIDEVICEOBJECTINSTANCEW *instance, WORD usage, int index ) 
{
    instance->dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE( index );    
    instance->wUsagePage = HID_USAGE_PAGE_GENERIC;
    instance->wUsage = usage;
    instance->dwFlags = DIDOI_ASPECTPOSITION;
        
    switch (usage) 
    {
    case HID_USAGE_GENERIC_X:
        instance->guidType = GUID_XAxis;
        instance->dwOfs = DIJOFS_X;
        lstrcpynW( instance->tszName, L"X Axis", MAX_PATH );
        break;
    case HID_USAGE_GENERIC_Y:
        instance->guidType = GUID_YAxis;
        instance->dwOfs = DIJOFS_Y;
        lstrcpynW( instance->tszName, L"Y Axis", MAX_PATH );
        break;
    case HID_USAGE_GENERIC_Z:
        instance->guidType = GUID_ZAxis;
        instance->dwOfs = DIJOFS_Z;
        lstrcpynW( instance->tszName, L"Z Axis", MAX_PATH );
        break;
    case HID_USAGE_GENERIC_RX:
        instance->guidType = GUID_RxAxis;
        instance->dwOfs = DIJOFS_RX;
        lstrcpynW( instance->tszName, L"Rx Axis", MAX_PATH );
        break;
    case HID_USAGE_GENERIC_RY:
        instance->guidType = GUID_RyAxis;
        instance->dwOfs = DIJOFS_RY;
        lstrcpynW( instance->tszName, L"Ry Axis", MAX_PATH );
        break; 
    case HID_USAGE_GENERIC_RZ:
        instance->guidType = GUID_RzAxis;
        instance->dwOfs = DIJOFS_RZ;
        lstrcpynW( instance->tszName, L"Rz Axis", MAX_PATH );
        break;
    case HID_USAGE_GENERIC_HATSWITCH:
        instance->guidType = GUID_POV;
        instance->dwOfs = DIJOFS_POV( 0 );
        instance->dwType = DIDFT_POV | DIDFT_MAKEINSTANCE( 0 );
        instance->dwFlags = 0;
        lstrcpynW( instance->tszName, L"POV", MAX_PATH );
        break;        
    }
}

static HRESULT gamepad_enum_objects( IDirectInputDevice8W *iface, const DIPROPHEADER *filter,
                                     DWORD flags, enum_object_callback callback, void *context )
{
    static const WORD standard_object_usages[] = {HID_USAGE_GENERIC_X, HID_USAGE_GENERIC_Y, HID_USAGE_GENERIC_Z, HID_USAGE_GENERIC_RZ, HID_USAGE_GENERIC_HATSWITCH, 0};
    static const WORD xinput_object_usages[] = {HID_USAGE_GENERIC_X, HID_USAGE_GENERIC_Y, HID_USAGE_GENERIC_Z, HID_USAGE_GENERIC_RX, HID_USAGE_GENERIC_RY, HID_USAGE_GENERIC_HATSWITCH, 0};
    
    DIDEVICEOBJECTINSTANCEW instance = {.dwSize = sizeof(DIDEVICEOBJECTINSTANCEW)};
    int i = 0, index = 0, button_count;
    const WORD *object_usages;
    BOOL ret;
    
    if (gamepad.mapper_type == MAPPER_TYPE_STANDARD) 
    {
        object_usages = standard_object_usages;
        button_count = 12;
    }
    else
    {
        object_usages = xinput_object_usages;
        button_count = 10;
    }

    while (object_usages[i] != 0)     {
        fill_device_object_instance( &instance, object_usages[i], i );
        
        ret = try_enum_object( &gamepad.base, filter, flags, callback, index++, &instance, context );
        if (ret != DIENUM_CONTINUE) return DIENUM_STOP;
        i++;
    }
    
    for (i = 0; i < button_count; i++)
    {
        instance.guidType = GUID_Button,
        instance.dwOfs = DIJOFS_BUTTON( i ),
        instance.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE( i ),
        instance.dwFlags = 0;
        swprintf( instance.tszName, MAX_PATH, L"Button %d", i );
        instance.wUsagePage = HID_USAGE_PAGE_BUTTON;
        instance.wUsage = i + 1;
            
        ret = try_enum_object( &gamepad.base, filter, flags, callback, index++, &instance, context );
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

    memset( &gamepad.base, 0, sizeof(gamepad.base) );
    memset( &gamepad.state, 0, sizeof(gamepad.state) );
   
    dinput_device_init( &gamepad.base, &gamepad_vtbl, guid, dinput );
    gamepad.base.crit.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": struct gamepad*->base.crit");
    gamepad.base.read_event = CreateEventW( NULL, TRUE, FALSE, NULL );

    gamepad_enum_device( 0, 0, &gamepad.base.instance, dinput->dwVersion );
    gamepad.base.caps.dwDevType = gamepad.base.instance.dwDevType;
    gamepad.base.caps.dwFirmwareRevision = 100;
    gamepad.base.caps.dwHardwareRevision = 100;
    gamepad.base.dwCoopLevel = DISCL_NONEXCLUSIVE | DISCL_BACKGROUND;
    
    if (FAILED(hr = dinput_device_init_device_format( &gamepad.base.IDirectInputDevice8W_iface ))) goto failed;
    gamepad_enum_objects( &gamepad.base.IDirectInputDevice8W_iface, &filter, DIDFT_AXIS | DIDFT_POV, init_object_properties, NULL );

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