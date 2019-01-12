/*
 * NineWineCfg main entry point
 *
 * Copyright 2002 Jaco Greeff
 * Copyright 2003 Dimitrie O. Paun
 * Copyright 2003 Mike Hearn
 * Copyright 2017 Patrick Rudolph
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
 *
 */

#include <windows.h>
#include <ntstatus.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <objbase.h>
#include <winternl.h>
#include <d3d9.h>
#include <wine/debug.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
#include <wctype.h>

#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(ninecfg);

#ifndef WINE_STAGING
#warning DO NOT DEFINE WINE_STAGING TO 1 ON STABLE BRANCHES, ONLY ON WINE-STAGING ENABLED WINES
#define WINE_STAGING 1
#endif

static const char * const fn_nine_dll = "d3d9-nine.dll";
static const char * const reg_path_dll_overrides = "Software\\Wine\\DllOverrides";
static const char * const reg_path_dll_redirects = "Software\\Wine\\DllRedirects";
static const char * const reg_key_d3d9 = "d3d9";
static const char * const reg_path_nine = "Software\\Wine\\Direct3DNine";
static const char * const reg_key_module_path = "ModulePath";

#if !WINE_STAGING
static const char * const fn_d3d9_dll = "d3d9.dll";
static const char * const fn_nine_exe = "ninewinecfg.exe";
static const char * const reg_value_override = "native";
#else
static const char * const reg_value_redirect = fn_nine_dll;
#endif

#if !WINE_STAGING
static BOOL isWin64(void)
{
    return sizeof(void*) == 8;
}

static BOOL isWoW64(void)
{
    BOOL is_wow64;

    return IsWow64Process( GetCurrentProcess(), &is_wow64 ) && is_wow64;
}

static DWORD executeCmdline(LPSTR cmdline)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD exit_code;

    ZeroMemory( &si, sizeof(si) );
    si.cb = sizeof(si);
    ZeroMemory( &pi, sizeof(pi) );

    WINE_TRACE("Executing cmdline '%s'\n", cmdline);

    if (!CreateProcessA(NULL, cmdline, NULL, NULL,
        FALSE, 0, NULL, NULL, &si, &pi ))
    {
        WINE_ERR("CreateProcessA failed, error=%d", GetLastError());
        return ~0u;
    }

    if (WaitForSingleObject( pi.hProcess, INFINITE ) != WAIT_OBJECT_0)
    {
        WINE_ERR("WaitForSingleObject failed, error=%d", GetLastError());
        return ~0u;
    }

    if (!GetExitCodeProcess( pi.hProcess, &exit_code ))
    {
        WINE_ERR("GetExitCodeProcess failed, error=%d", GetLastError());
        return ~0u;
    }

    WINE_TRACE("Exit code: %u\n", exit_code);

    return exit_code;
}

static BOOL Call32bitNineWineCfg(BOOL state)
{
    CHAR buf[MAX_PATH + 6];

    if (!GetSystemWow64DirectoryA(buf, sizeof(buf)))
        return FALSE;

    strcat(buf, "\\");
    strcat(buf, fn_nine_exe);

    if (state)
        strcat(buf, " -e -n");
    else
        strcat(buf, " -d -n");

    return executeCmdline(buf) == 0;
}

static BOOL Call64bitNineWineCfg(BOOL state)
{
    void *redir;
    CHAR buf[MAX_PATH + 6];
    BOOL res;

    Wow64DisableWow64FsRedirection( &redir );

    if (!GetSystemDirectoryA((LPSTR)buf, sizeof(buf)))
        return FALSE;

    strcat(buf, "\\");
    strcat(buf, fn_nine_exe);

    if (state)
        strcat(buf, " -e -n");
    else
        strcat(buf, " -d -n");

    res = executeCmdline(buf) == 0;

    Wow64RevertWow64FsRedirection( redir );

    return res;
}

/* helper functions taken from NTDLL and KERNEL32 */
static LPWSTR FILE_name_AtoW(LPCSTR name, int optarg)
{
    ANSI_STRING str;
    UNICODE_STRING strW, *pstrW;
    NTSTATUS status;

    RtlInitAnsiString( &str, name );
    pstrW = &strW ;
    status = RtlAnsiStringToUnicodeString( pstrW, &str, TRUE );
    if (status == STATUS_SUCCESS)
        return pstrW->Buffer;

    return NULL;
}

static BOOL WINAPI DeleteSymLinkW(LPCWSTR lpFileName)
{
    NTSTATUS status;
    UNICODE_STRING ntDest;
    ANSI_STRING unixDest;
    BOOL ret = FALSE;

    WINE_TRACE("(%s)\n", wine_dbgstr_w(lpFileName));

    ntDest.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U( lpFileName, &ntDest, NULL, NULL ))
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    unixDest.Buffer = NULL;
    status = wine_nt_to_unix_file_name( &ntDest, &unixDest, 0, FALSE );
    if (!status)
    {
        if (!unlink(unixDest.Buffer))
        {
            WINE_TRACE("Removed symlink '%s'\n", wine_dbgstr_a( unixDest.Buffer ));
            ret = TRUE;
            status = STATUS_SUCCESS;
        }
        else
        {
            WINE_ERR("Failed to remove symlink\n");
        }
    }

    if (status)
         SetLastError( RtlNtStatusToDosError(status) );

    RtlFreeAnsiString( &unixDest );

err:
    RtlFreeUnicodeString( &ntDest );
    return ret;
}

static BOOL WINAPI DeleteSymLinkA(LPCSTR lpFileName)
{
    WCHAR *destW;
    BOOL res;

    if (!(destW = FILE_name_AtoW( lpFileName, TRUE )))
    {
        return FALSE;
    }

    res = DeleteSymLinkW( destW );

    HeapFree( GetProcessHeap(), 0, destW );

    return res;
}

static BOOL WINAPI CreateSymLinkW(LPCWSTR lpFileName, LPCSTR existingUnixFileName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    NTSTATUS status;
    UNICODE_STRING ntDest;
    ANSI_STRING unixDest;
    BOOL ret = FALSE;

    WINE_TRACE("(%s, %s, %p)\n", wine_dbgstr_w(lpFileName),
         existingUnixFileName, lpSecurityAttributes);

    ntDest.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U( lpFileName, &ntDest, NULL, NULL ))
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    unixDest.Buffer = NULL;
    status = wine_nt_to_unix_file_name( &ntDest, &unixDest, FILE_CREATE, FALSE );
    if (!status) /* destination must not exist */
    {
        status = STATUS_OBJECT_NAME_EXISTS;
    } else if (status == STATUS_NO_SUCH_FILE)
    {
        status = STATUS_SUCCESS;
    }

    if (status)
        SetLastError( RtlNtStatusToDosError(status) );
    else if (!symlink( existingUnixFileName, unixDest.Buffer ))
    {
        WINE_TRACE("Symlinked '%s' to '%s'\n", wine_dbgstr_a( unixDest.Buffer ),
            existingUnixFileName);
        ret = TRUE;
    }

    RtlFreeAnsiString( &unixDest );

err:
    RtlFreeUnicodeString( &ntDest );
    return ret;
}

static BOOL WINAPI CreateSymLinkA(LPCSTR lpFileName, LPCSTR lpExistingUnixFileName,
    LPSECURITY_ATTRIBUTES lpSecurityAttributes)
{
    WCHAR *destW;
    BOOL res;

    if (!(destW = FILE_name_AtoW( lpFileName, TRUE )))
    {
        return FALSE;
    }

    res = CreateSymLinkW( destW, lpExistingUnixFileName, lpSecurityAttributes );

    HeapFree( GetProcessHeap(), 0, destW );

    return res;
}

static BOOL WINAPI IsFileSymLinkW(LPCWSTR lpExistingFileName)
{
    NTSTATUS status;
    UNICODE_STRING ntSource;
    ANSI_STRING unixSource;
    BOOL ret = FALSE;
    struct stat sb;

    WINE_TRACE("(%s)\n", wine_dbgstr_w(lpExistingFileName));

    ntSource.Buffer = NULL;
    if (!RtlDosPathNameToNtPathName_U( lpExistingFileName, &ntSource, NULL, NULL ))
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    unixSource.Buffer = NULL;
    status = wine_nt_to_unix_file_name( &ntSource, &unixSource, FILE_OPEN, FALSE );
    if (status == STATUS_NO_SUCH_FILE)
    {
        SetLastError( ERROR_PATH_NOT_FOUND );
        goto err;
    }

    if (!lstat( unixSource.Buffer, &sb) && (sb.st_mode & S_IFMT) == S_IFLNK)
    {
        ret = TRUE;
    }

    RtlFreeAnsiString( &unixSource );

err:
    RtlFreeUnicodeString( &ntSource );
    return ret;
}

static BOOL WINAPI IsFileSymLinkA(LPCSTR lpExistingFileName)
{
    WCHAR *sourceW;
    BOOL res;

    if (!(sourceW = FILE_name_AtoW( lpExistingFileName, TRUE )))
    {
        return FALSE;
    }

    res = IsFileSymLinkW( sourceW );

    HeapFree( GetProcessHeap(), 0, sourceW );

    return res;
}

static BOOL nine_get_system_path(CHAR *pOut, DWORD SizeOut)
{
    if (isWoW64())
    {
        return !!GetSystemWow64DirectoryA((LPSTR)pOut, SizeOut);
    }
    else
    {
        return !!GetSystemDirectoryA((LPSTR)pOut, SizeOut);
    }
}
#endif

/*
 * Winecfg
 */
WCHAR* load_string (UINT id)
{
    WCHAR buf[1024];
    int len;
    WCHAR* newStr;

    LoadStringW (GetModuleHandleW(NULL), id, buf, sizeof(buf)/sizeof(buf[0]));

    len = lstrlenW (buf);
    newStr = HeapAlloc (GetProcessHeap(), 0, (len + 1) * sizeof (WCHAR));
    memcpy (newStr, buf, len * sizeof (WCHAR));
    newStr[len] = 0;

    return newStr;
}

/*
 * Gallium nine
 */
static BOOL getRegistryString(LPCSTR path, LPCSTR name, LPSTR *value)
{
    HKEY regkey;
    DWORD type;
    DWORD size = 0;

    WINE_TRACE("Getting string key '%s' at 'HKCU\\%s'\n", name, path);

    if (RegOpenKeyA(HKEY_CURRENT_USER, path, &regkey) != ERROR_SUCCESS)
    {
        WINE_TRACE("Failed to open path 'HKCU\\%s'\n", path);
        return FALSE;
    }

    if (RegQueryValueExA(regkey, name, 0, &type, NULL, &size) != ERROR_SUCCESS)
    {
        WINE_TRACE("Failed to query key '%s' at 'HKCU\\%s'\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    if (type != REG_SZ)
    {
        WINE_TRACE("Key '%s' at 'HKCU\\%s' is not a string\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    *value = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 1);
    if (!(*value))
    {
        RegCloseKey(regkey);
        return FALSE;
    }

    if (RegQueryValueExA(regkey, name, 0, &type, (LPBYTE)*value, &size) != ERROR_SUCCESS)
    {
        WINE_TRACE("Failed to read value of key '%s' at 'HKCU\\%s'\n", name, path);
        HeapFree(GetProcessHeap(), 0, *value);
        RegCloseKey(regkey);
        return FALSE;
    }

    RegCloseKey(regkey);

    WINE_TRACE("Value is '%s'\n", *value);

    return TRUE;
}

static BOOL setRegistryString(LPCSTR path, LPCSTR name, LPCSTR value)
{
    HKEY regkey;

    WINE_TRACE("Setting key '%s' at 'HKCU\\%s' to '%s'\n", name, path, value);

    if (RegCreateKeyA(HKEY_CURRENT_USER, path, &regkey) != ERROR_SUCCESS)
    {
        WINE_TRACE("Failed to open path 'HKCU\\%s'\n", path);
        return FALSE;
    }

    if (RegSetValueExA(regkey, name, 0, REG_SZ, (LPBYTE)value, strlen(value)) != ERROR_SUCCESS)
    {
        WINE_TRACE("Failed to write key '%s' at 'HKCU\\%s'\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    RegCloseKey(regkey);

    return TRUE;
}

static BOOL delRegistryKey(LPCSTR path, LPCSTR name)
{
    HKEY regkey;
    LSTATUS rc;

    WINE_TRACE("Deleting key '%s' at 'HKCU\\%s'\n", name, path);

    rc = RegOpenKeyA(HKEY_CURRENT_USER, path, &regkey);
    if (rc == ERROR_FILE_NOT_FOUND)
        return TRUE;

    if (rc != ERROR_SUCCESS)
    {
        WINE_TRACE("Failed to open path 'HKCU\\%s'\n", path);
        return FALSE;
    }

    rc = RegDeleteValueA(regkey, name);
    if (rc != ERROR_FILE_NOT_FOUND && rc != ERROR_SUCCESS)
    {
        WINE_TRACE("Failed to delete key '%s' at 'HKCU\\%s'\n", name, path);
        RegCloseKey(regkey);
        return FALSE;
    }

    RegCloseKey(regkey);

    return TRUE;
}

static BOOL nine_get(void)
{
    BOOL ret = FALSE;
    LPSTR value;

#if WINE_STAGING
    if (getRegistryString(reg_path_dll_redirects, reg_key_d3d9, &value))
    {
        ret = !strcmp(value, reg_value_redirect);
        HeapFree(GetProcessHeap(), 0, value);
    }
#else
    CHAR buf[MAX_PATH];

    if (getRegistryString(reg_path_dll_overrides, reg_key_d3d9, &value))
    {
        ret = !strcmp(value, reg_value_override);
        HeapFree(GetProcessHeap(), 0, value);
    }

    if (!nine_get_system_path(buf, sizeof(buf)))
    {
        WINE_ERR("Failed to get system path\n");
        return FALSE;
    }
    strcat(buf, "\\");
    strcat(buf, fn_d3d9_dll);

    if (!ret && IsFileSymLinkA(buf))
    {
        /* Sanity: Remove symlink if any */
        WINE_ERR("removing obsolete symlink\n");
        DeleteSymLinkA(buf);
        return FALSE;
    }

    ret = IsFileSymLinkA(buf);
    if (ret && !PathFileExistsA(buf))
    {
        /* broken symlink */
        DeleteSymLinkA(buf);
        WINE_ERR("removing dead symlink\n");
        return FALSE;
    }
#endif

    return ret;
}

static void nine_set(BOOL status, BOOL NoOtherArch)
{
#if WINE_STAGING
    /* Delete unused DllOverrides key */
    delRegistryKey(reg_path_dll_overrides, reg_key_d3d9);

    /* Active dll redirect */
    if (!status)
    {
        if (!delRegistryKey(reg_path_dll_redirects, reg_key_d3d9))
            WINE_ERR("Failed to delete 'HKCU\\%s\\%s'\n'", reg_path_dll_redirects, reg_key_d3d9);
    }
    else
    {
        if (!setRegistryString(reg_path_dll_redirects, reg_key_d3d9, reg_value_redirect))
            WINE_ERR("Failed to write 'HKCU\\%s\\%s'\n", reg_path_dll_redirects, reg_key_d3d9);
    }
#else
    CHAR dst[MAX_PATH];

    /* Prevent infinite recursion if called from other arch already */
    if (!NoOtherArch)
    {
        /* Started as 64bit, call 32bit process */
        if (isWin64())
            Call32bitNineWineCfg(status);
        /* Started as 32bit, call 64bit process */
        else if (isWoW64())
            Call64bitNineWineCfg(status);
    }

    /* Delete unused DllRedirects key */
    delRegistryKey(reg_path_dll_redirects, reg_key_d3d9);

    /* enable native dll */
    if (!status)
    {
        if (!delRegistryKey(reg_path_dll_overrides, reg_key_d3d9))
            WINE_ERR("Failed to delete 'HKCU\\%s\\%s'\n'", reg_path_dll_overrides, reg_key_d3d9);
    }
    else
    {
        if (!setRegistryString(reg_path_dll_overrides, reg_key_d3d9, reg_value_override))
            WINE_ERR("Failed to write 'HKCU\\%s\\%s'\n", reg_path_dll_overrides, reg_key_d3d9);
    }

    if (!nine_get_system_path(dst, sizeof(dst))) {
        WINE_ERR("Failed to get system path\n");
        return;
    }
    strcat(dst, "\\");
    strcat(dst, fn_d3d9_dll);

    if (status)
    {
        HMODULE hmod;

        /* Sanity: Always recreate symlink */
        DeleteSymLinkA(dst);

        hmod = LoadLibraryExA(fn_nine_dll, NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (hmod)
        {
            Dl_info info;

            if (dladdr(hmod, &info) && info.dli_fname)
            {
                if (!CreateSymLinkA(dst, info.dli_fname, NULL))
                    WINE_ERR("CreateSymLinkA(%s,%s) failed\n", dst, info.dli_fname);

            }
            else
                WINE_ERR("dladdr failed to get file path\n");

            FreeLibrary(hmod);
        }
        else
            WINE_ERR("%s not found.\n", fn_nine_dll);
    }
    else
        DeleteSymLinkA(dst);
#endif
}

typedef IDirect3D9* (WINAPI *LPDIRECT3DCREATE9)( UINT );

static void load_staging_settings(HWND dialog)
{
    HMODULE hmod = NULL;
    char have_modpath = 0;
    char *mod_path = NULL;
    LPDIRECT3DCREATE9 Direct3DCreate9Ptr = NULL;
    IDirect3D9 *iface = NULL;
    void *handle;

#if defined(D3D9NINE_MODULEPATH)
    have_modpath = 1;
    mod_path = (char*)D3D9NINE_MODULEPATH;
#endif

    CheckDlgButton(dialog, IDC_ENABLE_NATIVE_D3D9, nine_get() ? BST_CHECKED : BST_UNCHECKED);

    SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP2, NULL);
    SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP3, NULL);
    SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP4, NULL);
    SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP5, NULL);

    CheckDlgButton(dialog, IDC_NINE_STATE2, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE3, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE4, BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_NINE_STATE5, BST_UNCHECKED);

    if (!have_modpath && getRegistryString(reg_path_nine, reg_key_module_path, &mod_path))
        have_modpath = 1;

    if (have_modpath)
    {
        SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP2, mod_path);
        CheckDlgButton(dialog, IDC_NINE_STATE2, BST_CHECKED);
    }
    else
    {
        goto out;
    }

    handle = dlopen(mod_path, RTLD_GLOBAL | RTLD_NOW);
    if (handle)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE3, BST_CHECKED);
    }
    else
    {
        SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP3, dlerror());
        goto out;
    }

    hmod = LoadLibraryA(fn_nine_dll);
    if (hmod)
        Direct3DCreate9Ptr = (LPDIRECT3DCREATE9)
                GetProcAddress(hmod, "Direct3DCreate9");

    if (hmod && Direct3DCreate9Ptr)
    {
        CheckDlgButton(dialog, IDC_NINE_STATE4, BST_CHECKED);
        {
            Dl_info info;

            if (dladdr(hmod, &info) && info.dli_fname)
                SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP4, info.dli_fname);
            else
                SetDlgItemTextA(dialog, IDC_NINE_STATE_TIP4, dlerror());
        }
    }
    else
    {
        wchar_t buf[256];
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPWSTR)buf, 256, NULL);

        SetDlgItemTextW(dialog, IDC_NINE_STATE_TIP4, buf);
        goto out;
    }

    iface = Direct3DCreate9Ptr(0);
    if (iface)
    {
        IDirect3DDevice9_Release(iface);
        CheckDlgButton(dialog, IDC_NINE_STATE5, BST_CHECKED);
    }
    else
    {
        SetDlgItemTextW(dialog, IDC_NINE_STATE_TIP5, load_string(IDS_NINECFG_D3D_ERROR));
        goto out;
    }

    if (hmod)
        FreeLibrary(hmod);

    return;

out:
    EnableWindow(GetDlgItem(dialog, IDC_ENABLE_NATIVE_D3D9), 0);

    if (hmod)
        FreeLibrary(hmod);
}

static BOOL ProcessCmdLine(WCHAR *cmdline, BOOL *result)
{
    WCHAR **argv;
    int argc, i;
    BOOL NoOtherArch = FALSE;
    BOOL NineSet = FALSE;
    BOOL NineClear = FALSE;

    argv = CommandLineToArgvW(cmdline, &argc);

    if (!argv)
        return FALSE;

    if (argc == 1)
    {
        LocalFree(argv);
        return FALSE;
    }

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] != '/' && argv[i][0] != '-')
            break; /* No flags specified. */

        if (!argv[i][1] && argv[i][0] == '-')
            break; /* '-' is a filename. It indicates we should use stdin. */

        if (argv[i][1] && argv[i][2] && argv[i][2] != ':')
            break; /* This is a file path beginning with '/'. */

        switch (towupper(argv[i][1]))
        {
        case '?':
            WINE_ERR("\nSupported arguments: [ -e | -d ][ -n ]\n-e Enable nine\n-d Disable nine\n-n Do not call other arch exe\n");
            return TRUE;
        case 'E':
            NineSet = TRUE;
            break;
        case 'D':
            NineClear = TRUE;
            break;
        case 'N':
            NoOtherArch = TRUE;
            break;
        default:
            return FALSE;
        }
    }

    if (NineSet && !NineClear)
    {
        nine_set(TRUE, NoOtherArch);
        *result = nine_get();
        return TRUE;
    }
    else if (NineClear && !NineSet)
    {
        nine_set(FALSE, NoOtherArch);
        *result = !nine_get();
        return TRUE;
    }

    return FALSE;
}

static INT_PTR CALLBACK AppDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        load_staging_settings(hDlg);
        break;

    case WM_COMMAND:
        if (HIWORD(wParam) != BN_CLICKED) break;
        switch (LOWORD(wParam))
        {
        case IDC_ENABLE_NATIVE_D3D9:
            nine_set(IsDlgButtonChecked(hDlg, IDC_ENABLE_NATIVE_D3D9) == BST_UNCHECKED, TRUE);
            CheckDlgButton(hDlg, IDC_ENABLE_NATIVE_D3D9, nine_get() ? BST_CHECKED : BST_UNCHECKED);
            SendMessageW(GetParent(hDlg), PSM_CHANGED, 0, 0);
            return TRUE;
        }
        break;
    }

    return FALSE;
}

static INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_INITDIALOG:
        SetDlgItemTextA(hDlg, IDC_NINE_URL, "<a href=\"" NINE_URL "\">" NINE_URL "</a>");
        break;

    case WM_NOTIFY:
        switch (((LPNMHDR)lParam)->code)
        {
        case NM_CLICK:
        case NM_RETURN:
            if (wParam == IDC_NINE_URL)
              ShellExecuteA(NULL, "open", NINE_URL, NULL, NULL, SW_SHOW);

            break;
        }

        break;
    }

    return FALSE;
}

static INT_PTR
doPropertySheet (HINSTANCE hInstance, HWND hOwner)
{
    PROPSHEETPAGEW psp[2];
    PROPSHEETHEADERW psh;

    psp[0].dwSize = sizeof (PROPSHEETPAGEW);
    psp[0].dwFlags = PSP_USETITLE;
    psp[0].hInstance = hInstance;
    psp[0].pszTemplate = MAKEINTRESOURCEW (IDD_NINE);
    psp[0].pszIcon = NULL;
    psp[0].pfnDlgProc = AppDlgProc;
    psp[0].pszTitle = load_string (IDS_TAB_MAIN);
    psp[0].lParam = 0;

    psp[1].dwSize = sizeof (PROPSHEETPAGEW);
    psp[1].dwFlags = PSP_USETITLE;
    psp[1].hInstance = hInstance;
    psp[1].pszTemplate = MAKEINTRESOURCEW (IDD_ABOUT);
    psp[1].pszIcon = NULL;
    psp[1].pfnDlgProc = AboutDlgProc;
    psp[1].pszTitle = load_string (IDS_TAB_ABOUT);
    psp[1].lParam = 0;

    /*
     * Fill out the PROPSHEETHEADER
     */
    psh.dwSize = sizeof (PROPSHEETHEADERW);
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_USEICONID | PSH_USECALLBACK | PSH_NOAPPLYNOW;
    psh.hwndParent = hOwner;
    psh.hInstance = hInstance;
    psh.pszIcon = NULL;
    psh.pszCaption =  load_string (IDS_NINECFG_TITLE);
    psh.nPages = sizeof(psp) / sizeof(psp[0]);
    psh.ppsp = psp;
    psh.pfnCallback = NULL;
    psh.nStartPage = 0;

    /*
     * Display the modal property sheet
     */
    return PropertySheetW (&psh);
}

/*****************************************************************************
 * Name       : WinMain
 * Description: Main windows entry point
 * Parameters : hInstance
 *              hPrev
 *              szCmdLine
 *              nShow
 * Returns    : Program exit code
 */
int WINAPI
WinMain (HINSTANCE hInstance, HINSTANCE hPrev, LPSTR szCmdLine, int nShow)
{
    BOOL res = FALSE;

    if (ProcessCmdLine(GetCommandLineW(), &res))
    {
        if (!res)
            return 1;

        return 0;
    }

    /*
     * The next 9 lines should be all that is needed
     * for the Wine Configuration property sheet
     */
    InitCommonControls ();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (doPropertySheet (hInstance, NULL) > 0)
    {
        WINE_TRACE("OK\n");
    }
    else
    {
        WINE_TRACE("Cancel\n");
    }
    CoUninitialize();
    ExitProcess (0);

    return 0;
}
