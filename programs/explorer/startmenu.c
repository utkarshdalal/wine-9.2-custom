/*
 * Copyright (C) 2008 Vincent Povirk
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

#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include "wine/debug.h"
#include "wine/list.h"
#include "explorer_private.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(explorer);

struct menu_item
{
    struct list entry;
    LPWSTR displayname;

    /* parent information */
    struct menu_item* parent;
    LPITEMIDLIST pidl; /* relative to parent; absolute if parent->pidl is NULL */

    /* folder information */
    IShellFolder* folder;
    struct menu_item* base;
    HMENU menuhandle;
    BOOL menu_filled;
    HBITMAP hbitmap;
};

static struct list items = LIST_INIT(items);

static struct menu_item root_menu;
static struct menu_item public_startmenu;
static struct menu_item user_startmenu;

extern IShellLinkW *load_shelllink(const WCHAR *path);
extern HICON extract_icon(IShellLinkW *link, BOOL large_icon);

#define MENU_ID_RUN 1
#define MENU_ID_WINEBOOT 2

static INT_PTR CALLBACK wineboot_dlgproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
        {
            WCHAR label[64];
            CheckRadioButton(hwnd, IDC_RAD1, IDC_RAD3, IDC_RAD1);
            
            LoadStringW(NULL, IDS_WINE_BOOT_PROMPT, label, ARRAY_SIZE(label));
            SetWindowTextW(GetDlgItem(hwnd, IDC_LABEL), label);
            
            LoadStringW(NULL, IDS_EXIT_LABEL, label, ARRAY_SIZE(label));
            SetWindowTextW(GetDlgItem(hwnd, IDC_RAD1), label);
            
            LoadStringW(NULL, IDS_RESTART_LABEL, label, ARRAY_SIZE(label));
            SetWindowTextW(GetDlgItem(hwnd, IDC_RAD2), label);
            
            LoadStringW(NULL, IDS_RECONFIGURE_WINE, label, ARRAY_SIZE(label));
            SetWindowTextW(GetDlgItem(hwnd, IDC_RAD3), label);
            
            LoadStringW(NULL, IDS_OK, label, ARRAY_SIZE(label));
            SetWindowTextW(GetDlgItem(hwnd, IDOK), label);
            
            LoadStringW(NULL, IDS_CANCEL, label, ARRAY_SIZE(label));
            SetWindowTextW(GetDlgItem(hwnd, IDCANCEL), label);            
            
            ShowWindow(hwnd, SW_SHOWNORMAL);
            return TRUE;
        }
    case WM_COMMAND:
        switch (wparam)
        {
        case IDC_RAD1:
            CheckRadioButton(hwnd, IDC_RAD1, IDC_RAD3, IDC_RAD1);
            return TRUE;
        case IDC_RAD2:
            CheckRadioButton(hwnd, IDC_RAD1, IDC_RAD3, IDC_RAD2);
            return TRUE;
        case IDC_RAD3:
            CheckRadioButton(hwnd, IDC_RAD1, IDC_RAD3, IDC_RAD3);
            return TRUE;
        case IDOK:
            {
                char* arg = NULL;
                
                if (IsDlgButtonChecked(hwnd, IDC_RAD1) == BST_CHECKED)
                    arg = "--kill --force --shutdown";
                else if (IsDlgButtonChecked(hwnd, IDC_RAD2) == BST_CHECKED)
                    arg = "--end-session --force --restart";
                else if (IsDlgButtonChecked(hwnd, IDC_RAD3) == BST_CHECKED)
                    arg = "--update";
                
                ShellExecuteA(hwnd, "open", "wineboot.exe", arg, NULL, SW_HIDE);
                EndDialog(hwnd, TRUE);
                return TRUE;
            }            
        case IDCANCEL:
            EndDialog(hwnd, FALSE);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static ULONG copy_pidls(struct menu_item* item, LPITEMIDLIST dest)
{
    ULONG item_size;
    ULONG bytes_copied = 2;

    if (item->parent->pidl)
    {
        bytes_copied = copy_pidls(item->parent, dest);
    }

    item_size = ILGetSize(item->pidl);

    if (dest)
        memcpy(((char*)dest) + bytes_copied - 2, item->pidl, item_size);

    return bytes_copied + item_size - 2;
}

static LPITEMIDLIST build_pidl(struct menu_item* item)
{
    ULONG length;
    LPITEMIDLIST result;

    length = copy_pidls(item, NULL);

    result = CoTaskMemAlloc(length);

    copy_pidls(item, result);

    return result;
}

static void exec_item(struct menu_item* item)
{
    LPITEMIDLIST abs_pidl;
    SHELLEXECUTEINFOW sei;

    abs_pidl = build_pidl(item);

    ZeroMemory(&sei, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_IDLIST;
    sei.nShow = SW_SHOWNORMAL;
    sei.lpIDList = abs_pidl;

    ShellExecuteExW(&sei);

    CoTaskMemFree(abs_pidl);
}

static HRESULT pidl_to_shellfolder(LPITEMIDLIST pidl, LPWSTR *displayname, IShellFolder **out_folder)
{
    IShellFolder* parent_folder=NULL;
    LPCITEMIDLIST relative_pidl=NULL;
    STRRET strret;
    HRESULT hr;

    hr = SHBindToParent(pidl, &IID_IShellFolder, (void**)&parent_folder, &relative_pidl);

    if (displayname)
    {
        if (SUCCEEDED(hr))
            hr = IShellFolder_GetDisplayNameOf(parent_folder, relative_pidl, SHGDN_INFOLDER, &strret);

        if (SUCCEEDED(hr))
            hr = StrRetToStrW(&strret, NULL, displayname);
    }

    if (SUCCEEDED(hr))
        hr = IShellFolder_BindToObject(parent_folder, relative_pidl, NULL, &IID_IShellFolder, (void**)out_folder);

    if (parent_folder)
        IShellFolder_Release(parent_folder);

    return hr;
}

static BOOL shell_folder_is_empty(IShellFolder* folder)
{
    IEnumIDList* enumidl;
    LPITEMIDLIST pidl=NULL;

    if (IShellFolder_EnumObjects(folder, NULL, SHCONTF_NONFOLDERS, &enumidl) == S_OK)
    {
        if (IEnumIDList_Next(enumidl, 1, &pidl, NULL) == S_OK)
        {
            CoTaskMemFree(pidl);
            IEnumIDList_Release(enumidl);
            return FALSE;
        }

        IEnumIDList_Release(enumidl);
    }

    if (IShellFolder_EnumObjects(folder, NULL, SHCONTF_FOLDERS, &enumidl) == S_OK)
    {
        BOOL found = FALSE;
        IShellFolder *child_folder;

        while (!found && IEnumIDList_Next(enumidl, 1, &pidl, NULL) == S_OK)
        {
            if (IShellFolder_BindToObject(folder, pidl, NULL, &IID_IShellFolder, (void *)&child_folder) == S_OK)
            {
                if (!shell_folder_is_empty(child_folder))
                    found = TRUE;

                IShellFolder_Release(child_folder);
            }

            CoTaskMemFree(pidl);
        }

        IEnumIDList_Release(enumidl);

        if (found)
            return FALSE;
    }

    return TRUE;
}

static HBITMAP icon_to_bitmap(HICON hicon) 
{
    ICONINFO iconInfo;
    BITMAP bitmapInfo;
    HBITMAP hbitmap;
    HBITMAP hOldBitmap;
    HDC hdc = NULL;

    GetIconInfo(hicon, &iconInfo);
    GetObjectW(iconInfo.hbmColor, sizeof(BITMAP), &bitmapInfo);

    hdc = CreateCompatibleDC(NULL);
    hbitmap = CreateBitmap(bitmapInfo.bmWidth, bitmapInfo.bmHeight, 1, 32, NULL);
    hOldBitmap = SelectObject(hdc, hbitmap);

    RECT rect = {0, 0, bitmapInfo.bmWidth, bitmapInfo.bmHeight};
    FillRect(hdc, &rect, (HBRUSH)(COLOR_MENU + 1));
    DrawIconEx(hdc, 0, 0, hicon, bitmapInfo.bmWidth, bitmapInfo.bmHeight, 0, 0, DI_NORMAL); 

    SelectObject(hdc, hOldBitmap);
    DeleteDC(hdc);
    DestroyIcon(hicon);
    return hbitmap;
}

static int get_icon_index(WCHAR *name, BOOL folder) 
{
    int iconIndex;

    if (!lstrcmpW(L"Control Panel", name)) 
    {
        iconIndex = 36;
    }
    else if (!lstrcmpW(L"Programs", name)) 
    {
        iconIndex = 513;
    }
    else if (!lstrcmpW(L"Internet Settings", name)) 
    {
        iconIndex = 14;
    }
    else if (!lstrcmpW(L"System Tools", name)) 
    {
        iconIndex = 37;
    }
    else if (!lstrcmpW(L"Game Controllers", name)) 
    {
        iconIndex = 514;
    }
    else if (!lstrcmpW(L"Add/Remove Programs", name)) 
    {
        iconIndex = 148;
    }    
    else 
    {
        iconIndex = folder ? 4 : 30;
    }

    return -iconIndex;
}

/* add an individual file or folder to the menu, takes ownership of pidl */
static struct menu_item* add_shell_item(struct menu_item* parent, LPITEMIDLIST pidl)
{
    struct menu_item* item;
    MENUITEMINFOW mii;
    HMENU parent_menu;
    int existing_item_count, i;
    BOOL match = FALSE;
    SFGAOF flags;

    item = calloc( 1, sizeof(struct menu_item) );

    if (parent->pidl == NULL)
    {
        pidl_to_shellfolder(pidl, &item->displayname, &item->folder);
    }
    else
    {
        STRRET strret;

        if (SUCCEEDED(IShellFolder_GetDisplayNameOf(parent->folder, pidl, SHGDN_INFOLDER, &strret)))
            StrRetToStrW(&strret, NULL, &item->displayname);

        flags = SFGAO_FOLDER;
        IShellFolder_GetAttributesOf(parent->folder, 1, (LPCITEMIDLIST*)&pidl, &flags);

        if (flags & SFGAO_FOLDER)
            IShellFolder_BindToObject(parent->folder, pidl, NULL, &IID_IShellFolder, (void *)&item->folder);
    }

    if (item->folder && shell_folder_is_empty(item->folder))
    {
        IShellFolder_Release(item->folder);
        free( item->displayname );
        free( item );
        CoTaskMemFree(pidl);
        return NULL;
    }

    parent_menu = parent->menuhandle;

    item->parent = parent;
    item->pidl = pidl;
    item->hbitmap = NULL;

    existing_item_count = GetMenuItemCount(parent_menu);
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_SUBMENU|MIIM_DATA;

    /* search for an existing menu item with this name or the spot to insert this item */
    if (parent->pidl != NULL)
    {
        for (i=0; i<existing_item_count; i++)
        {
            struct menu_item* existing_item;
            int cmp;

            GetMenuItemInfoW(parent_menu, i, TRUE, &mii);
            existing_item = ((struct menu_item*)mii.dwItemData);

            if (!existing_item)
                continue;

            /* folders before files */
            if (existing_item->folder && !item->folder)
                continue;
            if (!existing_item->folder && item->folder)
                break;

            cmp = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, item->displayname, -1, existing_item->displayname, -1);

            if (cmp == CSTR_LESS_THAN)
                break;

            if (cmp == CSTR_EQUAL)
            {
                match = TRUE;
                break;
            }
        }
    }
    else
        /* This item manually added to the root menu, so put it at the end */
        i = existing_item_count;

    if (!match)
    {
        HICON hicon = NULL;
        WCHAR wszPath[MAX_PATH];
        LPITEMIDLIST abs_pidl;
        IShellLinkW *link;
        
        /* no existing item with the same name; just add it */
        mii.fMask = MIIM_STRING|MIIM_DATA|MIIM_BITMAP;
        mii.dwTypeData = item->displayname;
        mii.dwItemData = (ULONG_PTR)item;
        
        abs_pidl = build_pidl(item);
        SHGetPathFromIDListW(abs_pidl, wszPath);
        CoTaskMemFree(abs_pidl);        

        if (item->folder)
        {
            MENUINFO mi;
            item->menuhandle = CreatePopupMenu();
            mii.fMask |= MIIM_SUBMENU;
            mii.hSubMenu = item->menuhandle;

            mi.cbSize = sizeof(mi);
            mi.fMask = MIM_MENUDATA|MIM_STYLE;
            mi.dwStyle = MNS_CHECKORBMP;
            mi.dwMenuData = (ULONG_PTR)item;
            SetMenuInfo(item->menuhandle, &mi);
        }
        else if ((link = load_shelllink(wszPath))) 
        {
            hicon = extract_icon(link, FALSE);
            IShellLinkW_Release(link);
        }        

        if (!hicon) 
        {
            int iconIndex = get_icon_index(item->displayname, item->folder ? TRUE : FALSE);
            ExtractIconExA("shell32.dll", iconIndex, NULL, &hicon, 1);
        }

        item->hbitmap = icon_to_bitmap(hicon);
        mii.hbmpItem = item->hbitmap;
        InsertMenuItemW(parent->menuhandle, i, TRUE, &mii);

        list_add_tail(&items, &item->entry);
    }
    else if (item->folder)
    {
        /* there is an existing folder with the same name, combine them */
        MENUINFO mi;

        item->base = (struct menu_item*)mii.dwItemData;
        item->menuhandle = item->base->menuhandle;

        mii.dwItemData = (ULONG_PTR)item;
        SetMenuItemInfoW(parent_menu, i, TRUE, &mii);

        mi.cbSize = sizeof(mi);
        mi.fMask = MIM_MENUDATA|MIM_STYLE;
        mi.dwStyle = MNS_CHECKORBMP;
        mi.dwMenuData = (ULONG_PTR)item;
        SetMenuInfo(item->menuhandle, &mi);

        list_add_tail(&items, &item->entry);
    }
    else {
        /* duplicate shortcut, do nothing */
        free( item->displayname );
        free( item );
        CoTaskMemFree(pidl);
        item = NULL;
    }

    return item;
}

static void add_folder_contents(struct menu_item* parent)
{
    IEnumIDList* enumidl;

    if (IShellFolder_EnumObjects(parent->folder, NULL,
        SHCONTF_FOLDERS|SHCONTF_NONFOLDERS, &enumidl) == S_OK)
    {
        LPITEMIDLIST rel_pidl=NULL;
        while (S_OK == IEnumIDList_Next(enumidl, 1, &rel_pidl, NULL))
        {
            add_shell_item(parent, rel_pidl);
        }

        IEnumIDList_Release(enumidl);
    }
}

static void destroy_menus(void)
{
    if (!root_menu.menuhandle)
        return;

    DestroyMenu(root_menu.menuhandle);
    root_menu.menuhandle = NULL;
    
    if (root_menu.hbitmap) 
    {
        DeleteObject(root_menu.hbitmap);
        root_menu.hbitmap = NULL;
    }    

    while (!list_empty(&items))
    {
        struct menu_item* item;

        item = LIST_ENTRY(list_head(&items), struct menu_item, entry);

        if (item->folder)
            IShellFolder_Release(item->folder);
        
        if (item->hbitmap) 
        {
            DeleteObject(item->hbitmap);
            item->hbitmap = NULL;            
        }        

        CoTaskMemFree(item->pidl);
        CoTaskMemFree(item->displayname);

        list_remove(&item->entry);
        free( item );
    }
}

static void fill_menu(struct menu_item* item)
{
    if (!item->menu_filled)
    {
        add_folder_contents(item);

        if (item->base)
        {
            fill_menu(item->base);
        }

        item->menu_filled = TRUE;
    }
}

static void run_dialog(void)
{
    void (WINAPI *pRunFileDlg)(HWND owner, HICON icon, const char *dir,
                               const char *title, const char *desc, DWORD flags);
    HMODULE hShell32;

    hShell32 = LoadLibraryW(L"shell32");
    pRunFileDlg = (void*)GetProcAddress(hShell32, (LPCSTR)61);

    pRunFileDlg(NULL, NULL, NULL, NULL, NULL, 0);

    FreeLibrary(hShell32);
}

LRESULT menu_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_INITMENUPOPUP:
        {
            HMENU hmenu = (HMENU)wparam;
            struct menu_item* item;
            MENUINFO mi;

            mi.cbSize = sizeof(mi);
            mi.fMask = MIM_MENUDATA;
            GetMenuInfo(hmenu, &mi);
            item = (struct menu_item*)mi.dwMenuData;

            if (item)
                fill_menu(item);
            return 0;
        }
        break;

    case WM_MENUCOMMAND:
        {
            HMENU hmenu = (HMENU)lparam;
            struct menu_item* item;
            MENUITEMINFOW mii;

            mii.cbSize = sizeof(mii);
            mii.fMask = MIIM_DATA|MIIM_ID;
            GetMenuItemInfoW(hmenu, wparam, TRUE, &mii);
            item = (struct menu_item*)mii.dwItemData;

            if (item)
                exec_item(item);
            else if (mii.wID == MENU_ID_RUN)
                run_dialog();
            else if (mii.wID == MENU_ID_WINEBOOT)
                DialogBoxW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_WINEBOOT), NULL, wineboot_dlgproc);

            destroy_menus();

            return 0;
        }
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void do_startmenu(HWND hwnd)
{
    LPITEMIDLIST pidl;
    MENUINFO mi;
    MENUITEMINFOW mii;
    RECT rc={0,0,0,0};
    TPMPARAMS tpm;
    HICON hicon = NULL;
    WCHAR label[64];

    destroy_menus();

    TRACE( "creating start menu\n" );

    root_menu.menuhandle = public_startmenu.menuhandle = user_startmenu.menuhandle = CreatePopupMenu();
    if (!root_menu.menuhandle)
    {
        return;
    }

    user_startmenu.parent = public_startmenu.parent = &root_menu;
    user_startmenu.base = &public_startmenu;
    user_startmenu.menu_filled = public_startmenu.menu_filled = FALSE;

    if (!user_startmenu.pidl)
        SHGetSpecialFolderLocation(NULL, CSIDL_STARTMENU, &user_startmenu.pidl);

    if (!user_startmenu.folder)
        pidl_to_shellfolder(user_startmenu.pidl, NULL, &user_startmenu.folder);

    if (!public_startmenu.pidl)
        SHGetSpecialFolderLocation(NULL, CSIDL_COMMON_STARTMENU, &public_startmenu.pidl);

    if (!public_startmenu.folder)
        pidl_to_shellfolder(public_startmenu.pidl, NULL, &public_startmenu.folder);

    if ((user_startmenu.folder && !shell_folder_is_empty(user_startmenu.folder)) ||
        (public_startmenu.folder && !shell_folder_is_empty(public_startmenu.folder)))
    {
        fill_menu(&user_startmenu);

        AppendMenuW(root_menu.menuhandle, MF_SEPARATOR, 0, NULL);
    }

    if (SUCCEEDED(SHGetSpecialFolderLocation(NULL, CSIDL_CONTROLS, &pidl)))
        add_shell_item(&root_menu, pidl);

    LoadStringW(NULL, IDS_RUN, label, ARRAY_SIZE(label));
    mii.cbSize = sizeof(mii);
    mii.fMask = MIIM_STRING|MIIM_ID|MIIM_BITMAP;
    mii.dwTypeData = label;
    mii.wID = MENU_ID_RUN;
    
    ExtractIconExA("shell32.dll", -515, NULL, &hicon, 1);
    root_menu.hbitmap = icon_to_bitmap(hicon);
    mii.hbmpItem = root_menu.hbitmap;    
    InsertMenuItemW(root_menu.menuhandle, -1, TRUE, &mii);

    mii.fMask = MIIM_FTYPE;
    mii.fType = MFT_SEPARATOR;
    InsertMenuItemW(root_menu.menuhandle, -1, TRUE, &mii);

    LoadStringW(NULL, IDS_WINEBOOT_LABEL, label, ARRAY_SIZE(label));
    mii.fMask = MIIM_STRING|MIIM_ID|MIIM_BITMAP;
    mii.dwTypeData = label;
    mii.wID = MENU_ID_WINEBOOT;
    
    ExtractIconExA("shell32.dll", -28, NULL, &hicon, 1);
    root_menu.hbitmap = icon_to_bitmap(hicon);
    mii.hbmpItem = root_menu.hbitmap;    
    InsertMenuItemW(root_menu.menuhandle, -1, TRUE, &mii);

    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_STYLE;
    mi.dwStyle = MNS_NOTIFYBYPOS|MNS_CHECKORBMP;
    SetMenuInfo(root_menu.menuhandle, &mi);

    GetWindowRect(hwnd, &rc);

    tpm.cbSize = sizeof(tpm);
    tpm.rcExclude = rc;

    if (!TrackPopupMenuEx(root_menu.menuhandle,
        TPM_LEFTALIGN|TPM_BOTTOMALIGN|TPM_VERTICAL,
        rc.left, rc.top, hwnd, &tpm))
    {
        ERR( "couldn't display menu\n" );
    }
}
