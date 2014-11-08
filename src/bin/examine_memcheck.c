/* Examine - a tool for memory leak detection on Windows
 *
 * Copyright (C) 2014 Vincent Torri.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN

#ifdef _MSC_VER
# include <direct.h>
#endif

#include <examine_log.h>

#include "examine_private.h"


/*============================================================================*
 *                                  Local                                     *
 *============================================================================*/

#define CREATE_THREAD_ACCESS (PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_SUSPEND_RESUME | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ)

typedef HMODULE (*_load_library)(const char *);
typedef BOOL    (*_free_library)(HMODULE);

struct Exm_Map
{
    HANDLE handle;
    void *base;
};

typedef struct _Exm Exm;

struct _Exm
{
    _load_library  ll;
    _free_library  fl;

    char          *filename;
    char          *args;
    char          *dll_fullname;
    int            dll_length;

    struct
    {
        HANDLE     process;
        HANDLE     thread;
    } child;

    struct Exm_Map map_size;
    struct Exm_Map map_file;
    struct Exm_Map map_process;

    DWORD          exit_code; /* actually the base address of the mapped DLL */
};

static FARPROC
_exm_symbol_get(const char *module, const char *symbol)
{
    HMODULE  mod;
    FARPROC  proc;

    EXM_LOG_DBG("loading library %s", module);
    mod = LoadLibrary(module);
    if (!mod)
    {
        EXM_LOG_ERR("loading library %s failed", module);
        return NULL;
    }

    EXM_LOG_DBG("retrieving symbol %s", symbol);
    proc = GetProcAddress(mod, symbol);
    if (!proc)
    {
        EXM_LOG_ERR("retrieving symbol %s failed", symbol);
        goto free_library;
    }

    FreeLibrary(mod);

    return proc;

  free_library:
    FreeLibrary(mod);

    return NULL;
}

/*
 * check is the given filename is a PE file
 * result :
 * 2: DLL
 * 1: binary and not DLL
 * 0: not binary, nor DLL
 */
static unsigned char
_exm_file_is_pe(const char *filename)
{
    HANDLE file;
    HANDLE map;
    IMAGE_DOS_HEADER *dos_header;
    IMAGE_NT_HEADERS *nt_header;
    unsigned char res = 0;

    file = CreateFile(filename,
                      GENERIC_READ,
                      FILE_SHARE_READ,
                      NULL,
                      OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL,
                      NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;

    map = CreateFileMapping(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!map)
        goto close_file;

    dos_header = (IMAGE_DOS_HEADER *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
    if (!dos_header)
        goto close_map;

    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE)
        goto unmap_dos_header;

    nt_header = (IMAGE_NT_HEADERS *)((uintptr_t)dos_header + (uintptr_t)dos_header->e_lfanew);
    if (nt_header->Signature != IMAGE_NT_SIGNATURE)
        goto unmap_dos_header;

    if (nt_header->FileHeader.Characteristics & IMAGE_FILE_DLL)
        res = 2;
    else
        res = 1;

    UnmapViewOfFile(dos_header);
    CloseHandle(map);
    CloseHandle(file);

    return res;

  unmap_dos_header:
    UnmapViewOfFile(dos_header);
  close_map:
    CloseHandle(map);
  close_file:
    CloseHandle(file);

    return 0;
}

static Exm *
exm_new(char *filename, char *args)
{
#ifdef _MSC_VER
    char buf[MAX_PATH];
#endif
    Exm     *exm;
    HMODULE kernel32;
    char *iter;
    size_t  l1;
    size_t  l2;

    /* Check if CreateRemoteThread() is available. */
    /* MSDN suggests to check the availability of a */
    /* function instead of checking the Windows version. */

    kernel32 = LoadLibrary("kernel32.dll");
    if (!kernel32)
    {
        EXM_LOG_ERR("no kernel32.dll found");
        return NULL;
    }

    if (!GetProcAddress(kernel32, "CreateRemoteThread"))
    {
        EXM_LOG_ERR("no CreateRemoteThread() found");
        goto free_kernel32;
    }

    exm = (Exm *)calloc(1, sizeof(Exm));
    if (!exm)
        goto free_kernel32;

    exm->filename = filename;

    if (_exm_file_is_pe(exm->filename) != 1)
    {
        EXM_LOG_ERR("%s is not a valid binary", exm->filename);
        goto free_filename;
    }

    /* '/' replaced by '\' */
    iter = exm->filename;
    while (*iter)
    {
        if (*iter == '/') *iter = '\\';
        iter++;
    }

    exm->args = args;

    exm->ll = (_load_library)_exm_symbol_get("kernel32.dll", "LoadLibraryA");
    if (!exm->ll)
        goto free_args;

    exm->fl = (_free_library)_exm_symbol_get("kernel32.dll", "FreeLibrary");
    if (!exm->fl)
        goto free_exm;

#ifdef _MSC_VER
    _getcwd(buf, MAX_PATH);
    l1 = strlen(buf);
#else
    l1 = strlen(PACKAGE_BIN_DIR);
#endif
    l2 = strlen("/examine_dll.dll");
    exm->dll_fullname = malloc(sizeof(char) * (l1 + l2 + 1));
    if (!exm->dll_fullname)
        goto free_exm;
#ifdef _MSC_VER
    _getcwd(buf, MAX_PATH);
    memcpy(exm->dll_fullname, buf, l1);
#else
    memcpy(exm->dll_fullname, PACKAGE_BIN_DIR, l1);
#endif
    memcpy(exm->dll_fullname + l1, "/examine_dll.dll", l2);
    exm->dll_fullname[l1 + l2] = '\0';

    if (_exm_file_is_pe(exm->dll_fullname) != 2)
    {
        EXM_LOG_ERR("%s is not a valid DLL", exm->dll_fullname);
        goto free_exm;
    }

    exm->dll_length = l1 + l2 + 1;

    EXM_LOG_DBG("DLL to inject: %s", exm->dll_fullname);

    FreeLibrary(kernel32);

    return exm;

  free_args:
    free(exm->args);
  free_filename:
    free(exm->filename);
  free_exm:
    free(exm);
  free_kernel32:
    FreeLibrary(kernel32);

    return NULL;
}

static void
exm_del(Exm *exm)
{
    if (exm->child.thread)
        CloseHandle(exm->child.thread);
    if (exm->child.process)
        CloseHandle(exm->child.process);
    free(exm->dll_fullname);
    free(exm->args);
    free(exm->filename);
    free(exm);
}

static int
exm_file_map(Exm *exm)
{
    int length;

    length = strlen(exm->filename) + 1;

    exm->map_size.handle = CreateFileMapping(INVALID_HANDLE_VALUE,
                                             NULL, PAGE_READWRITE, 0, sizeof(int),
                                             "shared_size");
    if (!exm->map_size.handle)
        return 0;

    exm->map_size.base = MapViewOfFile(exm->map_size.handle, FILE_MAP_WRITE,
                                       0, 0, sizeof(int));
    if (!exm->map_size.base)
        goto close_size_mapping;

    CopyMemory(exm->map_size.base, &length, sizeof(int));

    exm->map_file.handle = CreateFileMapping(INVALID_HANDLE_VALUE,
                                             NULL, PAGE_READWRITE, 0, length,
                                             "shared_filename");
    if (!exm->map_file.handle)
        goto unmap_size_base;
    exm->map_file.base = MapViewOfFile(exm->map_file.handle, FILE_MAP_WRITE,
                                       0, 0, length);
    if (!exm->map_file.base)
        goto close_file_mapping;
    CopyMemory(exm->map_file.base, exm->filename, length);

    return 1;

  close_file_mapping:
    CloseHandle(exm->map_file.handle);
  unmap_size_base:
    UnmapViewOfFile(exm->map_size.base);
  close_size_mapping:
    CloseHandle(exm->map_size.handle);

    return 0;
}

static void
exm_file_unmap(Exm *exm)
{
    UnmapViewOfFile(exm->map_file.base);
    CloseHandle(exm->map_file.handle);
    UnmapViewOfFile(exm->map_size.base);
    CloseHandle(exm->map_size.handle);
}

static int
exm_dll_inject(Exm *exm)
{
    STARTUPINFO         si;
    PROCESS_INFORMATION pi;
    HANDLE              remote_thread;
    LPVOID              remote_string;
    DWORD               exit_code; /* actually the base address of the mapped DLL */

    ZeroMemory(&pi, sizeof(PROCESS_INFORMATION));
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.cb = sizeof(STARTUPINFO);

    EXM_LOG_DBG("creating child process %s", exm->filename);
    if (!CreateProcess(NULL, exm->filename, NULL, NULL, TRUE,
                       CREATE_SUSPENDED, NULL, NULL, &si, &pi))
    {
        EXM_LOG_ERR("creation of child process %s failed", exm->filename);
        return 0;
    }

    EXM_LOG_DBG("waiting for the child process %s to initialize", exm->filename);
    if (!WaitForInputIdle(pi.hProcess, INFINITE))
    {
        EXM_LOG_ERR("wait for the child process %s failed", exm->filename);
        goto close_handles;
    }

    EXM_LOG_DBG("mapping process handle 0x%p", pi.hProcess);
    exm->map_process.handle = CreateFileMapping(INVALID_HANDLE_VALUE,
                                                NULL, PAGE_READWRITE, 0, sizeof(HANDLE),
                                                "shared_process_handle");
    if (!exm->map_process.handle)
    {
        EXM_LOG_ERR("mapping process handle 0x%p failed", pi.hProcess);
        goto close_handles;
    }

    exm->map_process.base = MapViewOfFile(exm->map_process.handle, FILE_MAP_WRITE,
                                          0, 0, sizeof(HANDLE));
    if (!exm->map_process.base)
    {
        EXM_LOG_ERR("viewing map file handle 0x%p failed", exm->map_process.handle);
        goto close_process_handle;
    }

    CopyMemory(exm->map_process.base, &pi.hProcess, sizeof(HANDLE));

    EXM_LOG_DBG("allocating virtual memory of process 0x%p (%d bytes)", pi.hProcess, exm->dll_length);
    remote_string = VirtualAllocEx(pi.hProcess, NULL, exm->dll_length, MEM_COMMIT, PAGE_READWRITE);
    if (!remote_string)
    {
        EXM_LOG_ERR("allocating virtual memory of process 0x%p (%d bytes) failed", pi.hProcess, exm->dll_length);
        goto unmap_process_handle;
    }

    EXM_LOG_DBG("writing process %p in virtual memory", pi.hProcess);
    if (!WriteProcessMemory(pi.hProcess, remote_string, exm->dll_fullname, exm->dll_length, NULL))
    {
        EXM_LOG_ERR("writing process %p in virtual memory failed", pi.hProcess);
        goto virtual_free;
    }

    EXM_LOG_DBG("execute thread 0x%p", pi.hProcess);
    remote_thread = CreateRemoteThread(pi.hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)exm->ll, remote_string, 0, NULL);
    if (!remote_thread)
    {
        EXM_LOG_ERR("execute thread 0x%p failed", pi.hProcess);
        goto virtual_free;
    }

    WaitForSingleObject(remote_thread, INFINITE);

    EXM_LOG_DBG("getting exit code of thread 0x%p", remote_thread);
    if (!GetExitCodeThread(remote_thread, &exit_code))
    {
        EXM_LOG_ERR("getting exit code of thread 0x%p failed", remote_thread);
        goto close_thread;
    }

    CloseHandle(remote_thread);
    VirtualFreeEx(pi.hProcess, remote_string, 0, MEM_RELEASE);

    EXM_LOG_DBG("resume child process threas 0x%p", pi.hThread);
    ResumeThread(pi.hThread);

    exm->child.process = pi.hProcess;
    exm->child.thread = pi.hThread;
    exm->exit_code = exit_code;

    return 1;

  close_thread:
    CloseHandle(remote_thread);
  virtual_free:
    VirtualFreeEx(pi.hProcess, remote_string, 0, MEM_RELEASE);
  unmap_process_handle:
    UnmapViewOfFile(exm->map_process.base);
  close_process_handle:
    CloseHandle(exm->map_process.handle);
  close_handles:
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return 0;
}

static void
exm_dll_eject(Exm *exm)
{
    HANDLE thread;

    thread = CreateRemoteThread(exm->child.process, NULL, 0,
                                (LPTHREAD_START_ROUTINE)exm->fl,
                                (void*)(uintptr_t)exm->exit_code, 0, NULL );
    WaitForSingleObject(thread, INFINITE );
    CloseHandle(thread);
    UnmapViewOfFile(exm->map_process.base);
    CloseHandle(exm->map_process.handle);
}


/*============================================================================*
 *                                 Global                                     *
 *============================================================================*/


void
examine_memcheck_run(char *filename, char *args)
{
    Exm *exm;

    EXM_LOG_INFO("Examine, a memory leak detector");
    EXM_LOG_INFO("Copyright (c) 2013-2014, and GNU GPL2'd, by Vincent Torri");
    EXM_LOG_INFO("Options: --tool=memcheck");

    exm = exm_new(filename, args);
    if (!exm)
        return;

    EXM_LOG_INFO("Command: %s %s", filename, args);

    if (!exm_file_map(exm))
    {
        EXM_LOG_ERR("impossible to map filename %s", filename);
        goto del_exm;
    }

    if (!exm_dll_inject(exm))
    {
        EXM_LOG_ERR("injection failed");
        goto unmap_exm;
    }

    WaitForSingleObject(exm->child.process, INFINITE);

    EXM_LOG_DBG("end of process");

    exm_dll_eject(exm);

    exm_file_unmap(exm);
    exm_del(exm);
    EXM_LOG_DBG("resources freed");

    return;

  unmap_exm:
    exm_file_unmap(exm);
  del_exm:
    exm_del(exm);
}
