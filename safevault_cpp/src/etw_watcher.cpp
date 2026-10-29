// etw_watcher.cpp – Windows ETW based file‑watcher for SafeVault
// This file replaces the ReadDirectoryChangesW implementation with a private
// real‑time ETW session that listens to the kernel‑file provider. It emits
// events that contain the originating process ID, which we forward to the
// existing journal infrastructure.

#include "etw_watcher.hpp"
#include "watch.hpp" // for WatchEvent, TsQueue
#include <windows.h>
#include <evntrace.h>
#include <tdh.h>
#include <cstdio>
#include <string>
#include <iostream>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

// ---------------------------------------------------------------------------
// Helper: extract a UTF‑16 string from an EVENT_RECORD property
static std::wstring get_wstring_property(const EVENT_RECORD* er,
                                         const EVENT_PROPERTY_INFO* propInfo)
{
    const BYTE* base = reinterpret_cast<const BYTE*>(er->UserData);
    const BYTE* ptr  = base + propInfo->Offset;
    return std::wstring(reinterpret_cast<const wchar_t*>(ptr));
}

// ---------------------------------------------------------------------------
// Map a kernel‑file ETW event to our own WatchEvent
static bool etw_to_watch_event(const EVENT_RECORD* er, WatchEvent& out)
{
    // Event IDs we care about (kernel‑file provider v1)
    //   0x00 = Create, 0x02 = Write, 0x03 = Delete, 0x05 = Rename (new name)
    static const std::unordered_map<USHORT, EventKind> id2kind = {
        {0x00, EventKind::Create},
        {0x02, EventKind::Modify},
        {0x03, EventKind::Delete},
        {0x05, EventKind::Rename}
    };

    auto it = id2kind.find(er->EventHeader.EventDescriptor.Id);
    if (it == id2kind.end())
        return false; // ignore unrelated IDs (e.g., Read)

    // Locate the "FileName" property in the event schema
    EVENT_RECORD_INFO* info = nullptr;
    USHORT infoSize = 0;
    // First call to get required size
    if (TdhGetEventInformation(const_cast<EVENT_RECORD*>(er), 0, nullptr, nullptr, &infoSize) != ERROR_INSUFFICIENT_BUFFER)
        return false;
    std::vector<BYTE> buf(infoSize);
    info = reinterpret_cast<EVENT_RECORD_INFO*>(buf.data());
    if (TdhGetEventInformation(const_cast<EVENT_RECORD*>(er), 0, nullptr, info, &infoSize) != ERROR_SUCCESS)
        return false;

    const EVENT_PROPERTY_INFO* fnameProp = nullptr;
    for (USHORT i = 0; i < info->TopLevelPropertyCount; ++i) {
        if (wcscmp(info->EventPropertyInfoArray[i].Name, L"FileName") == 0) {
            fnameProp = &info->EventPropertyInfoArray[i];
            break;
        }
    }
    if (!fnameProp)
        return false; // no filename => nothing to report

    std::wstring wpath = get_wstring_property(er, fnameProp);
    out.kind = it->second;
    out.path = std::filesystem::path(wpath);
    out.pid  = er->EventHeader.ProcessId; // real editor PID
    return true;
}

// ---------------------------------------------------------------------------
// TLS helpers – store pointers for the callback thread
static void SetTLS(LPCWSTR name, void* ptr)
{
    typedef void (WINAPI *SetTLSFp)(LPCWSTR, void*);
    static SetTLSFp Set = nullptr;
    if (!Set) {
        Set = reinterpret_cast<SetTLSFp>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadLocalStoragePointer"));
    }
    if (Set) Set(name, ptr);
}
static void* GetTLS(LPCWSTR name)
{
    typedef void* (WINAPI *GetTLSFp)(LPCWSTR);
    static GetTLSFp Get = nullptr;
    if (!Get) {
        Get = reinterpret_cast<GetTLSFp>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadLocalStoragePointer"));
    }
    return Get ? Get(name) : nullptr;
}

// ---------------------------------------------------------------------------
// Callback invoked for each ETW event (runs on the ETW worker thread)
static VOID WINAPI etw_callback(EVENT_RECORD* pEvent)
{
    static thread_local TsQueue<WatchEvent>* gQueue = nullptr;
    static thread_local std::filesystem::path* gRoot = nullptr;
    if (!gQueue) {
        gQueue = reinterpret_cast<TsQueue<WatchEvent>*>(GetTLS(L"ETW_QUEUE"));
        gRoot  = reinterpret_cast<std::filesystem::path*>(GetTLS(L"ETW_ROOT"));
    }
    if (!gQueue || !gRoot) return; // sanity

    WatchEvent ev;
    if (!etw_to_watch_event(pEvent, ev))
        return; // not a file‑op we care about

    // Ensure the path is absolute and normalized relative to the watched root
    try {
        ev.path = std::filesystem::canonical(*gRoot / ev.path);
    } catch (...) {
        // If the file is already gone (e.g., delete) canonicalisation fails.
        // Keep the raw path – it is still useful for journalling.
    }

    gQueue->push(std::move(ev));
}

// ---------------------------------------------------------------------------
// Public API – start the ETW watcher thread and block until stop_flag is set
void start_etw_watcher(const std::filesystem::path& root,
                       TsQueue<WatchEvent>& queue,
                       std::atomic<bool>& stop_flag)
{
    // Store pointers for the callback via TLS (the ETW processing thread
    // will retrieve them in its own TLS space).
    SetTLS(L"ETW_QUEUE", &queue);
    SetTLS(L"ETW_ROOT",  const_cast<std::filesystem::path*>(&root));

    // -------------------------------------------------------------------
    // 1. Create a private real‑time ETW session
    // -------------------------------------------------------------------
    const wchar_t* sessionName = L"SafeVaultETWSession";
    size_t nameLen = wcslen(sessionName) + 1;
    size_t propsSize = sizeof(EVENT_TRACE_PROPERTIES) + nameLen * sizeof(wchar_t);
    EVENT_TRACE_PROPERTIES* props = (EVENT_TRACE_PROPERTIES*)malloc(propsSize);
    ZeroMemory(props, propsSize);

    props->Wnode.BufferSize = (ULONG)propsSize;
    props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    props->Wnode.Guid = SystemTraceControlGuid;
    props->Wnode.ClientContext = 1; // QPC clock
    props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    props->MaximumBuffers = 64;
    props->BufferSize = 1024; // KB per buffer
    props->FlushTimer = 0;
    props->EnableFlags = EVENT_TRACE_FLAG_PROCESS;
    props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES) / sizeof(ULONG);

    TRACEHANDLE sessionHandle = 0;
    ULONG status = StartTraceW(&sessionHandle, sessionName, props);
    if (status != ERROR_SUCCESS) {
        std::cerr << "[ETW] StartTrace failed, error=" << status << "\n";
        free(props);
        return;
    }

    // -------------------------------------------------------------------
    // 2. Enable kernel‑file provider (GUID = EDD08927‑9CC4‑4E65‑A5C5‑0A5CE5E56E44)
    // -------------------------------------------------------------------
    const GUID kernelFileGuid = {0xEDD08927,0x9CC4,0x4E65,{0xA5,0xC5,0x0A,0x5C,0xE5,0xE5,0x6E,0x44}};
    status = EnableTraceEx2(sessionHandle,
                            &kernelFileGuid,
                            EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                            TRACE_LEVEL_INFORMATION,
                            0, // any keyword mask
                            0,
                            0,
                            nullptr);
    if (status != ERROR_SUCCESS) {
        std::cerr << "[ETW] EnableTraceEx2 failed, error=" << status << "\n";
        ControlTraceW(sessionHandle, sessionName, props, EVENT_TRACE_CONTROL_STOP);
        free(props);
        return;
    }

    // -------------------------------------------------------------------
    // 3. Open the trace for real‑time processing
    // -------------------------------------------------------------------
    EVENT_TRACE_LOGFILE trace = {0};
    trace.LoggerName = const_cast<LPWSTR>(sessionName);
    trace.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    trace.EventRecordCallback = (PEVENT_RECORD_CALLBACK)etw_callback;

    TRACEHANDLE traceHandle = OpenTraceW(&trace);
    if (traceHandle == (TRACEHANDLE)INVALID_HANDLE_VALUE) {
        std::cerr << "[ETW] OpenTrace failed\n";
        ControlTraceW(sessionHandle, sessionName, props, EVENT_TRACE_CONTROL_STOP);
        free(props);
        return;
    }

    // -------------------------------------------------------------------
    // 4. Run ProcessTrace in a background thread
    // -------------------------------------------------------------------
    std::thread etwThread([&]() {
        ULONG rc = ProcessTrace(&traceHandle, 1, nullptr, nullptr);
        if (rc != ERROR_SUCCESS && rc != ERROR_CANCELLED) {
            std::cerr << "[ETW] ProcessTrace returned " << rc << "\n";
        }
    });

    // -------------------------------------------------------------------
    // 5. Wait for the user‑initiated stop_flag, then clean up
    // -------------------------------------------------------------------
    while (!stop_flag.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Shut down the ETW session
    ControlTraceW(sessionHandle, sessionName, props, EVENT_TRACE_CONTROL_STOP);
    CloseTrace(traceHandle);
    if (etwThread.joinable())
        etwThread.join();

    free(props);
}
