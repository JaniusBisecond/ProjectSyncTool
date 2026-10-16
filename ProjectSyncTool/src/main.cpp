// ProjectSync —— Win32 + Direct3D 11 + Dear ImGui 宿主。
// 只负责窗口、D3D 设备、ImGui 后端与消息循环；业务与界面都在 AppUi 里。

#include "ui/AppUi.h"

#include "CliRunner.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <Windows.h>
#include <d3d11.h>
#include <objbase.h>
#include <shellapi.h>

namespace
{
ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_deviceContext = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
bool g_swapChainOccluded = false;
UINT g_resizeWidth = 0;
UINT g_resizeHeight = 0;
ID3D11RenderTargetView* g_renderTargetView = nullptr;

// 供 WndProc 转发资源管理器拖入的文件夹
AppUi* g_app = nullptr;

bool CreateDeviceD3D(HWND hwnd)
{
	DXGI_SWAP_CHAIN_DESC desc = {};
	desc.BufferCount = 2;
	desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BufferDesc.RefreshRate.Numerator = 60;
	desc.BufferDesc.RefreshRate.Denominator = 1;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.OutputWindow = hwnd;
	desc.SampleDesc.Count = 1;
	desc.Windowed = TRUE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
	D3D_FEATURE_LEVEL obtained = D3D_FEATURE_LEVEL_11_0;

	HRESULT result = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
	                                                 levels, 2, D3D11_SDK_VERSION, &desc,
	                                                 &g_swapChain, &g_device, &obtained, &g_deviceContext);
	if (result == DXGI_ERROR_UNSUPPORTED)
	{
		// 没有可用硬件驱动时退回 WARP 软件渲染
		result = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
		                                         levels, 2, D3D11_SDK_VERSION, &desc,
		                                         &g_swapChain, &g_device, &obtained, &g_deviceContext);
	}
	if (result != S_OK)
	{
		return false;
	}

	ID3D11Texture2D* backBuffer = nullptr;
	g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
	if (backBuffer == nullptr)
	{
		return false;
	}
	g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
	backBuffer->Release();
	return true;
}

void CleanupRenderTarget()
{
	if (g_renderTargetView != nullptr)
	{
		g_renderTargetView->Release();
		g_renderTargetView = nullptr;
	}
}

void CleanupDeviceD3D()
{
	CleanupRenderTarget();
	if (g_swapChain != nullptr)
	{
		g_swapChain->Release();
		g_swapChain = nullptr;
	}
	if (g_deviceContext != nullptr)
	{
		g_deviceContext->Release();
		g_deviceContext = nullptr;
	}
	if (g_device != nullptr)
	{
		g_device->Release();
		g_device = nullptr;
	}
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
}	// namespace

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int)
{
	// 批处理模式（--cli-scan / --cli-execute）不创建窗口，直接跑核心逻辑后退出
	const int cliExitCode = CliRunner::RunIfRequested();
	if (cliExitCode >= 0)
	{
		return cliExitCode;
	}

	// 文件夹/文件选择对话框需要 COM
	const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	::ImGui_ImplWin32_EnableDpiAwareness();
	float dpiScale = ::ImGui_ImplWin32_GetDpiScaleForMonitor(
		::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
	if (dpiScale <= 0.0f)
	{
		dpiScale = 1.0f;
	}

	WNDCLASSEXW windowClass = {sizeof(windowClass), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
	                           nullptr, nullptr, nullptr, nullptr, L"ProjectSyncWindowClass", nullptr};
	::RegisterClassExW(&windowClass);

	HWND hwnd = ::CreateWindowW(windowClass.lpszClassName,
	                            L"ProjectSync",
	                            WS_OVERLAPPEDWINDOW, 80, 60,
	                            static_cast<int>(1200.0f * dpiScale), static_cast<int>(820.0f * dpiScale),
	                            nullptr, nullptr, windowClass.hInstance, nullptr);
	if (hwnd == nullptr)
	{
		::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
		return 1;
	}

	// 允许把文件夹从资源管理器拖进窗口
	::DragAcceptFiles(hwnd, TRUE);

	if (!CreateDeviceD3D(hwnd))
	{
		CleanupDeviceD3D();
		::DestroyWindow(hwnd);
		::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);
		::MessageBoxW(nullptr, L"Direct3D 11 设备创建失败，无法启动界面。", L"ProjectSync", MB_OK | MB_ICONERROR);
		return 1;
	}

	::ShowWindow(hwnd, SW_SHOWDEFAULT);
	::UpdateWindow(hwnd);

	IMGUI_CHECKVERSION();
	::ImGui::CreateContext();
	ImGuiIO& io = ::ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr;	// 不需要 imgui.ini

	::ImGui::StyleColorsDark();
	ImGuiStyle& style = ::ImGui::GetStyle();
	style.FrameRounding = 3.0f;
	style.GrabRounding = 3.0f;
	style.WindowRounding = 0.0f;
	style.ScaleAllSizes(dpiScale);

	::ImGui_ImplWin32_Init(hwnd);
	::ImGui_ImplDX11_Init(g_device, g_deviceContext);

	AppUi app;
	app.Init(hwnd, dpiScale);
	g_app = &app;

	bool done = false;
	while (!done)
	{
		MSG msg;
		while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
		{
			::TranslateMessage(&msg);
			::DispatchMessage(&msg);
			if (msg.message == WM_QUIT)
			{
				done = true;
			}
		}
		if (done)
		{
			break;
		}

		// 窗口被遮挡（最小化 / 锁屏）时不做无谓渲染
		if (g_swapChainOccluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
		{
			::Sleep(10);
			continue;
		}
		g_swapChainOccluded = false;

		if (g_resizeWidth != 0 && g_resizeHeight != 0)
		{
			CleanupRenderTarget();
			g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
			g_resizeWidth = 0;
			g_resizeHeight = 0;

			ID3D11Texture2D* backBuffer = nullptr;
			g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
			if (backBuffer != nullptr)
			{
				g_device->CreateRenderTargetView(backBuffer, nullptr, &g_renderTargetView);
				backBuffer->Release();
			}
		}

		::ImGui_ImplDX11_NewFrame();
		::ImGui_ImplWin32_NewFrame();
		::ImGui::NewFrame();

		app.Draw();

		::ImGui::Render();
		const float clearColor[4] = {0.09f, 0.10f, 0.12f, 1.00f};
		g_deviceContext->OMSetRenderTargets(1, &g_renderTargetView, nullptr);
		g_deviceContext->ClearRenderTargetView(g_renderTargetView, clearColor);
		::ImGui_ImplDX11_RenderDrawData(::ImGui::GetDrawData());

		const HRESULT presentResult = g_swapChain->Present(1, 0);
		g_swapChainOccluded = (presentResult == DXGI_STATUS_OCCLUDED);
	}

	// 先让界面层取消并等待 worker，再拆 ImGui 与 D3D
	g_app = nullptr;
	app.Shutdown();

	::ImGui_ImplDX11_Shutdown();
	::ImGui_ImplWin32_Shutdown();
	::ImGui::DestroyContext();

	CleanupDeviceD3D();
	::DestroyWindow(hwnd);
	::UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance);

	if (SUCCEEDED(comResult))
	{
		::CoUninitialize();
	}
	return 0;
}

namespace
{
LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (::ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
	{
		return true;
	}

	switch (msg)
	{
	case WM_SIZE:
		if (wParam == SIZE_MINIMIZED)
		{
			return 0;
		}
		g_resizeWidth = static_cast<UINT>(LOWORD(lParam));
		g_resizeHeight = static_cast<UINT>(HIWORD(lParam));
		return 0;

	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU)	// 屏蔽 ALT 菜单
		{
			return 0;
		}
		break;

	case WM_DROPFILES:
	{
		HDROP drop = reinterpret_cast<HDROP>(wParam);

		POINT point = {};
		::DragQueryPoint(drop, &point);

		const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
		for (UINT i = 0; i < count; ++i)
		{
			const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
			if (length == 0)
			{
				continue;
			}

			std::wstring path(static_cast<size_t>(length) + 1, L'\0');
			const UINT written = ::DragQueryFileW(drop, i, path.data(), length + 1);
			path.resize(written);

			if (g_app != nullptr)
			{
				g_app->QueueFolderDrop(point.x, point.y, path);
			}
		}

		::DragFinish(drop);
		return 0;
	}

	case WM_DESTROY:
		::PostQuitMessage(0);
		return 0;

	default:
		break;
	}

	return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
}	// namespace
