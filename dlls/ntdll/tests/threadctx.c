/*
 * Unit test suite for ntdll thread context behavior
 *
 * Copyright 2026 Hoshino Lina
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

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/test.h"

/* 90 second max runtime, to avoid winetest timeouts. */
#define MAX_RUNTIME 90
#define THREADS 50
#define STACK_SIZE 0x10000
#define LOOPS 2000

#ifdef __x86_64__
#define CTX_SP Rsp
#define CTX_IP Rip
#endif
#ifdef __i386__
#define CTX_SP Esp
#define CTX_IP Eip
#endif
#ifdef __arm__
#define CTX_SP Sp
#define CTX_IP Pc
#endif
#ifdef __aarch64__
#define CTX_SP Sp
#define CTX_IP Pc
#endif

static volatile bool looping = true;

struct thread
{
    DWORD tid;
    bool stopped;
    HANDLE hnd;
    HANDLE ready;
    CONTEXT ctx;
    DWORD64 stack_lo;
    DWORD64 stack_hi;
    NTSTATUS err;
    bool complete;
};

struct thread t[THREADS];

static HMODULE hntdll;
static HMODULE hkernel32;
static NTSTATUS (WINAPI *pNtGetContextThread)(HANDLE, CONTEXT *);
static NTSTATUS (WINAPI *pNtReadVirtualMemory)(HANDLE, const void *, void *, SIZE_T, SIZE_T *);
static NTSTATUS (WINAPI *pNtSetContextThread)(HANDLE, const CONTEXT *);
static void (WINAPI *pGetCurrentThreadStackLimits)(PULONG_PTR, PULONG_PTR);

static BOOL init_functions(void)
{
    hntdll = GetModuleHandleA( "ntdll.dll" );
    hkernel32 = GetModuleHandleA( "kernel32.dll" );

#define X(f) p##f = (void *)GetProcAddress( hntdll, #f )
    X(NtGetContextThread);
    X(NtReadVirtualMemory);
    X(NtSetContextThread);
#undef X
#define X(f) p##f = (void *)GetProcAddress( hkernel32, #f )
    X(GetCurrentThreadStackLimits);
#undef X

    if (!pNtGetContextThread || !pNtReadVirtualMemory || !pNtSetContextThread ||
        !pGetCurrentThreadStackLimits)
    {
        win_skip( "Required thread context functions are not available.\n" );
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI thread_func(void *param)
{
    struct thread *self = param;
    ULONG_PTR lo, hi;

    pGetCurrentThreadStackLimits( &lo, &hi );

    self->stack_lo = (DWORD64)lo;
    self->stack_hi = (DWORD64)hi;

    SetEvent( self->ready );

    while (looping)
    {
        CONTEXT c;
        NTSTATUS status;

        /* Play with the FP context. These exercise the FP restore syscall
         * path, which was seen to tickle a particular bug.
         */
        c.ContextFlags = CONTEXT_FLOATING_POINT;
        status = pNtGetContextThread( GetCurrentThread(), &c );
        if (status)
            ok( !status, "Failed to get context: %#lx\n", status );
        c.ContextFlags = CONTEXT_FLOATING_POINT;
        status = pNtSetContextThread( GetCurrentThread(), &c );
        if (status) ok( !status, "Failed to set context: %#lx\n", status );
    }

    self->complete = true;
    CloseHandle( self->ready );

    return 0;
}

static bool is_pe_map(DWORD64 addr)
{
    /* This check only makes sense on Wine, where fake kernel mode exists. */
    if (!winetest_platform_is_wine)
        return true;

    /* See server/mapping.c for the upper limits. */
    return (addr >= 0x60000000 && addr < 0x7c000000) ||
           (addr >= 0x600000000000 && addr < 0x700000000000) ||
#ifdef _WIN64
           (addr >= 0x140000000 && addr < 0x141000000);
#else
           (addr >= 0x400000 && addr < 0x1400000);
#endif
}

struct remote_context_params
{
    HANDLE target;
    HANDLE ready;
    CONTEXT context;
};

static void run_child_test(const char *name);

static DWORD WINAPI remote_context_thread(void *param)
{
    struct remote_context_params *params = param;
    CONTEXT context;
    NTSTATUS status;

    status = pNtSetContextThread( params->target, &params->context );
    ok( !status, "NtSetContextThread failed: %#lx.\n", status );
    SetEvent( params->ready );

    while (looping)
    {
        context.ContextFlags = CONTEXT_CONTROL;
        pNtGetContextThread( GetCurrentThread(), &context );
    }
    return 0;
}

static DWORD WINAPI remote_target_thread(void *param)
{
    (void)param;
    while (looping) Sleep( 0 );
    return 0;
}

static void remote_context_child(void)
{
    struct remote_context_params params;
    CONTEXT context;
    HANDLE caller, target;
    DWORD ret, wait;

    target = CreateThread( NULL, 0, remote_target_thread, NULL, 0, NULL );
    ok( !!target, "CreateThread failed: %lu.\n", GetLastError() );
    if (!target) return;

    ret = SuspendThread( target );
    ok( ret != ~0u, "SuspendThread failed: %lu.\n", GetLastError() );
    if (ret == ~0u)
    {
        looping = false;
        WaitForSingleObject( target, 5000 );
        CloseHandle( target );
        return;
    }
    params.context.ContextFlags = CONTEXT_FULL;
    ok( GetThreadContext( target, &params.context ), "GetThreadContext failed: %lu.\n", GetLastError() );
    params.target = target;
    params.ready = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!params.ready, "CreateEvent failed: %lu.\n", GetLastError() );

    caller = CreateThread( NULL, 0, remote_context_thread, &params, 0, NULL );
    ok( !!caller, "CreateThread failed: %lu.\n", GetLastError() );
    wait = params.ready ? WaitForSingleObject( params.ready, 5000 ) : WAIT_FAILED;
    ok( wait == WAIT_OBJECT_0, "Context-setting thread did not become ready.\n" );
    if (caller && wait == WAIT_OBJECT_0)
    {
        ret = SuspendThread( caller );
        ok( ret != ~0u, "SuspendThread failed: %lu.\n", GetLastError() );
        context.ContextFlags = CONTEXT_CONTROL;
        ok( GetThreadContext( caller, &context ), "GetThreadContext failed: %lu.\n", GetLastError() );
        ResumeThread( caller );
    }

    ResumeThread( target );
    looping = false;
    if (caller) WaitForSingleObject( caller, 5000 );
    WaitForSingleObject( target, 5000 );
    if (caller) CloseHandle( caller );
    if (params.ready) CloseHandle( params.ready );
    CloseHandle( target );
}

static void test_remote_context(void)
{
    run_child_test( "remote-context" );
}

struct fault_context_params
{
    CONTEXT *context;
    HANDLE ready;
    NTSTATUS status;
    NTSTATUS followup_status;
    BOOL set_context;
};

static DWORD WINAPI fault_context_thread(void *param)
{
    struct fault_context_params *params = param;
    CONTEXT context = { .ContextFlags = CONTEXT_CONTROL };

    if (params->set_context)
        params->status = pNtSetContextThread( GetCurrentThread(), params->context );
    else
        params->status = pNtGetContextThread( GetCurrentThread(), params->context );
    params->followup_status = pNtGetContextThread( GetCurrentThread(), &context );
    SetEvent( params->ready );
    while (looping) Sleep( 0 );
    return 0;
}

static void fault_context_child(BOOL set_context)
{
    struct fault_context_params params = { .set_context = set_context };
    SYSTEM_INFO info;
    CONTEXT context;
    HANDLE thread;
    BYTE *mem;
    DWORD old_protect, ret, wait;
    unsigned int i;

    GetSystemInfo( &info );
    mem = VirtualAlloc( NULL, 2 * info.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    ok( !!mem, "VirtualAlloc failed: %lu.\n", GetLastError() );
    if (!mem) return;
    ret = VirtualProtect( mem + info.dwPageSize, info.dwPageSize, PAGE_NOACCESS, &old_protect );
    ok( ret, "VirtualProtect failed: %lu.\n", GetLastError() );
    if (!ret)
    {
        VirtualFree( mem, 0, MEM_RELEASE );
        return;
    }

    params.context = (CONTEXT *)(mem + info.dwPageSize - FIELD_OFFSET(CONTEXT, CTX_SP));
    params.context->ContextFlags = CONTEXT_CONTROL;
    params.ready = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!params.ready, "CreateEvent failed: %lu.\n", GetLastError() );
    if (!params.ready)
    {
        VirtualFree( mem, 0, MEM_RELEASE );
        return;
    }

    thread = CreateThread( NULL, 0, fault_context_thread, &params, 0, NULL );
    ok( !!thread, "CreateThread failed: %lu.\n", GetLastError() );
    if (!thread)
    {
        CloseHandle( params.ready );
        VirtualFree( mem, 0, MEM_RELEASE );
        return;
    }

    wait = WaitForSingleObject( params.ready, 5000 );
    ok( wait == WAIT_OBJECT_0, "Context fault thread did not become ready.\n" );
    if (wait == WAIT_OBJECT_0)
    {
        ok( params.status == STATUS_ACCESS_VIOLATION, "%s returned %#lx.\n",
            set_context ? "NtSetContextThread" : "NtGetContextThread", params.status );
        ok( !params.followup_status, "Follow-up NtGetContextThread failed: %#lx.\n",
            params.followup_status );

        for (i = 0; i < 8; ++i)
        {
            ret = SuspendThread( thread );
            ok( ret != ~0u, "SuspendThread failed: %lu.\n", GetLastError() );
            if (ret == ~0u) break;
            context.ContextFlags = CONTEXT_CONTROL;
            ok( GetThreadContext( thread, &context ), "GetThreadContext failed: %lu.\n",
                GetLastError() );
            ret = ResumeThread( thread );
            ok( ret != ~0u, "ResumeThread failed: %lu.\n", GetLastError() );
        }
    }

    looping = false;
    wait = WaitForSingleObject( thread, 5000 );
    ok( wait == WAIT_OBJECT_0, "Context fault thread did not exit.\n" );
    CloseHandle( thread );
    CloseHandle( params.ready );
    VirtualFree( mem, 0, MEM_RELEASE );
}

struct guarded_copy_params
{
    void *src;
    void *dst;
    SIZE_T size;
    SIZE_T read;
    HANDLE ready;
    NTSTATUS status;
};

static DWORD WINAPI guarded_copy_thread(void *param)
{
    struct guarded_copy_params *params = param;
    SetEvent( params->ready );
    while (looping)
    {
        params->status = pNtReadVirtualMemory( GetCurrentProcess(), params->src, params->dst,
                                              params->size, &params->read );
        if (params->status || params->read != params->size) break;
    }
    return 0;
}

static void guarded_suspend_child(void)
{
    struct guarded_copy_params params = { .size = 32 * 1024 * 1024 };
    CONTEXT context;
    HANDLE thread;
    DWORD ret, wait;
    unsigned int i;

    params.src = VirtualAlloc( NULL, params.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    params.dst = VirtualAlloc( NULL, params.size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE );
    params.ready = CreateEventW( NULL, TRUE, FALSE, NULL );
    ok( !!params.src && !!params.dst && !!params.ready, "Failed to create guarded-copy resources.\n" );
    if (!params.src || !params.dst || !params.ready) goto done;

    thread = CreateThread( NULL, 0, guarded_copy_thread, &params, 0, NULL );
    ok( !!thread, "CreateThread failed: %lu.\n", GetLastError() );
    if (!thread) goto done;
    wait = WaitForSingleObject( params.ready, 5000 );
    ok( wait == WAIT_OBJECT_0, "Guarded-copy thread did not become ready.\n" );

    for (i = 0; i < 256 && wait == WAIT_OBJECT_0; ++i)
    {
        ret = SuspendThread( thread );
        ok( ret != ~0u, "SuspendThread failed: %lu.\n", GetLastError() );
        if (ret == ~0u) break;
        context.ContextFlags = CONTEXT_CONTROL;
        ok( GetThreadContext( thread, &context ), "GetThreadContext failed: %lu.\n", GetLastError() );
        ret = ResumeThread( thread );
        ok( ret != ~0u, "ResumeThread failed: %lu.\n", GetLastError() );
    }

    looping = false;
    wait = WaitForSingleObject( thread, 5000 );
    ok( wait == WAIT_OBJECT_0, "Guarded-copy thread did not exit.\n" );
    ok( !params.status && params.read == params.size,
        "NtReadVirtualMemory returned %#lx, read %Iu bytes.\n", params.status, params.read );
    CloseHandle( thread );

done:
    if (params.ready) CloseHandle( params.ready );
    if (params.dst) VirtualFree( params.dst, 0, MEM_RELEASE );
    if (params.src) VirtualFree( params.src, 0, MEM_RELEASE );
}

static void run_child_test(const char *name)
{
    char cmdline[2 * MAX_PATH];
    PROCESS_INFORMATION pi;
    STARTUPINFOA si = { .cb = sizeof(si) };
    DWORD exit_code, wait;
    char **argv;

    winetest_get_mainargs( &argv );
    sprintf( cmdline, "\"%s\" %s %s", argv[0], argv[1], name );
    if (!CreateProcessA( NULL, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ))
    {
        ok( 0, "CreateProcess failed: %lu.\n", GetLastError() );
        return;
    }

    wait = WaitForSingleObject( pi.hProcess, 10000 );
    ok( wait == WAIT_OBJECT_0, "%s child timed out.\n", name );
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess( pi.hProcess, 1 );
        WaitForSingleObject( pi.hProcess, 5000 );
    }
    GetExitCodeProcess( pi.hProcess, &exit_code );
    ok( !exit_code, "%s child exited with %lu.\n", name, exit_code );
    CloseHandle( pi.hThread );
    CloseHandle( pi.hProcess );
}

static void test_context_sp(void)
{
    int i, loop, thread_count = 0;
    bool failed_sp = false;
    bool failed_ip = false;
    DWORD timeout = GetTickCount() + MAX_RUNTIME * 1000;
    trace( "Starting %d threads\n", THREADS );
    for (i = 0; i < THREADS; i++)
    {
        t[i].ready = CreateEventA( NULL, TRUE, FALSE, NULL );
        ok( !!t[i].ready, "Failed to create event\n" );
        if (!t[i].ready)
        {
            looping = false;
            break;
        }

        t[i].hnd = CreateThread( NULL, STACK_SIZE, thread_func, &t[i], 0, &t[i].tid );
        ok( !!t[i].hnd, "Failed to create thread\n" );
        if (!t[i].hnd)
        {
            looping = false;
            break;
        }

        WaitForSingleObject( t[i].ready, INFINITE );
        thread_count++;
    }
    trace( "Started %d threads\n", thread_count );

    trace( "Starting %d loops of thread context fetching\n", LOOPS );

    for (loop = 0; looping && loop < LOOPS && GetTickCount() < timeout; loop++)
    {
        for (i = 0; i < thread_count; i++)
        {
            if (SuspendThread( t[i].hnd ) != ~0u)
            {
                t[i].ctx.ContextFlags = CONTEXT_INTEGER | CONTEXT_CONTROL;

                if (GetThreadContext( t[i].hnd, &t[i].ctx ))
                {
                    bool in_sp_range = t[i].ctx.CTX_SP > t[i].stack_lo &&
                                       t[i].ctx.CTX_SP <= t[i].stack_hi;
                    bool in_ip_range = is_pe_map( t[i].ctx.CTX_IP );

                    if (!in_sp_range)
                    {
                        trace( "[%d/%d:%d] SP=0x%llx [%llx..%llx]\n", loop,
                              LOOPS, i, (long long)t[i].ctx.CTX_SP,
                              (long long)t[i].stack_lo,
                              (long long)t[i].stack_hi);
                        failed_sp = true;
                    }

                    if (!in_ip_range)
                    {
                        trace( "[%d/%d:%d] IP=0x%llx\n", loop, LOOPS, i,
                              (long long)t[i].ctx.CTX_IP);
                        failed_ip = true;
                    }

                    t[i].stopped = true;
                }
                else
                {
                    ResumeThread( t[i].hnd );
                }
            }
        }

        for (i = 0; i < thread_count; i++)
        {
            if (t[i].stopped) ResumeThread( t[i].hnd );

            t[i].stopped = false;
        }

        if (failed_sp && failed_ip) break;
    }

    trace( "Completed %d/%d loops\n", loop, LOOPS );

    looping = false;

    for (i = 0; i < thread_count; i++) WaitForSingleObject( t[i].hnd, INFINITE );

    ok( !failed_sp, "Invalid SP value detected\n" );
    ok( !failed_ip, "Invalid IP value detected\n" );

    for (i = 0; i < thread_count; i++)
    {
        ok( t[i].complete, "Thread %d died unexpectedly\n", i );
        CloseHandle( t[i].hnd );
    }
}

START_TEST(threadctx)
{
    char **argv;
    int argc;

    if (!init_functions()) return;
    argc = winetest_get_mainargs( &argv );
    if (argc >= 3 && !strcmp( argv[2], "remote-context" ))
    {
        remote_context_child();
        return;
    }
    if (argc >= 3 && !strcmp( argv[2], "fault-get-context" ))
    {
        fault_context_child( FALSE );
        return;
    }
    if (argc >= 3 && !strcmp( argv[2], "fault-set-context" ))
    {
        fault_context_child( TRUE );
        return;
    }
    if (argc >= 3 && !strcmp( argv[2], "guarded-suspend" ))
    {
        guarded_suspend_child();
        return;
    }

    test_remote_context();
    run_child_test( "fault-get-context" );
    run_child_test( "fault-set-context" );
    run_child_test( "guarded-suspend" );
    test_context_sp();
}
