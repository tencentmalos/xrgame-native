// SPDX-License-Identifier: GPL-3.0-or-later
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static unsigned keys, frames;
static int x = 100;
static FILE *report;
static BOOL gdi;
static BOOL resize_requested;
static HWND window;

__declspec(noinline) uint64_t xrgame_probe_add(uint64_t a, uint64_t b) {
    return a + b;
}

static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_KEYDOWN:
        ++keys;
        if (wparam == VK_SPACE) {
            uint64_t sum = xrgame_probe_add((uint64_t)keys, UINT64_C(0x12345678));
            fprintf(report, "{\"event\":\"debugFunction\",\"wineTid\":%lu,\"a\":%u,\"sum\":\"%llx\"}\n",
                    GetCurrentThreadId(), keys, (unsigned long long)sum);
            fflush(report);
        }
        if (wparam == VK_ESCAPE) DestroyWindow(hwnd);
        else { x += wparam == VK_LEFT ? -20 : 20; InvalidateRect(hwnd, NULL, TRUE); }
        return 0;
    case WM_LBUTTONDOWN:
        ++keys; x = LOWORD(lparam); InvalidateRect(hwnd, NULL, TRUE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(hwnd, &paint);
        if (gdi) {
            RECT area = {x, 100, x + 100, 200};
            FillRect(dc, &area, GetSysColorBrush(COLOR_HIGHLIGHT));
            const wchar_t *label = L"XRGame x64 GDI: arrows / click move; Esc exits";
            TextOutW(dc, 20, 20, label, (int)wcslen(label));
        }
        EndPaint(hwnd, &paint); return 0;
    }
    case WM_SIZE:
        if (report) {
            fprintf(report, "{\"event\":\"windowSize\",\"kind\":%llu,\"width\":%u,\"height\":%u}\n",
                    (unsigned long long)wparam, (unsigned)LOWORD(lparam), (unsigned)HIWORD(lparam));
            fflush(report);
        }
        resize_requested = TRUE;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

int wmain(int argc, wchar_t **argv) {
    wchar_t temp[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, temp) ||
        swprintf(path, MAX_PATH, L"%lsxrgame-windows-probe-%lu.jsonl", temp, GetCurrentProcessId()) < 0) return 2;
    report = _wfopen(path, L"wb");
    if (!report) return 2;
    typedef LONG (WINAPI *query_process_fn)(HANDLE, ULONG, void *, ULONG, ULONG *);
    query_process_fn query_process = (query_process_fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    int unix_pid = 0;
    LONG identity_status = query_process ? query_process(GetCurrentProcess(), 1101 /* Wine Unix PID */, &unix_pid, sizeof(unix_pid), NULL) : (LONG)0xc0000002;
    fprintf(report, "{\"event\":\"start\",\"winePid\":%lu,\"unixPid\":%d,\"identityStatus\":\"%08lx\",\"pointerBits\":%u}\n",
            GetCurrentProcessId(), identity_status == 0 ? unix_pid : 0, (unsigned long)identity_status,
            (unsigned)(sizeof(void *) * 8));
    fflush(report);
    if (argc > 1 && !wcscmp(argv[1], L"--path-contract")) {
        wchar_t module[MAX_PATH] = {0}, cwd[MAX_PATH] = {0}, converted[64] = {0};
        wchar_t ini[MAX_PATH], value[64] = {0};
        DWORD module_length = GetModuleFileNameW(NULL, module, MAX_PATH);
        DWORD cwd_length = GetCurrentDirectoryW(MAX_PATH, cwd);
        int converted_length = MultiByteToWideChar(CP_UTF8, 0, "graphics_option.ini", -1,
                                                  converted, 64);
        BOOL made = GetTempFileNameW(temp, L"xrg", 0, ini);
        BOOL written = made && WritePrivateProfileStringW(L"GraphicsOption", L"Resolution", L"1280x720", ini);
        DWORD count = made ? GetPrivateProfileStringW(L"GraphicsOption", L"Resolution", L"missing", value, 64, ini) : 0;
        BOOL matched = count == 8 && !wcscmp(value, L"1280x720");
        BOOL removed = made && DeleteFileW(ini);
        fprintf(report, "{\"event\":\"pathContract\",\"moduleLength\":%lu,\"moduleFirst\":%u,"
                "\"cwdLength\":%lu,\"cwdFirst\":%u,\"utf8Length\":%d,\"utf8Matched\":%s,"
                "\"profileWritten\":%s,\"profileLength\":%lu,\"profileMatched\":%s,\"cleanup\":%s}\n",
                module_length, (unsigned)module[0], cwd_length, (unsigned)cwd[0], converted_length,
                !wcscmp(converted, L"graphics_option.ini") ? "true" : "false",
                written ? "true" : "false", count, matched ? "true" : "false", removed ? "true" : "false");
        fclose(report);
        return module_length && cwd_length && converted_length == 20 && written && matched && removed ? 0 : 1;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--buffer-contract")) {
        ID3D11Device *device = NULL;
        D3D_FEATURE_LEVEL level;
        D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_HARDWARE;
        HRESULT hr = D3D11CreateDevice(NULL, driver, NULL, 0, NULL, 0,
                                      D3D11_SDK_VERSION, &device, &level, NULL);
        if (FAILED(hr)) {
            driver = D3D_DRIVER_TYPE_WARP;
            hr = D3D11CreateDevice(NULL, driver, NULL, 0, NULL, 0,
                                  D3D11_SDK_VERSION, &device, &level, NULL);
        }
        fprintf(report, "{\"event\":\"bufferDevice\",\"driverType\":%u,\"hresult\":\"%08lx\"}\n",
                (unsigned)driver, (unsigned long)hr);
        HRESULT valid_hr = E_FAIL;
        if (SUCCEEDED(hr)) {
            const UINT sizes[] = {16, 0};
            for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
                D3D11_BUFFER_DESC desc = {0}, obtained = {0};
                desc.ByteWidth = sizes[i];
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                desc.StructureByteStride = 16;
                ID3D11Buffer *buffer = NULL;
                HRESULT result = ID3D11Device_CreateBuffer(device, &desc, NULL, &buffer);
                if (!i) valid_hr = result;
                if (buffer) ID3D11Buffer_GetDesc(buffer, &obtained);
                fprintf(report, "{\"event\":\"structuredBuffer\",\"requestedBytes\":%u,"
                        "\"hresult\":\"%08lx\",\"hasBuffer\":%s,\"actualBytes\":%u}\n",
                        sizes[i], (unsigned long)result, buffer ? "true" : "false", obtained.ByteWidth);
                if (buffer) ID3D11Buffer_Release(buffer);
            }
            ID3D11Device_Release(device);
        }
        fclose(report);
        wprintf(L"XRGame buffer report: %ls\n", path);
        /* Observe zero-size behavior; do not assume the native driver's answer. */
        return SUCCEEDED(hr) && SUCCEEDED(valid_hr) ? 0 : 12;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--console")) {
        wchar_t env[64];
        BOOL args_ok = argc == 5 && !wcscmp(argv[2], L"") && !wcscmp(argv[3], L"a b") && !wcscmp(argv[4], L"中文");
        BOOL env_ok = GetEnvironmentVariableW(L"XR_PROBE_SENTINEL", env, 64) && !wcscmp(env, L"space value");
        uint64_t sum = xrgame_probe_add(UINT64_C(0x123456789abcdef0), 0x12);
        fprintf(report, "{\"event\":\"console\",\"argsMatch\":%s,\"envMatch\":%s,\"sum\":\"%llx\"}\n",
                args_ok ? "true" : "false", env_ok ? "true" : "false", (unsigned long long)sum);
        fclose(report);
        wprintf(L"XRGame console report: %ls\n", path);
        return args_ok && env_ok && sum == UINT64_C(0x123456789abcdf02) ? 37 : 38;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--d3d10-check")) {
        typedef HRESULT (WINAPI *create10_fn)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, ID3D10Device **);
        typedef HRESULT (WINAPI *create101_fn)(IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1, UINT, ID3D10Device1 **);
        HMODULE lib10 = LoadLibraryW(L"d3d10.dll"), lib101 = LoadLibraryW(L"d3d10_1.dll");
        create10_fn create10 = lib10 ? (create10_fn)GetProcAddress(lib10, "D3D10CreateDevice") : NULL;
        create101_fn create101 = lib101 ? (create101_fn)GetProcAddress(lib101, "D3D10CreateDevice1") : NULL;
        ID3D10Device *d10 = NULL;
        ID3D10Device1 *d101 = NULL;
        HRESULT hr10 = create10 ? create10(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &d10) : E_NOINTERFACE;
        HRESULT hr101 = create101 ? create101(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &d101) : E_NOINTERFACE;
        fprintf(report, "{\"event\":\"d3d10Devices\",\"d3d10\":\"%08lx\",\"d3d10_1\":\"%08lx\"}\n", (unsigned long)hr10, (unsigned long)hr101);
        if (d10) ID3D10Device_Release(d10);
        if (d101) ID3D10Device1_Release(d101);
        if (lib10) FreeLibrary(lib10);
        if (lib101) FreeLibrary(lib101);
        fclose(report);
        return SUCCEEDED(hr10) && SUCCEEDED(hr101) ? 0 : 9;
    }
    /* Verify Windows child creation before opening the graphics window. */
    wchar_t executable[MAX_PATH], command[MAX_PATH + 80];
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION child = {0};
    startup.cb = sizeof(startup);
    DWORD child_exit = STILL_ACTIVE;
    BOOL child_started = FALSE;
    if (GetModuleFileNameW(NULL, executable, MAX_PATH) &&
        swprintf(command, MAX_PATH + 80, L"\"%ls\" --console \"\" \"a b\" \"中文\"", executable) > 0) {
        SetEnvironmentVariableW(L"XR_PROBE_SENTINEL", L"space value");
        child_started = CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                                      NULL, NULL, &startup, &child);
        if (child_started) {
            if (WaitForSingleObject(child.hProcess, 15000) != WAIT_OBJECT_0) {
                TerminateProcess(child.hProcess, 124);
                WaitForSingleObject(child.hProcess, 2000);
            }
            GetExitCodeProcess(child.hProcess, &child_exit);
            CloseHandle(child.hThread); CloseHandle(child.hProcess);
        }
    }
    fprintf(report, "{\"event\":\"consoleChild\",\"started\":%s,\"exitCode\":%lu}\n",
            child_started ? "true" : "false", child_exit);
    fflush(report);
    if (!child_started || child_exit != 37) return 8;
    /* This is a console-subsystem executable; do not let conhost cover the test window. */
    FreeConsole();
    gdi = argc > 1 && !wcscmp(argv[1], L"--gdi");
    WNDCLASSW cls = {0};
    cls.lpfnWndProc = window_proc; cls.hInstance = GetModuleHandleW(NULL);
    cls.lpszClassName = L"XRGameWindowsProbe"; cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    if (!RegisterClassW(&cls)) return 3;
    window = CreateWindowW(cls.lpszClassName, gdi ? L"XRGame x64 GDI" : L"XRGame x64 DXVK D3D11",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 800, 500,
                           NULL, NULL, cls.hInstance, NULL);
    if (!window) return 4;
    ID3D11Device *device = NULL;
    ID3D11DeviceContext *context = NULL;
    IDXGISwapChain *chain = NULL;
    ID3D11RenderTargetView *target = NULL;
    ID3D11VertexShader *vertex_shader = NULL;
    ID3D11PixelShader *pixel_shader = NULL;
    BOOL auto_size = argc > 1 && !wcscmp(argv[1], L"--display-contract-auto");
    BOOL display_contract = auto_size || (argc > 1 && !wcscmp(argv[1], L"--display-contract"));
    if (!gdi) {
        DXGI_SWAP_CHAIN_DESC desc = {0};
        desc.BufferDesc.Width = auto_size ? 0 : 800;
        desc.BufferDesc.Height = auto_size ? 0 : 500;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.OutputWindow = window; desc.Windowed = TRUE;
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                                  D3D11_SDK_VERSION, &desc, &chain, &device, &level, &context);
        fprintf(report, "{\"event\":\"d3d11Create\",\"hresult\":\"%08lx\"}\n", (unsigned long)hr); fflush(report);
        if (FAILED(hr)) return 5;
        ID3D11Texture2D *buffer = NULL;
        hr = IDXGISwapChain_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **)&buffer);
        if (display_contract) {
            DXGI_SWAP_CHAIN_DESC actual = {0};
            D3D11_TEXTURE2D_DESC texture = {0};
            RECT client = {0};
            HRESULT desc_hr = IDXGISwapChain_GetDesc(chain, &actual);
            BOOL rect_ok = GetClientRect(window, &client);
            if (buffer) ID3D11Texture2D_GetDesc(buffer, &texture);
            DEVMODEW desktop = {0};
            desktop.dmSize = sizeof(desktop);
            BOOL desktop_ok = EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &desktop);
            for (DWORD index = 0; index < 16; ++index) {
                DISPLAY_DEVICEW display = {0};
                DEVMODEW mode = {0};
                display.cb = sizeof(display);
                mode.dmSize = sizeof(mode);
                if (!EnumDisplayDevicesW(NULL, index, &display, 0)) break;
                BOOL mode_ok = EnumDisplaySettingsW(display.DeviceName, ENUM_CURRENT_SETTINGS, &mode);
                fprintf(report, "{\"event\":\"displayDevice\",\"index\":%lu,\"flags\":%lu,\"name\":\"", index, display.StateFlags);
                for (unsigned i = 0; i < sizeof(display.DeviceName) / sizeof(*display.DeviceName) && display.DeviceName[i]; ++i)
                    fprintf(report, "\\u%04x", (unsigned)display.DeviceName[i]);
                fprintf(report, "\",\"modeOk\":%s,\"size\":[%lu,%lu]}\n", mode_ok ? "true" : "false", mode.dmPelsWidth, mode.dmPelsHeight);
            }
            IDXGIFactory *factory = NULL;
            IDXGIAdapter *adapter = NULL;
            IDXGIOutput *output = NULL;
            DXGI_OUTPUT_DESC output_desc = {0};
            DXGI_MODE_DESC request = {0}, closest = {0};
            request.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            HRESULT output_hr = CreateDXGIFactory(&IID_IDXGIFactory, (void **)&factory);
            if (SUCCEEDED(output_hr)) output_hr = IDXGIFactory_EnumAdapters(factory, 0, &adapter);
            if (SUCCEEDED(output_hr)) output_hr = IDXGIAdapter_EnumOutputs(adapter, 0, &output);
            if (SUCCEEDED(output_hr)) output_hr = IDXGIOutput_GetDesc(output, &output_desc);
            HRESULT closest_hr = output ? IDXGIOutput_FindClosestMatchingMode(output, &request, &closest, (IUnknown *)device) : E_FAIL;
            fprintf(report, "{\"event\":\"displayMode\",\"desktopOk\":%s,\"desktop\":[%lu,%lu],"
                    "\"systemMetrics\":[%d,%d],\"outputDesc\":\"%08lx\",\"outputRect\":[%ld,%ld,%ld,%ld],"
                    "\"closestMode\":\"%08lx\",\"closest\":[%u,%u]}\n",
                    desktop_ok ? "true" : "false", desktop.dmPelsWidth, desktop.dmPelsHeight,
                    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), (unsigned long)output_hr,
                    output_desc.DesktopCoordinates.left, output_desc.DesktopCoordinates.top,
                    output_desc.DesktopCoordinates.right, output_desc.DesktopCoordinates.bottom,
                    (unsigned long)closest_hr, closest.Width, closest.Height);
            if (output) IDXGIOutput_Release(output);
            if (adapter) IDXGIAdapter_Release(adapter);
            if (factory) IDXGIFactory_Release(factory);
            /* Match MHW's structured SRV/UAV buffer, with a valid size and zero size. */
            D3D11_BUFFER_DESC structured = {0};
            structured.ByteWidth = 16;
            structured.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
            structured.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            structured.StructureByteStride = 16;
            ID3D11Buffer *valid = NULL, *zero = NULL;
            HRESULT valid_hr = ID3D11Device_CreateBuffer(device, &structured, NULL, &valid);
            structured.ByteWidth = 0;
            HRESULT zero_hr = ID3D11Device_CreateBuffer(device, &structured, NULL, &zero);
            fprintf(report, "{\"event\":\"displayContract\",\"getDesc\":\"%08lx\",\"swapchain\":[%u,%u],"
                    "\"getBuffer\":\"%08lx\",\"texture\":[%u,%u],\"clientRectOk\":%s,\"client\":[%ld,%ld],"
                    "\"validBuffer\":\"%08lx\",\"zeroBuffer\":\"%08lx\"}\n",
                    (unsigned long)desc_hr, actual.BufferDesc.Width, actual.BufferDesc.Height,
                    (unsigned long)hr, texture.Width, texture.Height, rect_ok ? "true" : "false",
                    client.right - client.left, client.bottom - client.top,
                    (unsigned long)valid_hr, (unsigned long)zero_hr);
            UINT expected_width = auto_size ? (UINT)(client.right - client.left) : 800;
            UINT expected_height = auto_size ? (UINT)(client.bottom - client.top) : 500;
            BOOL passed = SUCCEEDED(desc_hr) && SUCCEEDED(hr) && rect_ok &&
                actual.BufferDesc.Width == expected_width && actual.BufferDesc.Height == expected_height &&
                texture.Width == expected_width && texture.Height == expected_height &&
                client.right > client.left && client.bottom > client.top &&
                SUCCEEDED(valid_hr) && zero_hr == E_INVALIDARG;
            if (valid) ID3D11Buffer_Release(valid);
            if (zero) ID3D11Buffer_Release(zero);
            if (buffer) ID3D11Texture2D_Release(buffer);
            IDXGISwapChain_Release(chain);
            ID3D11DeviceContext_Release(context);
            ID3D11Device_Release(device);
            DestroyWindow(window);
            fprintf(report, "{\"event\":\"displayContractExit\",\"result\":%d}\n", passed ? 0 : 11);
            fclose(report);
            return passed ? 0 : 11;
        }
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)buffer, NULL, &target);
        if (buffer) ID3D11Texture2D_Release(buffer);
        if (FAILED(hr)) return 6;
        const char *hlsl =
            "float4 vs(uint id:SV_VertexID):SV_POSITION {"
            "return float4(id==0?-0.8:(id==1?0.0:0.8),id==1?0.8:-0.8,0,1); }"
            "float4 ps(float4 p:SV_POSITION):SV_TARGET { return float4(1,0.5,0.1,1); }";
        ID3DBlob *vs = NULL, *ps = NULL, *errors = NULL;
        hr = D3DCompile(hlsl, strlen(hlsl), "xrgame-probe", NULL, NULL, "vs", "vs_4_0", 0, 0, &vs, &errors);
        if (errors) { ID3D10Blob_Release(errors); errors = NULL; }
        if (SUCCEEDED(hr)) hr = D3DCompile(hlsl, strlen(hlsl), "xrgame-probe", NULL, NULL, "ps", "ps_4_0", 0, 0, &ps, &errors);
        if (errors) ID3D10Blob_Release(errors);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(device, ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &vertex_shader);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(device, ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &pixel_shader);
        if (vs) ID3D10Blob_Release(vs);
        if (ps) ID3D10Blob_Release(ps);
        fprintf(report, "{\"event\":\"shaders\",\"hresult\":\"%08lx\"}\n", (unsigned long)hr); fflush(report);
        if (FAILED(hr)) return 9;
        RECT client;
        GetClientRect(window, &client);
        D3D11_VIEWPORT viewport = {0, 0, (float)client.right, (float)client.bottom, 0, 1};
        ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
        ID3D11DeviceContext_IASetPrimitiveTopology(context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(context, vertex_shader, NULL, 0);
        ID3D11DeviceContext_PSSetShader(context, pixel_shader, NULL, 0);
    }
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    UpdateWindow(window);
    MSG message;
    BOOL done = FALSE;
    DWORD start = GetTickCount();
    int result = 0;
    BOOL lifecycle = argc > 1 && !wcscmp(argv[1], L"--lifecycle");
    BOOL clear_only = argc > 1 && (!wcscmp(argv[1], L"--clear-only") || !wcscmp(argv[1], L"--clear-readback") || !wcscmp(argv[1], L"--clear-flush"));
    BOOL readback = argc > 1 && (!wcscmp(argv[1], L"--readback") || !wcscmp(argv[1], L"--clear-readback"));
    BOOL clear_flush = argc > 1 && !wcscmp(argv[1], L"--clear-flush");
    while (!done && GetTickCount() - start < (gdi ? 300000U : 120000U)) {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { done = TRUE; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (done) break;
        if (!gdi) {
            if (lifecycle && (frames == 120 || frames == 240)) {
                BOOL wide = frames == 120;
                SetWindowPos(window, NULL, 0, 0, wide ? 1000 : 800, wide ? 650 : 500, SWP_NOMOVE | SWP_NOZORDER);
            }
            if (lifecycle && frames == 360) {
                ShowWindow(window, SW_HIDE);
                Sleep(500);
                ShowWindow(window, SW_SHOW);
                fprintf(report, "{\"event\":\"hideShow\"}\n"); fflush(report);
            }
            if (lifecycle && frames == 480) break;
            if (resize_requested) {
                RECT size;
                resize_requested = FALSE;
                GetClientRect(window, &size);
                if (size.right && size.bottom) {
                    ID3D11DeviceContext_OMSetRenderTargets(context, 0, NULL, NULL);
                    ID3D11RenderTargetView_Release(target); target = NULL;
                    HRESULT resize_hr = IDXGISwapChain_ResizeBuffers(chain, 0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
                    ID3D11Texture2D *back = NULL;
                    if (SUCCEEDED(resize_hr)) resize_hr = IDXGISwapChain_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **)&back);
                    if (SUCCEEDED(resize_hr)) resize_hr = ID3D11Device_CreateRenderTargetView(device, (ID3D11Resource *)back, NULL, &target);
                    if (back) ID3D11Texture2D_Release(back);
                    fprintf(report, "{\"event\":\"resize\",\"width\":%ld,\"height\":%ld,\"hresult\":\"%08lx\"}\n",
                            size.right, size.bottom, (unsigned long)resize_hr); fflush(report);
                    if (FAILED(resize_hr)) { result = 10; break; }
                    D3D11_VIEWPORT viewport = {0, 0, (float)size.right, (float)size.bottom, 0, 1};
                    ID3D11DeviceContext_RSSetViewports(context, 1, &viewport);
                }
            }
            const float colour[] = {0.1f, ((keys + frames / 60) % 5) * 0.15f + 0.1f, 0.7f, 1.0f};
            ID3D11DeviceContext_OMSetRenderTargets(context, 1, &target, NULL);
            ID3D11DeviceContext_ClearRenderTargetView(context, target, colour);
            if (!clear_only)
                ID3D11DeviceContext_Draw(context, 3, 0);
            if (!frames && readback) {
                /* One diagnostic readback, before presenting, isolates guest rendering from transport. */
                ID3D11Texture2D *back = NULL, *staging = NULL;
                HRESULT sample_hr = IDXGISwapChain_GetBuffer(chain, 0, &IID_ID3D11Texture2D, (void **)&back);
                if (SUCCEEDED(sample_hr)) {
                    D3D11_TEXTURE2D_DESC sample_desc;
                    ID3D11Texture2D_GetDesc(back, &sample_desc);
                    sample_desc.Usage = D3D11_USAGE_STAGING;
                    sample_desc.BindFlags = 0;
                    sample_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    sample_desc.MiscFlags = 0;
                    sample_hr = ID3D11Device_CreateTexture2D(device, &sample_desc, NULL, &staging);
                    if (SUCCEEDED(sample_hr)) {
                        ID3D11DeviceContext_CopyResource(context, (ID3D11Resource *)staging, (ID3D11Resource *)back);
                        D3D11_MAPPED_SUBRESOURCE mapped;
                        sample_hr = ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mapped);
                        if (SUCCEEDED(sample_hr)) {
                            const unsigned char *pixel = mapped.pData;
                            fprintf(report, "{\"event\":\"guestPixel\",\"rgba\":[%u,%u,%u,%u]}\n", pixel[0], pixel[1], pixel[2], pixel[3]);
                            ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
                        }
                        ID3D11Texture2D_Release(staging);
                    }
                    ID3D11Texture2D_Release(back);
                }
                fprintf(report, "{\"event\":\"guestReadback\",\"hresult\":\"%08lx\"}\n", (unsigned long)sample_hr);
                fflush(report);
            }
            if (clear_flush) ID3D11DeviceContext_Flush(context);
            HRESULT hr = IDXGISwapChain_Present(chain, 1, 0);
            if (FAILED(hr)) { fprintf(report, "{\"event\":\"presentFailure\",\"hresult\":\"%08lx\"}\n", (unsigned long)hr); result = 7; break; }
            ++frames;
            if (frames == 1) { fprintf(report, "{\"event\":\"firstPresent\"}\n"); fflush(report); }
        }
        Sleep(16);
    }
    if (target) ID3D11RenderTargetView_Release(target);
    if (vertex_shader) ID3D11VertexShader_Release(vertex_shader);
    if (pixel_shader) ID3D11PixelShader_Release(pixel_shader);
    if (chain) IDXGISwapChain_Release(chain);
    if (context) ID3D11DeviceContext_Release(context);
    if (device) ID3D11Device_Release(device);
    fprintf(report, "{\"event\":\"exit\",\"frames\":%u,\"inputEvents\":%u,\"elapsedMs\":%lu,\"result\":%d}\n",
            frames, keys, GetTickCount() - start, result);
    fclose(report);
    return result;
}
