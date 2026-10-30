// ==WindhawkMod==
// @id              taskview-restore-preparer
// @name            Task View Pre-Restore
// @name:zh-CN      任务视图预还原
// @description     Pre-restore minimized windows before Task View switches to them, preserving the Shell transition.
// @version         0.9.2
// @author          Local compatibility patch
// @include         explorer.exe
// @compilerOptions -luser32 -ldwmapi -ladvapi32 -fno-exceptions -fno-rtti -nostdlib++
// ==/WindhawkMod==

#include <windows.h>
#include <dwmapi.h>
#include <unknwn.h>
#include <sddl.h>
#include <atomic>
#include <cstring>
#include <cstddef>

namespace {
enum Command : DWORD { Prepare=1, Complete=2, Cancel=3 };
// Fixed-width wire fields keep the protocol identical across x64 and x86.
struct alignas(8) Request {
    DWORD magic, command;
    ULONGLONG hwnd;
    DWORD pid, reserved;
    ULONGLONG sequence, acknowledge;
};
static_assert(sizeof(Request)==40 && offsetof(Request,hwnd)==8 &&
              offsetof(Request,sequence)==24 && offsetof(Request,acknowledge)==32);
constexpr DWORD kMagic=0x32525654;
void FormatPipeName(wchar_t (&name)[80],DWORD pid) {
    wsprintfW(name,L"\\\\.\\pipe\\Windhawk.TaskViewRestore.%lu",pid);
}
bool IsWindowOf(HWND hwnd,DWORD expected) {
    DWORD actual=0;
    return GetWindowThreadProcessId(hwnd,&actual) && actual==expected;
}
bool SetTransitions(HWND hwnd,BOOL disabled) {
    return SUCCEEDED(DwmSetWindowAttribute(hwnd,DWMWA_TRANSITIONS_FORCEDISABLED,&disabled,sizeof(disabled)));
}
void RestoreWindow(HWND hwnd,const WINDOWPLACEMENT& before) {
    WINDOWPLACEMENT placement=before;
    placement.flags|=WPF_ASYNCWINDOWPLACEMENT;
    SetWindowPlacement(hwnd,&placement);
}
#ifdef _WIN64
bool isShell=false;
#endif
} // namespace

#ifdef _WIN64
namespace Shell {
namespace {
constexpr DWORD kInvoke = 0x782000, kResolve = 0x76f700;
constexpr DWORD kSettle = 32, kTimeout = 250;
constexpr UINT kCleanup = WM_APP + 0x541;
constexpr wchar_t kClass[] = L"Windhawk.TaskViewRestoreCore";
using Invoke = void(__cdecl*)(IUnknown*, IUnknown*);
using Resolve = void*(__cdecl*)(IUnknown**, IUnknown*);
Invoke original;
Resolve resolve;
HMODULE module;
std::atomic<HWND> dispatcher{nullptr};
std::atomic<bool> stopping{false};

ULONGLONG requestCounter;
HANDLE Send(DWORD command, HWND hwnd, DWORD pid, ULONGLONG sequence, HANDLE acknowledge) {
    wchar_t name[80];
    FormatPipeName(name,pid);
    HANDLE pipe=CreateFileW(name,GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(pipe==INVALID_HANDLE_VALUE) {return nullptr;}
    ULONG server=0;DWORD mode=PIPE_NOWAIT,bytes=0;
    Request request{kMagic,command,reinterpret_cast<ULONG_PTR>(hwnd),pid,0,
                    sequence,reinterpret_cast<ULONG_PTR>(acknowledge)};
    if(!GetNamedPipeServerProcessId(pipe,&server) || server!=pid ||
       !SetNamedPipeHandleState(pipe,&mode,nullptr,nullptr) ||
       !WriteFile(pipe,&request,sizeof(request),&bytes,nullptr) || bytes!=sizeof(request)) {
        CloseHandle(pipe);return nullptr;
    }
    return pipe;
}
void SendCancel(HWND hwnd,DWORD pid,ULONGLONG sequence) {
    if(HANDLE pipe=Send(Cancel,hwnd,pid,sequence,nullptr)) CloseHandle(pipe);
}
enum Stage : BYTE { Local, Remote, Completing, Cancelling };
struct Operation {
    IUnknown* owner = nullptr;
    IUnknown* item = nullptr;
    HWND hwnd = nullptr, foreground = nullptr;
    DWORD pid = 0, started = 0, ready = 0;
    WINDOWPLACEMENT before{sizeof(WINDOWPLACEMENT)};
    bool rollback = false, transitionsDisabled = false;
    Stage stage = Local;
    HANDLE pipe = nullptr, finishAcknowledge = nullptr;
    ULONGLONG sequence = 0;
} pending;
void EndNow(bool invoke, bool undo) {
    KillTimer(dispatcher,1);
    Operation op=pending;pending={};
    if(op.stage!=Local) {
        if(undo) SendCancel(op.hwnd,op.pid,op.sequence);
        if(op.pipe) CloseHandle(op.pipe);
        if(op.finishAcknowledge) CloseHandle(op.finishAcknowledge);
    }
    if(IsWindowOf(op.hwnd,op.pid)) {
        if(undo && op.stage==Local) RestoreWindow(op.hwnd,op.before);
        if(op.transitionsDisabled) SetTransitions(op.hwnd,FALSE);
    }
    if(invoke && !stopping.load(std::memory_order_relaxed)) original(op.owner,op.item);
    op.item->Release();op.owner->Release();
}
void Finish(bool invoke,bool undo) {
    if(invoke && pending.stage!=Local && !stopping.load(std::memory_order_relaxed)) {
        if(pending.pipe) CloseHandle(pending.pipe);
        pending.finishAcknowledge=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!pending.finishAcknowledge) {pending.pipe=nullptr;EndNow(true,true);return;}
        pending.stage=undo ? Cancelling : Completing;
        pending.started=GetTickCount();
        pending.pipe=Send(undo ? Cancel : Complete,pending.hwnd,pending.pid,pending.sequence,pending.finishAcknowledge);
        return;
    }
    EndNow(invoke,undo);
}
void Tick() {
    if(!pending.owner) return;
    if(stopping.load(std::memory_order_relaxed)) {EndNow(false,true);return;}
    if(!IsWindowOf(pending.hwnd,pending.pid)) {EndNow(true,true);return;}
    DWORD now=GetTickCount();
    if(pending.stage>=Completing) {
        if(!pending.pipe) pending.pipe=Send(pending.stage==Cancelling ? Cancel : Complete,
            pending.hwnd,pending.pid,pending.sequence,pending.finishAcknowledge);
        if(WaitForSingleObject(pending.finishAcknowledge,0)==WAIT_OBJECT_0) {
            if(pending.stage==Cancelling) {pending.rollback=true;pending.stage=Remote;}
            else {EndNow(true,false);return;}
        }
        if(now-pending.started>=kTimeout) {EndNow(true,true);return;}
        if(pending.stage>=Completing) return;
    }
    if(pending.rollback) {
        if(IsIconic(pending.hwnd) || now-pending.started>=kTimeout) EndNow(true,false);
        return;
    }
    HWND foreground=GetForegroundWindow();
    if(pending.foreground && foreground!=pending.foreground && foreground!=pending.hwnd) {
        Finish(false,true);return;
    }
    if(!IsIconic(pending.hwnd)) {
        if((pending.before.flags&WPF_RESTORETOMAXIMIZED) && !IsZoomed(pending.hwnd)) {
            if(pending.stage!=Local) {Finish(true,true);return;}
            pending.rollback=true;pending.started=now;
            RestoreWindow(pending.hwnd,pending.before);return;
        }
        if(!pending.ready) pending.ready=now;
        if(now-pending.ready>=kSettle) {Finish(true,false);return;}
    }
    if(now-pending.started>=kTimeout) Finish(true,pending.stage!=Local);
}
LRESULT CALLBACK Dispatch(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER && wp == 1) { Tick(); return 0; }
    if (message == kCleanup) {
        if (pending.owner) Finish(false, true);
        return DestroyWindow(hwnd);
    }
    if (message == WM_NCDESTROY) dispatcher = nullptr;
    return DefWindowProcW(hwnd, message, wp, lp);
}
bool EnsureDispatcher() {
    if (dispatcher) return GetWindowThreadProcessId(dispatcher, nullptr) == GetCurrentThreadId();
    WNDCLASSW wc{};
    wc.lpfnWndProc = Dispatch;
    wc.hInstance = module;
    wc.lpszClassName = kClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    dispatcher = CreateWindowExW(0, kClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                 nullptr, module, nullptr);
    return dispatcher != nullptr;
}
bool Start(IUnknown* owner, IUnknown* item, HWND hwnd) {
    if (stopping.load(std::memory_order_relaxed) || !IsIconic(hwnd) || !EnsureDispatcher()) return false;
    Operation op;
    op.owner = owner; op.item = item; op.hwnd = hwnd;
    if (!GetWindowPlacement(hwnd, &op.before) || !GetWindowThreadProcessId(hwnd, &op.pid))
        return false;
    op.transitionsDisabled=SetTransitions(hwnd,TRUE);
    op.foreground=GetForegroundWindow();op.started=GetTickCount();
    owner->AddRef();item->AddRef();pending=op;
    if(!SetTimer(dispatcher,1,USER_TIMER_MINIMUM,nullptr)) {EndNow(false,false);return false;}
    if(op.transitionsDisabled && ShowWindowAsync(hwnd,SW_SHOWNOACTIVATE)) return true;
    pending.sequence=(GetTickCount64()<<16)|(++requestCounter&0xffff);
    pending.pipe=Send(Prepare,hwnd,op.pid,pending.sequence,nullptr);
    if(pending.pipe) {pending.stage=Remote;return true;}
    EndNow(false,false);return false;
}
void __cdecl InvokeHook(IUnknown* owner, IUnknown* item) {
    DWORD error = GetLastError();
    bool deferred = false;
    if (item && !stopping.load(std::memory_order_relaxed)) {
        IUnknown* view = nullptr;
        resolve(&view, item);
        if (view) {
            // IApplicationView::GetThumbnailWindow on the checked Shell ABI.
            auto table = *reinterpret_cast<void***>(view);
            using GetWindow = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, HWND*);
            HWND hwnd = nullptr;
            HRESULT hr = reinterpret_cast<GetWindow>(table[9])(view, &hwnd);
            view->Release();
            if (SUCCEEDED(hr) && hwnd) {
                if (pending.owner && pending.hwnd == hwnd) deferred = true;
                else {
                    if (pending.owner) Finish(false, true);
                    deferred = Start(owner, item, hwnd);
                }
            }
        }
    }
    SetLastError(error);
    if (!deferred) original(owner, item);
}
} // namespace
static BOOL Init() {
    auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(L"twinui.pcshell.dll"));
    if (!base) return FALSE;
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    // Startup-only ABI guard. Do not reuse these offsets on another build.
    constexpr BYTE invoke[] = {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0x55,0x57,0x41,0x56};
    constexpr BYTE resolver[] = {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57};
    auto controller = reinterpret_cast<void**>(base + 0x9874b0);
    if (nt->FileHeader.TimeDateStamp != 0xd8ceffa6 || nt->OptionalHeader.SizeOfImage != 0xa27000 ||
        std::memcmp(base + kInvoke, invoke, sizeof(invoke)) ||
        std::memcmp(base + kResolve, resolver, sizeof(resolver)) ||
        controller[1] != base + 0x791e0 || controller[2] != base + 0x81d30)
        return FALSE;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(Dispatch), &module)) return FALSE;
    resolve = reinterpret_cast<Resolve>(base + kResolve);
    if (Wh_SetFunctionHook(base + kInvoke, reinterpret_cast<void*>(InvokeHook),
                           reinterpret_cast<void**>(&original))) return TRUE;
    FreeLibrary(module); module = nullptr;
    return FALSE;
}
static void CleanupDispatcher() {
    if(dispatcher) {
        DWORD_PTR result;
        SendMessageTimeoutW(dispatcher,kCleanup,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,1500,&result);
    }
}
static void BeforeUninit() {
    stopping.store(true,std::memory_order_relaxed);
    CleanupDispatcher();
}
static void Uninit() {
    // Catch a dispatcher created by an in-flight hook after BeforeUninit.
    CleanupDispatcher();
    if(!dispatcher && module) {
        UnregisterClassW(kClass,module);
        FreeLibrary(module);
    }
}
} // namespace Shell
#endif // _WIN64

namespace Receiver {
namespace {

constexpr DWORD kLease=1000;
HANDLE stopEvent, worker;
HMODULE module;
struct Operation {
    HWND hwnd=nullptr;
    WINDOWPLACEMENT before{sizeof(WINDOWPLACEMENT)};
    ULONGLONG sequence=0;
    DWORD started=0;
    bool disabled=false;
} active;
ULONGLONG closedThrough;

void Finish(bool undo) {
    if(active.sequence>closedThrough) closedThrough=active.sequence;
    if(active.hwnd && IsWindowOf(active.hwnd,GetCurrentProcessId())) {
        if(undo) RestoreWindow(active.hwnd,active.before);
        if(active.disabled) SetTransitions(active.hwnd,FALSE);
    }
    active={};
}

void Execute(HANDLE pipe,const Request& r) {
    HWND hwnd=reinterpret_cast<HWND>(static_cast<ULONG_PTR>(r.hwnd));
    HANDLE sourceAck=reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(r.acknowledge));
    ULONG client=0; DWORD shell=0;
    GetWindowThreadProcessId(GetShellWindow(),&shell);
    if(!shell || !GetNamedPipeClientProcessId(pipe,&client) || client!=shell ||
       r.magic!=kMagic || r.reserved || r.pid!=GetCurrentProcessId() ||
       !r.sequence || r.command<Prepare || r.command>Cancel) return;
    // Only the desktop Shell may request a fixed operation on this process's own windows.
    HANDLE ack=nullptr;
    if(r.acknowledge) {
        HANDLE process=OpenProcess(PROCESS_DUP_HANDLE,FALSE,client);
        if(process) {
            DuplicateHandle(process,sourceAck,GetCurrentProcess(),&ack,EVENT_MODIFY_STATE,FALSE,0);
            CloseHandle(process);
        }
        if(!ack) return;
    }
    if(r.command==Prepare) {
        if(r.sequence>closedThrough && r.sequence>active.sequence && IsWindowOf(hwnd,GetCurrentProcessId())) {
            if(active.hwnd) Finish(true);
            WINDOWPLACEMENT p{sizeof(WINDOWPLACEMENT)};
            if(IsIconic(hwnd) && GetWindowPlacement(hwnd,&p)) {
                active.hwnd=hwnd; active.before=p;
                active.sequence=r.sequence;active.started=GetTickCount();
                active.disabled=SetTransitions(hwnd,TRUE);
                if(!active.disabled || !ShowWindowAsync(hwnd,SW_SHOWNOACTIVATE)) Finish(false);
            }
        }
    } else {
        if(r.sequence>closedThrough) closedThrough=r.sequence;
        if(active.sequence==r.sequence && active.hwnd==hwnd) Finish(r.command==Cancel);
    }
    if(ack) {SetEvent(ack);CloseHandle(ack);}
}

struct Slot {
    HANDLE pipe=INVALID_HANDLE_VALUE, event=nullptr;
    OVERLAPPED overlapped{};
    Request request{};
    bool reading=false, pending=false;
};
void ResetOverlapped(Slot& s) {
    ResetEvent(s.event);s.overlapped={};s.overlapped.hEvent=s.event;
}
bool Connect(Slot& s) {
    ResetOverlapped(s);s.reading=false;
    BOOL ok=ConnectNamedPipe(s.pipe,&s.overlapped);
    DWORD error=ok ? ERROR_SUCCESS : GetLastError();
    s.pending=error==ERROR_IO_PENDING;
    if(ok || error==ERROR_PIPE_CONNECTED) SetEvent(s.event);
    return ok || s.pending || error==ERROR_PIPE_CONNECTED;
}
bool Reconnect(Slot& s) {
    DisconnectNamedPipe(s.pipe);
    return Connect(s);
}
bool Pump(Slot& s) {
    DWORD bytes=0;
    if(s.pending && !GetOverlappedResult(s.pipe,&s.overlapped,&bytes,FALSE)) return Reconnect(s);
    if(s.reading) {
        if(bytes==sizeof(Request)) Execute(s.pipe,s.request);
        return Reconnect(s);
    }
    ResetOverlapped(s);s.reading=true;
    BOOL ok=ReadFile(s.pipe,&s.request,sizeof(s.request),&bytes,&s.overlapped);
    s.pending=!ok && GetLastError()==ERROR_IO_PENDING;
    if(s.pending) return true;
    if(ok && bytes==sizeof(Request)) Execute(s.pipe,s.request);
    return Reconnect(s);
}

PSECURITY_DESCRIPTOR PipeSecurity() {
    HANDLE token=nullptr; BYTE buffer[512]; DWORD bytes=0;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return nullptr;
    BOOL ok=GetTokenInformation(token,TokenUser,buffer,sizeof(buffer),&bytes);
    CloseHandle(token);
    if(!ok) return nullptr;
    LPWSTR sid=nullptr;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid,&sid)) return nullptr;
    wchar_t sddl[512];
    wsprintfW(sddl,L"D:P(A;;GA;;;SY)(A;;GA;;;%s)S:(ML;;NW;;;ME)",sid);
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd=nullptr;
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&sd,nullptr);
    return sd;
}

DWORD WINAPI Run(void*) {
    Slot slots[2];
    wchar_t name[80];
    FormatPipeName(name,GetCurrentProcessId());
    PSECURITY_DESCRIPTOR sd=PipeSecurity();
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    bool okay=sd!=nullptr;
    for(unsigned i=0;okay && i<2;++i) {
        Slot& s=slots[i];
        s.event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        s.pipe=CreateNamedPipeW(name,PIPE_ACCESS_INBOUND|FILE_FLAG_OVERLAPPED|
            (i==0 ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
            2,0,512,0,&sa);
        okay=s.event && s.pipe!=INVALID_HANDLE_VALUE && Connect(s);
    }
    if(sd) LocalFree(sd);
    HANDLE events[]{stopEvent,slots[0].event,slots[1].event};
    while(okay) {
        DWORD timeout=INFINITE;
        if(active.hwnd) {
            DWORD elapsed=GetTickCount()-active.started;
            if(elapsed>=kLease) {Finish(false);continue;}
            timeout=kLease-elapsed;
        }
        DWORD result=WaitForMultipleObjects(3,events,FALSE,timeout);
        if(result==WAIT_OBJECT_0) break;
        if(result==WAIT_TIMEOUT) {Finish(false);continue;}
        if(result==WAIT_OBJECT_0+1 || result==WAIT_OBJECT_0+2) okay=Pump(slots[result-WAIT_OBJECT_0-1]);
        else break;
    }
    Finish(true);
    for(auto& s:slots) {
        if(s.pipe!=INVALID_HANDLE_VALUE) {
            CancelIoEx(s.pipe,nullptr);
            if(s.pending) {DWORD bytes;GetOverlappedResult(s.pipe,&s.overlapped,&bytes,TRUE);}
            DisconnectNamedPipe(s.pipe);CloseHandle(s.pipe);
        }
        if(s.event) CloseHandle(s.event);
    }
    // This thread owns the extra DLL reference, including when unloading times out.
    FreeLibraryAndExitThread(module,0);
}
} // namespace

static BOOL Init() {
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(Run),&module)) return FALSE;
    stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(stopEvent) worker=CreateThread(nullptr,0,Run,nullptr,0,nullptr);
    if(worker) return TRUE;
    if(stopEvent) CloseHandle(stopEvent);
    FreeLibrary(module);return FALSE;
}
static void Uninit() {
    SetEvent(stopEvent);
    if(WaitForSingleObject(worker,1500)==WAIT_OBJECT_0) CloseHandle(stopEvent);
    // On timeout the event remains valid for the still-running thread.
    CloseHandle(worker);
}
} // namespace Receiver

BOOL Wh_ModInit() {
#ifdef _WIN64
    DWORD shell=0;
    GetWindowThreadProcessId(GetShellWindow(),&shell);
    isShell=shell && shell==GetCurrentProcessId();
    if(isShell) return Shell::Init();
#endif
    // Exclude auxiliary Explorer hosts by their main image, not loaded resources.
    return GetModuleHandleW(L"explorer.exe")!=GetModuleHandleW(nullptr) && Receiver::Init();
}
#ifdef _WIN64
void Wh_ModBeforeUninit() {
    if(isShell) Shell::BeforeUninit();
}
#endif
void Wh_ModUninit() {
#ifdef _WIN64
    if(isShell) {Shell::Uninit();return;}
#endif
    Receiver::Uninit();
}
