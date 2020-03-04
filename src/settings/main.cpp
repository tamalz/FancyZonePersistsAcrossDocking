#include "pch.h"
#include <Commdlg.h>
#include "StreamUriResolverFromFile.h"
#include <Shellapi.h>
#include <common/two_way_pipe_message_ipc.h>
#include <ShellScalingApi.h>
#include "resource.h"
#include <common/dpi_aware.h>
#include <common/common.h>
#include "WebView_2.h"
#include <cstdlib>
#include <filesystem>

#include "trace.h"

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "windowsapp")

#ifdef _DEBUG
#define _DEBUG_WITH_LOCALHOST 0
// Define as 1 For debug purposes, to access localhost servers.
// webview_process_options.PrivateNetworkClientServerCapability(winrt::Windows::Web::UI::Interop::WebViewControlProcessCapabilityState::Enabled);
// To access localhost:8080 for development, you'll also need to disable loopback restrictions for the webview:
// > checknetisolation LoopbackExempt -a -n=Microsoft.Win32WebViewHost_cw5n1h2txyewy
// To remove the exception after development:
// > checknetisolation LoopbackExempt -d -n=Microsoft.Win32WebViewHost_cw5n1h2txyewy
// Source: https://github.com/windows-toolkit/WindowsCommunityToolkit/issues/2226#issuecomment-396360314
#endif

using namespace winrt;

#define WM_USER_POST_TO_WEBVIEW (WM_USER + 0)
#define WM_USER_EXIT (WM_USER + 1)

HINSTANCE g_hinst = nullptr;
HWND g_hwnd = nullptr;
WebView2Controller* g_webview_controller = nullptr;

// Message pipe to send/receive messages to/from the Powertoys runner.
TwoWayPipeMessageIPC* g_message_pipe = nullptr;

// Set to true if waiting for webview confirmation before closing the Window.
bool g_waiting_for_exit_confirmation  = false;

// Is the setting window to be started in dark mode
bool g_start_in_dark_mode = false;

void send_message_to_webview(const std::wstring& msg)
{
    if (g_hwnd != nullptr)
    {
        // Allocate the COPYDATASTRUCT and message to pass to the Webview.
        // This is needed in order to use PostMessage, since COM calls to
        // g_webview.InvokeScriptAsync can't be made from other threads.

        PCOPYDATASTRUCT message = new COPYDATASTRUCT();
        DWORD buff_size = (DWORD)(msg.length() + 1);

        // 'wnd_static_proc()' will free the buffer allocated here.
        wchar_t* buffer = new wchar_t[buff_size];

        wcscpy_s(buffer, buff_size, msg.c_str());
        message->dwData = 0;
        message->cbData = buff_size * sizeof(wchar_t);
        message->lpData = (PVOID)buffer;
        WINRT_VERIFY(PostMessage(g_hwnd, WM_USER_POST_TO_WEBVIEW, (WPARAM)g_hwnd, (LPARAM)message));
    }
}

void send_message_to_powertoys_runner(_In_ const std::wstring& msg)
{
    if (g_message_pipe != nullptr)
    {
        g_message_pipe->send(msg);
    }
    else
    {
#ifdef _DEBUG
 #include "DebugData.h"
        // If PowerToysSettings was not started by the PowerToys runner, simulate
        // the data as if it was sent by the runner.
        MessageBox(nullptr, msg.c_str(), L"From Webview", MB_OK);
        send_message_to_webview(debug_settings_info);
#endif
    }
}

// Called by the WebView control.
// The message can be:
// 1 - json data to be forward to the runner
// 2 - exit confirmation by the user
// 3 - exit canceled by the user
void receive_message_from_webview(const std::wstring& msg)
{
    if (msg[0] == '{')
    {
        // It's a JSON string, send the message to the PowerToys runner.
        std::thread(send_message_to_powertoys_runner, msg).detach();
    }
    else
    {
        // It's not a JSON string, check for expected control messages.
        if (msg == L"exit")
        {
            // WebView confirms the settings application can exit.
            WINRT_VERIFY(PostMessage(g_hwnd, WM_USER_EXIT, 0, 0));
        }
        else if (msg == L"cancel-exit")
        {
            // WebView canceled the exit request.
            g_waiting_for_exit_confirmation  = false;
        }
    }
}

LRESULT CALLBACK wnd_proc_static(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CLOSE:
        if (g_waiting_for_exit_confirmation )
        {
            // If another WM_CLOSE is received while waiting for webview confirmation,
            // allow DefWindowProc to be called and destroy the window.
            break;
        }
        else
        {
            // Allow user to confirm exit in the WebView in case there's possible data loss.
            g_waiting_for_exit_confirmation  = true;
            if (g_webview_controller != nullptr)
            {
                g_webview_controller->ProcessExit();
                return 0;
            }
        }
    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    case WM_SIZE:
        if (g_webview_controller != nullptr)
        {
            g_webview_controller->Resize();
        }
        break;
    case WM_CREATE:
        break;
    case WM_DPICHANGED:
    {
        // Resize the window using the suggested rect
        RECT* const prcNewWindow = (RECT*)lParam;
        SetWindowPos(hWnd,
                     nullptr,
                     prcNewWindow->left,
                     prcNewWindow->top,
                     prcNewWindow->right - prcNewWindow->left,
                     prcNewWindow->bottom - prcNewWindow->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    break;
    case WM_NCCREATE: {
        // Enable auto-resizing the title bar
        EnableNonClientDpiScaling(hWnd);
    }
    break;
    case WM_USER_POST_TO_WEBVIEW:
    {
        PCOPYDATASTRUCT msg = (PCOPYDATASTRUCT)lParam;
        wchar_t* json_message = (wchar_t*)(msg->lpData);
        if (g_webview_controller != nullptr)
        {
            g_webview_controller->PostData(json_message);
        }
        delete[] json_message;
        // wnd_proc_static is responsible for freeing memory.
        delete msg;
    }
    break;

    default:
        break;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

void register_classes(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;
    wcex.cbSize = sizeof(WNDCLASSEX);

    wcex.style = 0;
    wcex.lpfnWndProc = wnd_proc_static;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(APPICON));
    wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = g_start_in_dark_mode ? CreateSolidBrush(0) : (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = L"PTSettingsClass";
    wcex.hIconSm = nullptr;

    WINRT_VERIFY(RegisterClassExW(&wcex));
}

HWND create_main_window(HINSTANCE hInstance)
{
    RECT desktopRect;
    const HWND hDesktop = GetDesktopWindow();
    WINRT_VERIFY(hDesktop != nullptr);
    WINRT_VERIFY(GetWindowRect(hDesktop, &desktopRect));

    int wind_width = 1024;
    int wind_height = 700;
    DPIAware::Convert(nullptr, wind_width, wind_height);

    return CreateWindowW(
        L"PTSettingsClass",
        L"PowerToys Settings",
        WS_OVERLAPPEDWINDOW,
        (desktopRect.right - wind_width) / 2,
        (desktopRect.bottom - wind_height) / 2,
        wind_width,
        wind_height,
        nullptr,
        nullptr,
        hInstance,
        nullptr);
}

void wait_on_parent_process(DWORD pid)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (process != nullptr)
    {
        if (WaitForSingleObject(process, INFINITE) == WAIT_OBJECT_0)
        {
            // If it's possible to detect when the PowerToys process terminates, message the main window.
            CloseHandle(process);
            if (g_hwnd)
            {
                WINRT_VERIFY(PostMessage(g_hwnd, WM_USER_EXIT, 0, 0));
            }
        }
        else
        {
            CloseHandle(process);
        }
    }
}

// Parse arguments: initialize two-way IPC message pipe and if settings window is to be started
// in dark mode.
void parse_args()
{
    // Expected calling arguments:
    // [0] - This executable's path.
    // [1] - PowerToys pipe server.
    // [2] - Settings pipe server.
    // [3] - PowerToys process pid.
    // [4] - optional "dark" parameter if the settings window is to be started in dark mode
    LPWSTR* argument_list;
    int n_args;

    argument_list = CommandLineToArgvW(GetCommandLineW(), &n_args);
    if (n_args > 3)
    {
        g_message_pipe = new TwoWayPipeMessageIPC(std::wstring(argument_list[2]), std::wstring(argument_list[1]), send_message_to_webview);
        g_message_pipe->start(nullptr);

        DWORD parent_pid = std::stol(argument_list[3]);
        std::thread(wait_on_parent_process, parent_pid).detach();
    }
    else
    {
#ifndef _DEBUG
        MessageBox(nullptr, L"This executable isn't supposed to be called as a stand-alone process", L"Error running settings", MB_OK);
        exit(1);
#endif
    }

    if (n_args > 4)
    {
        g_start_in_dark_mode = wcscmp(argument_list[4], L"dark") == 0;
    }

    LocalFree(argument_list);
}

bool initialize_com_security_policy_for_webview()
{
    const wchar_t* security_descriptor =
        L"O:BA" // Owner: Builtin (local) administrator
        L"G:BA" // Group: Builtin (local) administrator
        L"D:"
        L"(A;;0x7;;;PS)" // Access allowed on COM_RIGHTS_EXECUTE, _LOCAL, & _REMOTE for Personal self
        L"(A;;0x3;;;SY)" // Access allowed on COM_RIGHTS_EXECUTE, & _LOCAL for Local system
        L"(A;;0x7;;;BA)" // Access allowed on COM_RIGHTS_EXECUTE, _LOCAL, & _REMOTE for Builtin (local) administrator
        L"(A;;0x3;;;S-1-15-3-1310292540-1029022339-4008023048-2190398717-53961996-4257829345-603366646)" // Access allowed on COM_RIGHTS_EXECUTE, & _LOCAL for Win32WebViewHost package capability
        L"S:"
        L"(ML;;NX;;;LW)"; // Integrity label on No execute up for Low mandatory level
    PSECURITY_DESCRIPTOR self_relative_sd{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(security_descriptor, SDDL_REVISION_1, &self_relative_sd, nullptr))
    {
        return false;
    }

    on_scope_exit free_realtive_sd([&] {
        LocalFree(self_relative_sd);
    });

    DWORD absolute_sd_size = 0;
    DWORD dacl_size = 0;
    DWORD group_size = 0;
    DWORD owner_size = 0;
    DWORD sacl_size = 0;

    if (!MakeAbsoluteSD(self_relative_sd, nullptr, &absolute_sd_size, nullptr, &dacl_size, nullptr, &sacl_size, nullptr, &owner_size, nullptr, &group_size))
    {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return false;
        }
    }

    typed_storage<SECURITY_DESCRIPTOR> absolute_sd{ absolute_sd_size };
    typed_storage<ACL> dacl{ dacl_size };
    typed_storage<ACL> sacl{ sacl_size };
    typed_storage<SID> owner{ owner_size };
    typed_storage<SID> group{ group_size };

    if (!MakeAbsoluteSD(self_relative_sd,
                        absolute_sd,
                        &absolute_sd_size,
                        dacl,
                        &dacl_size,
                        sacl,
                        &sacl_size,
                        owner,
                        &owner_size,
                        group,
                        &group_size))
    {
        return false;
    }

    return !FAILED(CoInitializeSecurity(
        absolute_sd,
        -1,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
        RPC_C_IMP_LEVEL_IDENTIFY,
        nullptr,
        EOAC_DYNAMIC_CLOAKING | EOAC_DISABLE_AAA,
        nullptr));
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
    Trace::RegisterProvider();
    winrt::init_apartment(apartment_type::multi_threaded);
    //CoInitialize(nullptr);

    const bool should_try_drop_privileges = !initialize_com_security_policy_for_webview() && is_process_elevated();

    if (should_try_drop_privileges)
    {
        if (!drop_elevated_privileges())
        {
            MessageBox(NULL, L"Failed to drop admin privileges.\nPlease report the bug to https://github.com/microsoft/PowerToys/issues", L"PowerToys Settings Error", MB_OK);
            Trace::SettingsInitError(Trace::SettingsInitErrorCause::FailedToDropPrivileges);
            exit(1);
        }
    }

    g_hinst = hInstance;
    parse_args();
    register_classes(hInstance);
    g_hwnd = create_main_window(hInstance);
    if (g_hwnd == nullptr)
    {
        MessageBox(NULL, L"Failed to create main window.\nPlease report the bug to https://github.com/microsoft/PowerToys/issues", L"PowerToys Settings Error", MB_OK);
        exit(1);
    }

    // WebView2 creates a user data folder, set it to the user temp folder.
    _wputenv_s(L"WEBVIEW2_USER_DATA_FOLDER", std::filesystem::temp_directory_path().wstring().c_str());

    g_webview_controller = new WebView2Controller(g_hwnd, nShowCmd, g_message_pipe, g_start_in_dark_mode, receive_message_from_webview);
    ShowWindow(g_hwnd, nShowCmd);

    // Main message loop.
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) != 0)
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Trace::UnregisterProvider();
    return (int)msg.wParam;
}
