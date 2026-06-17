// 防止 windows.h 自动包含 winsock.h (避免与 winsock2.h 冲突)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <string>
#include "core/Config.hpp"
#include "core/Logger.hpp"

// 前向声明
namespace Hooks {
    void Install();
    void Uninstall();
}

namespace VersionProxy {
    bool Initialize();
    void Uninitialize();
}

namespace {
    struct LoadNotifyPayload {
        bool success;
        const wchar_t* message;
    };

    const LoadNotifyPayload kLoadSuccessNotify{
        true,
        L"Antigravity-Proxy 已加载，配置读取成功，API Hook 安装流程已执行。"
    };

    const LoadNotifyPayload kLoadFailedNotify{
        false,
        L"Antigravity-Proxy 已加载，但配置读取失败，当前已进入 BYPASS 模式。请检查 config.json 与日志。"
    };

    DWORD WINAPI LoadNotifyThreadProc(LPVOID param) {
        const auto* payload = reinterpret_cast<const LoadNotifyPayload*>(param);
        if (!payload) return 0;

        // 延迟到 DllMain 返回后再按需加载 user32，避免默认导入 UI 库并降低加载期行为面。
        Sleep(800);
        HMODULE user32 = LoadLibraryW(L"user32.dll");
        if (!user32) return 0;

        using MessageBoxWFn = int (WINAPI*)(HWND, LPCWSTR, LPCWSTR, UINT);
        auto messageBoxW = reinterpret_cast<MessageBoxWFn>(GetProcAddress(user32, "MessageBoxW"));
        if (!messageBoxW) {
            FreeLibrary(user32);
            return 0;
        }

        messageBoxW(
            NULL,
            payload->message,
            L"Antigravity-Proxy 加载状态",
            MB_OK | MB_TOPMOST | MB_SETFOREGROUND | (payload->success ? MB_ICONINFORMATION : MB_ICONERROR)
        );
        FreeLibrary(user32);
        return 0;
    }

    void MaybeShowLoadNotifyAsync(bool success) {
        const auto& config = Core::Config::Instance();
        if (config.uiLoadNotify != "messagebox") return;

        const LoadNotifyPayload* payload = success ? &kLoadSuccessNotify : &kLoadFailedNotify;
        HANDLE hThread = CreateThread(NULL, 0, LoadNotifyThreadProc, (LPVOID)payload, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
        } else {
            Core::Logger::Warn("加载提示线程启动失败, err=" + std::to_string(GetLastError()));
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
    switch (fdwReason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hinstDLL);
        
        // ============================================================================
        // VersionProxy 采用懒加载模式 (Lazy Initialization)
        // Initialize() 现在是空操作，真正的系统 version.dll 会在导出函数首次被调用时加载
        // 这样可以避免在 DllMain 中调用 LoadLibraryW 导致的 Loader Lock 问题
        // （可能触发 0xc0000022 STATUS_ACCESS_DENIED 错误）
        // ============================================================================
        VersionProxy::Initialize();  // 空操作，保持接口兼容
        
        Core::Logger::Info("Antigravity-Proxy DLL 已加载 (模拟 version.dll)");
        
        // 加载配置
        const bool loaded = Core::Config::Instance().Load("config.json");
        
        // WARN-4: 必须检查 Load() 返回值。若加载失败则进入 BYPASS 模式，避免“坏配置导致全局网络不可用”。
        if (!loaded) {
            Core::Logger::Error("配置加载失败：已进入 BYPASS 模式（不安装 Hooks）。请检查 config.json 与日志告警信息。");
            MaybeShowLoadNotifyAsync(false);
            break;
        }

        // 安装 Hooks（必须及时安装以确保网络流量被正确拦截）
        Hooks::Install();
        MaybeShowLoadNotifyAsync(true);
        break;
    }
        
    case DLL_PROCESS_DETACH: {
        Hooks::Uninstall();
        VersionProxy::Uninitialize();
        Core::Logger::Info("Antigravity-Proxy DLL 已卸载");
        break;
    }
    }
    return TRUE;
}
