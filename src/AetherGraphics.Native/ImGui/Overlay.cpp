#include "Overlay.h"
#include "../Core/Config.h"
#include "../Core/Log.h"
#include "../Device/GpuInfo.h"
#include "../Diagnostics.h"
#include "../Profiler/Profiler.h"
#include "../Renderer/Dlss.h"
#include "../Renderer/Renderer.h"
#include "../Renderer/SceneRenderer.h"
#include "../Resources/GpuResources.h"
#include "../Resources/ResourceRegistry.h"
#include "../Scene/SceneStore.h"
#include "../Runtime.h"
#include "../Shaders/ShaderManager.h"

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include <windowsx.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace aether {

Overlay& Overlay::Get() {
	static Overlay* o = new Overlay();
	return *o;
}

void Overlay::SetVisible(bool visible) {
	visible_.store(visible);
	if (!Visible()) {
		wantsMouse_.store(false);
		wantsKeyboard_.store(false);
	}
}

void Overlay::SetDebugVisible(bool visible) {
	debug_.store(visible);
	if (!Visible()) {
		wantsMouse_.store(false);
		wantsKeyboard_.store(false);
	}
}

static bool IsMouseMessage(UINT msg);

bool Overlay::WantsFrame() const {
	return Visible() || hud_.load() || Runtime::Get().ApprovalPending();
}

int Overlay::InputFlags() const {
	if (FreeCameraLive()) {
		return 3; // the free camera has the mouse and the keyboard: the game must not drive or steer the menus with them
	}
	return Visible() || Runtime::Get().ApprovalPending() ? (wantsMouse_.load() ? 1 : 0) | (wantsKeyboard_.load() ? 2 : 0) : 0;
}

int Overlay::TakeFreeCameraInput(float fovDegrees, float out[5]) {
	freeCameraFov_.store(fovDegrees);
	const SettingsData s = Settings::Get().Snapshot();
	freeCameraHotkey_.store(s.freeCameraHotkey);
	hud_.store(s.overlayHud);
	const bool live = FreeCameraLive();
	uint32_t keys = freeCameraKeys_.load();
	if (live && hooked_ && GetForegroundWindow() == hooked_) {
		// The keys' state and the cursor read directly: the game takes the keyboard and the mouse through raw input
		// (Rewired), and the letter keys and mouse motion did not all arrive as window messages (the camera did not move).
		auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
		if (down('W') || down(VK_UP)) keys |= kKeyForward;
		if (down('A') || down(VK_LEFT)) keys |= kKeyLeft;
		if (down('S') || down(VK_DOWN)) keys |= kKeyBack;
		if (down('D') || down(VK_RIGHT)) keys |= kKeyRight;
		if (down('Q') || down('C')) keys |= kKeyDown;
		if (down('E') || down(VK_SPACE)) keys |= kKeyUp;
		if (down(VK_SHIFT)) keys |= kKeyFast;
		if (down(VK_CONTROL)) keys |= kKeySlow;
		if (down('R')) keys |= kKeyResetFov;
		if (!rawMouse_.load()) {
			// the cursor's way from the window's middle since the last frame, then back to the middle
			RECT rc;
			POINT centre = {0, 0}, cursor;
			if (GetClientRect(hooked_, &rc) && GetCursorPos(&cursor)) {
				centre = {(rc.right - rc.left) / 2, (rc.bottom - rc.top) / 2};
				ClientToScreen(hooked_, &centre);
				if (cursorCentred_) {
					mouseX_.fetch_add(cursor.x - centre.x);
					mouseY_.fetch_add(cursor.y - centre.y);
				}
				SetCursorPos(centre.x, centre.y);
				cursorCentred_ = true;
			}
		}
	} else {
		cursorCentred_ = false;
	}
	if (live && keys != 0 && !loggedInput_) {
		loggedInput_ = true;
		AE_LOG(Info, "Overlay", "Free camera: keys arrive (0x%X)", keys);
	}
	const int mx = mouseX_.exchange(0), my = mouseY_.exchange(0), wheel = wheel_.exchange(0);
	out[0] = live ? float(mx) : 0.0f;
	out[1] = live ? float(my) : 0.0f;
	out[2] = live ? float(wheel) / float(WHEEL_DELTA) : 0.0f;
	out[3] = s.freeCameraSpeed;
	out[4] = s.freeCameraSensitivity;
	int result = (freeCamera_.load() ? 1 : 0) | (live ? 2 : 0);
	if (live) {
		result |= int(keys);
	}
	return result;
}

// The free camera's input, while it has it (on, the overlay closed): every key and mouse message is kept from the
// game. Keys held are remembered, the mouse's motion and wheel are added up for TakeFreeCameraInput.
bool Overlay::HandleFreeCamera(UINT msg, WPARAM wParam, LPARAM lParam) {
	auto keyBit = [](WPARAM vk) -> uint32_t {
		switch (vk) {
		case 'W': case VK_UP: return kKeyForward;
		case 'A': case VK_LEFT: return kKeyLeft;
		case 'S': case VK_DOWN: return kKeyBack;
		case 'D': case VK_RIGHT: return kKeyRight;
		case 'Q': case 'C': return kKeyDown;
		case 'E': case VK_SPACE: return kKeyUp;
		case VK_SHIFT: return kKeyFast;
		case VK_CONTROL: return kKeySlow;
		case 'R': return kKeyResetFov;
		default: return 0;
		}
	};
	switch (msg) {
	case WM_KEYDOWN:
	case WM_SYSKEYDOWN:
		freeCameraKeys_.fetch_or(keyBit(wParam));
		return true;
	case WM_KEYUP:
	case WM_SYSKEYUP:
		freeCameraKeys_.fetch_and(~keyBit(wParam));
		return true;
	case WM_CHAR:
	case WM_SYSCHAR:
	case WM_DEADCHAR:
		return true;
	case WM_KILLFOCUS:
		freeCameraKeys_.store(0); // (a key let go in another window never comes back as a key up)
		return false;
	case WM_INPUT: {
		// relative motion straight from the mouse (the game registered for raw input): not stopped by the screen's edge
		UINT size = 0;
		GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
		alignas(8) BYTE buffer[256];
		if (size > 0 && size <= sizeof(buffer) && GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) == size) {
			const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
			if (raw->header.dwType == RIM_TYPEMOUSE && (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
				mouseX_.fetch_add(raw->data.mouse.lLastX);
				mouseY_.fetch_add(raw->data.mouse.lLastY);
				if (!rawMouse_.exchange(true)) {
					AE_LOG(Info, "Overlay", "Free camera: the mouse's raw input is used for looking round");
				}
			}
		}
		return true;
	}
	case WM_MOUSEMOVE: {
		// (without raw input: the cursor's motion, ignoring jumps such as the game putting it back in the middle)
		const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
		// (without raw input the cursor is read and put back in the middle once a frame: TakeFreeCameraInput)
		lastCursorX_ = x;
		lastCursorY_ = y;
		return true;
	}
	case WM_MOUSEWHEEL:
		wheel_.fetch_add(GET_WHEEL_DELTA_WPARAM(wParam));
		return true;
	default:
		return IsMouseMessage(msg) && msg != WM_MOUSELEAVE && msg != WM_NCMOUSELEAVE && msg != WM_NCMOUSEMOVE;
	}
}

// ------------------------------------------------------------------------------------------------ window procedure

void Overlay::AttachWindow(HWND hwnd) {
	if (!hwnd || hooked_ == hwnd) {
		return;
	}
	if (GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) {
		return; // only from the thread that owns the window
	}
	DetachWindow();
	SetLastError(0);
	LONG_PTR previous = SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Overlay::WndProc));
	if (previous == 0) {
		AE_LOG_CODE(Error, "Overlay", "E_WNDPROC", "Could not subclass the game window (error %lu): the overlay has no input", GetLastError());
		return;
	}
	previousProc_ = reinterpret_cast<WNDPROC>(previous);
	hooked_ = hwnd;
	{
		const SettingsData s = Settings::Get().Snapshot();
		hotkey_.store(s.overlayHotkey);
		freeCameraHotkey_.store(s.freeCameraHotkey);
	}
	AE_LOG(Info, "Overlay", "Input attached to the game window (hotkey: virtual key %d)", hotkey_.load());
}

void Overlay::DetachWindow() {
	if (!hooked_) {
		return;
	}
	if (IsWindow(hooked_)) {
		LONG_PTR current = GetWindowLongPtrW(hooked_, GWLP_WNDPROC);
		if (current == reinterpret_cast<LONG_PTR>(&Overlay::WndProc)) {
			SetWindowLongPtrW(hooked_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousProc_));
		} else {
			// someone subclassed the window after Aether: taking Aether's procedure out of the middle of the chain is
			// not possible, so it stays as a pass-through (the DLL is never unloaded while the game runs)
			AE_LOG(Warning, "Overlay", "Another window procedure was installed after Aether's: Aether's stays in the chain as a pass-through");
			SetVisible(false);
			return;
		}
	}
	hooked_ = nullptr;
	previousProc_ = nullptr;
}

LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	return Overlay::Get().HandleMessage(hwnd, msg, wParam, lParam);
}

static bool IsMouseMessage(UINT msg) {
	return (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_MOUSELEAVE || msg == WM_NCMOUSEMOVE || msg == WM_NCMOUSELEAVE;
}

static bool IsKeyboardMessage(UINT msg) {
	return msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR;
}

LRESULT Overlay::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
	WNDPROC next = previousProc_;
	if (!next) {
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
	if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && int(wParam) == hotkey_.load()) {
		if (!(lParam & (1 << 30))) { // not an auto-repeat
			if (GetKeyState(VK_SHIFT) & 0x8000) {
				ToggleDebug();
			} else {
				Toggle();
			}
		}
		return 0;
	}
	if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && int(wParam) == freeCameraHotkey_.load()) {
		if (!(lParam & (1 << 30))) {
			const bool on = !freeCamera_.load();
			freeCameraKeys_.store(0);
			mouseX_.store(0);
			mouseY_.store(0);
			wheel_.store(0);
			lastCursorX_ = lastCursorY_ = INT_MIN;
			cursorCentred_ = false;
			freeCamera_.store(on);
			AE_LOG(Info, "Overlay", "Free camera %s", on ? "on" : "off");
		}
		return 0;
	}
	if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && (int(wParam) == hotkey_.load() || int(wParam) == freeCameraHotkey_.load())) {
		return 0;
	}
	if (FreeCameraLive() && HandleFreeCamera(msg, wParam, lParam)) {
		return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wParam, lParam) : 0; // (raw input must still be cleaned up)
	}
	if (!Visible() && !Runtime::Get().ApprovalPending()) {
		return CallWindowProcW(next, hwnd, msg, wParam, lParam);
	}
	const bool mouse = IsMouseMessage(msg), keyboard = IsKeyboardMessage(msg);
	if (mouse || keyboard || msg == WM_SETFOCUS || msg == WM_KILLFOCUS || msg == WM_INPUTLANGCHANGE) {
		std::lock_guard lock(queueMutex_);
		if (queue_.size() < 512) {
			queue_.push_back({hwnd, msg, wParam, lParam});
		}
	}
	// what the overlay uses does not reach the game
	if ((mouse && msg != WM_MOUSELEAVE && msg != WM_NCMOUSELEAVE && wantsMouse_.load()) || (keyboard && wantsKeyboard_.load())) {
		return 0;
	}
	if (msg == WM_INPUT && (wantsMouse_.load() || wantsKeyboard_.load())) {
		return DefWindowProcW(hwnd, msg, wParam, lParam); // raw input must still be cleaned up
	}
	return CallWindowProcW(next, hwnd, msg, wParam, lParam);
}

// ------------------------------------------------------------------------------------------------ device objects

void Overlay::SrvAlloc(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) {
	Overlay* self = static_cast<Overlay*>(info->UserData);
	if (self->srvFree_.empty()) {
		AE_LOG_CODE(Error, "Overlay", "E_DESCRIPTOR_HEAP_FULL", "The overlay's SRV heap is full (%u descriptors)", kSrvCount);
		*cpu = self->srvHeap_->GetCPUDescriptorHandleForHeapStart();
		*gpu = self->srvHeap_->GetGPUDescriptorHandleForHeapStart();
		return;
	}
	UINT index = self->srvFree_.back();
	self->srvFree_.pop_back();
	cpu->ptr = self->srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr + SIZE_T(index) * self->srvSize_;
	gpu->ptr = self->srvHeap_->GetGPUDescriptorHandleForHeapStart().ptr + UINT64(index) * self->srvSize_;
}

void Overlay::SrvFree(ImGui_ImplDX12_InitInfo* info, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
	Overlay* self = static_cast<Overlay*>(info->UserData);
	if (!self->srvHeap_) {
		return;
	}
	UINT index = UINT((cpu.ptr - self->srvHeap_->GetCPUDescriptorHandleForHeapStart().ptr) / self->srvSize_);
	self->srvFree_.push_back(index);
}

void Overlay::ShutdownBackends() {
	if (backends_) {
		ImGui_ImplDX12_Shutdown();
		ImGui_ImplWin32_Shutdown();
		backends_ = false;
	}
	srvHeap_.Reset();
	srvFree_.clear();
	draws_ = 0;
	drawPending_ = false;
	for (UINT64& f : slotFence_) {
		f = 0;
	}
}

bool Overlay::EnsureContext(const HostGfx& host, HWND hwnd, DXGI_FORMAT rtvFormat) {
	if (!hwnd) {
		return false;
	}
	if (!context_) {
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		static std::string iniPath; // ImGui keeps the pointer
		iniPath = WideToUtf8(Runtime::Get().DataDir() + L"\\imgui.ini");
		io.IniFilename = iniPath.c_str();
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		io.MouseDrawCursor = true; // the game hides the system cursor while driving
		context_ = true;
		appliedScale_ = 0;
		fontsLoaded_ = false;
	}
	if (backends_ && (backendWindow_ != hwnd || backendFormat_ != rtvFormat || device_ != host.device)) {
		// only reached with the GPU idle on the old objects? Not guaranteed: wait for the slots first
		UINT64 completed = host.completedFenceValue();
		for (UINT64 f : slotFence_) {
			if (f > completed) {
				return false; // try again next frame
			}
		}
		ShutdownBackends();
	}
	if (!backends_) {
		device_ = host.device;
		D3D12_DESCRIPTOR_HEAP_DESC desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
		HRESULT hr = device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&srvHeap_));
		if (FAILED(hr)) {
			AE_LOG_CODE(Error, "Overlay", "E_DEVICE_OBJECTS", "The overlay's descriptor heap could not be created: %s", HrToString(hr).c_str());
			return false;
		}
		srvHeap_->SetName(L"Aether.Overlay.SrvHeap");
		srvSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		srvFree_.clear();
		for (UINT i = kSrvCount; i > 0; --i) {
			srvFree_.push_back(i - 1);
		}
		if (!ImGui_ImplWin32_Init(hwnd)) {
			srvHeap_.Reset();
			return false;
		}
		// The IME hook would call into the window's thread from the render thread: off.
		ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;
		ImGui_ImplDX12_InitInfo info;
		info.Device = device_;
		info.CommandQueue = host.queue;
		info.NumFramesInFlight = kFramesInFlight;
		info.RTVFormat = rtvFormat;
		info.DSVFormat = DXGI_FORMAT_UNKNOWN;
		info.UserData = this;
		info.SrvDescriptorHeap = srvHeap_.Get();
		info.SrvDescriptorAllocFn = &Overlay::SrvAlloc;
		info.SrvDescriptorFreeFn = &Overlay::SrvFree;
		if (!ImGui_ImplDX12_Init(&info)) {
			ImGui_ImplWin32_Shutdown();
			srvHeap_.Reset();
			AE_LOG_CODE(Error, "Overlay", "E_DEVICE_OBJECTS", "The Dear ImGui DX12 backend could not start");
			return false;
		}
		backends_ = true;
		backendWindow_ = hwnd;
		backendFormat_ = rtvFormat;
		draws_ = 0;
		AE_LOG(Info, "Overlay", "Dear ImGui %s started (DX12 + Win32 backends, render target format %d)", IMGUI_VERSION, int(rtvFormat));
	}
	return true;
}

void Overlay::NoteSubmitted(UINT64 fence) {
	std::lock_guard lock(mutex_);
	if (drawPending_) {
		slotFence_[(draws_ - 1) % kFramesInFlight] = fence;
		drawPending_ = false;
	}
}

void Overlay::ReleaseDeviceObjects(bool leak) {
	std::lock_guard lock(mutex_);
	if (leak) {
		// the backend's buffers may still be read by the GPU: forget them without releasing
		srvHeap_.Detach();
		backends_ = false;
		if (context_) {
			ImGuiIO& io = ImGui::GetIO();
			io.BackendRendererUserData = nullptr;
			io.BackendPlatformUserData = nullptr;
			context_ = false; // the context itself is leaked too (it still points at backend data)
			ImGui::SetCurrentContext(nullptr);
		}
		return;
	}
	if (backends_) {
		ShutdownBackends();
	}
	if (context_) {
		ImGui::DestroyContext();
		context_ = false;
	}
	device_ = nullptr;
	backendWindow_ = nullptr;
	backendFormat_ = DXGI_FORMAT_UNKNOWN;
}

void LoadOverlayFonts();                  // (OverlayUi below)
void ApplyOverlayStyle(ImGuiStyle& style);
void SetOverlayLayoutScale(float scale);

bool Overlay::Draw(const HostGfx& host, HWND hwnd, DXGI_FORMAT rtvFormat, UINT width, UINT height, ID3D12GraphicsCommandList* list, UINT64 completedFence, uint32_t& drawCalls, std::string& note) {
	std::lock_guard lock(mutex_);
	if (!EnsureContext(host, hwnd, rtvFormat)) {
		note = "the overlay could not start (see the log)";
		return false;
	}
	// the DX12 backend reuses (and may free) the buffers of the draw kFramesInFlight frames ago
	if (slotFence_[draws_ % kFramesInFlight] > completedFence) {
		note = "waiting for the GPU to finish an older overlay frame";
		return false;
	}

	SettingsData s = Settings::Get().Snapshot();
	hotkey_.store(s.overlayHotkey);
	freeCameraHotkey_.store(s.freeCameraHotkey);
	ImGuiIO& io = ImGui::GetIO();
	hud_.store(s.overlayHud);
	io.MouseDrawCursor = Visible(); // (the game hides the system cursor while driving; no cursor over the free camera's view)
	if (!fontsLoaded_) {
		LoadOverlayFonts();
		fontsLoaded_ = true;
	}
	// the UI scale is for a 1440 pixel high screen: taller screens get it larger by their height
	const float scale = s.overlayScale * std::max(1.0f, float(height) / 1440.0f);
	if (appliedScale_ != scale || appliedTextScale_ != s.overlayTextScale) {
		ImGuiStyle style;
		ApplyOverlayStyle(style);
		style.ScaleAllSizes(scale);
		style.FontScaleMain = scale * s.overlayTextScale; // (the text size option scales the text only)
		ImGui::GetStyle() = style;
		appliedScale_ = scale;
		appliedTextScale_ = s.overlayTextScale;
	}
	SetOverlayLayoutScale(scale);

	// input the window procedure queued on the main thread
	std::vector<Message> messages;
	{
		std::lock_guard qlock(queueMutex_);
		messages.swap(queue_);
	}
	for (const Message& m : messages) {
		ImGui_ImplWin32_WndProcHandler(m.hwnd, m.msg, m.wParam, m.lParam);
	}
	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	// the Win32 backend reads the modifier keys with GetKeyState, which is per thread: set them from the async state
	io.AddKeyEvent(ImGuiMod_Ctrl, (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0);
	io.AddKeyEvent(ImGuiMod_Shift, (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
	io.AddKeyEvent(ImGuiMod_Alt, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0);
	io.DisplaySize = ImVec2(float(width), float(height));
	ImGui::NewFrame();
	BuildUi(width, height);
	ImGui::Render();
	wantsMouse_.store(io.WantCaptureMouse);
	wantsKeyboard_.store(io.WantCaptureKeyboard);

	ImDrawData* data = ImGui::GetDrawData();
	ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
	list->SetDescriptorHeaps(1, heaps);
	ImGui_ImplDX12_RenderDrawData(data, list);
	drawCalls = 0;
	for (ImDrawList* dl : data->CmdLists) {
		drawCalls += uint32_t(dl->CmdBuffer.Size);
	}
	++draws_;
	drawPending_ = true;
	return true;
}

// ------------------------------------------------------------------------------------------------ the windows

namespace {

std::string Bytes(uint64_t b) {
	if (b >= (1ull << 30)) {
		return Format("%.2f GB", double(b) / double(1ull << 30));
	}
	if (b >= (1ull << 20)) {
		return Format("%.1f MB", double(b) / double(1ull << 20));
	}
	if (b >= (1ull << 10)) {
		return Format("%.1f KB", double(b) / double(1ull << 10));
	}
	return Format("%llu B", static_cast<unsigned long long>(b));
}

// Data that is not worth fetching every frame.
struct UiCache {
	double nextSlow = 0, nextLog = 0, nextManaged = 0;
	json gpu, scene, materials, renderTextures, managed, shaders;
	json sceneRenderer, sceneGraph, sceneStore, gpuManager; // Aether's own scene path
	double sceneAge = -1, materialsAge = -1, rtAge = -1;
	std::vector<LogEntry> logs;
	VideoMemory vm;
	std::vector<ResourceRecord> resources;
	std::vector<float> intervals, gpuMs;
	uint64_t lastCapture = 0;
	int logSeverity = 1;
	bool logAutoScroll = true;
};
UiCache g_ui;

void JsonTree(const char* label, const json& j, int depth = 0) {
	if (j.is_object() || j.is_array()) {
		if (depth > 0 && !ImGui::TreeNode(label, "%s (%zu)", label, j.size())) {
			return;
		}
		int index = 0;
		for (auto it = j.begin(); it != j.end(); ++it, ++index) {
			std::string key = j.is_object() ? it.key() : Format("[%d]", index);
			if (j.is_array() && it->is_object()) {
				// name array elements after a telling member
				for (const char* k : {"name", "shader", "key", "path"}) {
					if (it->contains(k) && (*it)[k].is_string()) {
						key += " " + (*it)[k].get<std::string>();
						break;
					}
				}
			}
			ImGui::PushID(index);
			JsonTree(key.c_str(), *it, depth + 1);
			ImGui::PopID();
			if (index >= 400) {
				ImGui::TextDisabled("... %zu more", j.size() - size_t(index) - 1);
				break;
			}
		}
		if (depth > 0) {
			ImGui::TreePop();
		}
		return;
	}
	std::string value = j.is_string() ? j.get<std::string>() : Dump(j);
	ImGui::BulletText("%s: %s", label, value.c_str());
}

void KeyValue(const char* key, const std::string& value) {
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::TextUnformatted(key);
	ImGui::TableSetColumnIndex(1);
	ImGui::TextUnformatted(value.c_str());
}

void UnityRow(const char* label, int index, bool bytes = false) {
	double v = Profiler::Get().UnityValue(index);
	if (v < 0) {
		return;
	}
	KeyValue(label, bytes ? Bytes(uint64_t(v)) : (v == double(int64_t(v)) ? Format("%lld", static_cast<long long>(v)) : Format("%.2f", v)));
}

// ------------------------------------------------------------------------------------------------ look (the Aether Loader's)

ImVec4 Rgb(uint32_t c, float a = 1.0f) {
	return ImVec4(float((c >> 16) & 255) / 255.0f, float((c >> 8) & 255) / 255.0f, float(c & 255) / 255.0f, a);
}
enum : uint32_t {
	kBackground = 0x1E1F22, kPanel = 0x26282C, kRail = 0x222326, kLine = 0x34363B, kHover = 0x2F3237, kChosen = 0x26395C,
	kAccent = 0x2F6BE0, kAccentHover = 0x3D78EA, kText = 0xDFE1E5, kSecondary = 0x9DA0A8, kField = 0x2B2D31, kFieldHover = 0x33363B,
	kWarning = 0xE5B454, kError = 0xF08080, kGood = 0x7CC48A,
};
ImFont* g_bold = nullptr;  // Segoe UI Semibold (null: the regular font)
ImFont* g_mono = nullptr;  // Consolas, for paths and ids
float g_layout = 1.0f;     // the UI scale (overlayScale), without the text size option

float Sc() { return g_layout; }
ImU32 U(uint32_t c, float a = 1.0f) { return ImGui::GetColorU32(Rgb(c, a)); }

void Secondary(const char* text) {
	ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kSecondary));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
}

void Heading(const char* text, float size = 1.0f) {
	ImGui::PushFont(g_bold, ImGui::GetStyle().FontSizeBase * size);
	ImGui::TextUnformatted(text);
	ImGui::PopFont();
}

// a framed line of text in a colour (errors, warnings)
void Banner(uint32_t color, const char* text) {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgb(color, 0.10f));
	ImGui::PushStyleColor(ImGuiCol_Border, Rgb(color, 0.45f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * Sc(), 8 * Sc()));
	ImGui::BeginChild(text, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PushStyleColor(ImGuiCol_Text, Rgb(color));
	ImGui::TextWrapped("%s", text);
	ImGui::PopStyleColor();
	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(2);
}

void Banners() {
	if (Renderer::Get().Failed()) {
		Banner(kError, "Aether's rendering stopped after an error. The debug tools (Shift+F10) have the details under Render and Log.");
	}
	if (Runtime::Get().PreviousSessionCrashed()) {
		Banner(kWarning, "The previous session did not shut down cleanly (get_crash_report has the details).");
	}
}

bool g_rows = false; // the card's table of rows is open

// A card: a framed panel with a title, and a two column table of rows (label left, control right).
void CardBegin(const char* title, const char* note = nullptr, bool rows = true) {
	ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgb(kPanel));
	ImGui::PushStyleColor(ImGuiCol_Border, Rgb(kLine));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * Sc(), 12 * Sc()));
	ImGui::BeginChild(title, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(2);
	Heading(title);
	if (note) {
		Secondary(note);
	}
	ImGui::Dummy(ImVec2(0, 2 * Sc()));
	// (BeginTable is false while the card is scrolled out of view: the rows then add nothing)
	g_rows = rows && ImGui::BeginTable("rows", 2, ImGuiTableFlags_SizingStretchProp);
	if (g_rows) {
		ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, 240 * Sc());
	}
}

void CardEnd(bool = true) {
	if (g_rows) {
		ImGui::EndTable();
		g_rows = false;
	}
	ImGui::EndChild();
	ImGui::Dummy(ImVec2(0, 4 * Sc()));
}

// a row's label (and its help below it, smaller and quieter); leaves the cursor in the control column
void Label(const char* label, const char* help) {
	if (!g_rows) {
		return; // (the card is out of view: its widgets are skipped by ImGui)
	}
	ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(0);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	if (help) {
		ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.9f);
		Secondary(help);
		ImGui::PopFont();
	}
	ImGui::TableSetColumnIndex(1);
	ImGui::SetNextItemWidth(-FLT_MIN);
}

// the Loader's switch: a rounded track, the knob right when on
bool SwitchWidget(const char* id, bool value) {
	const float h = 16 * Sc(), w = h * 30.0f / 16.0f;
	const float frame = ImGui::GetFrameHeight();
	ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, frame));
	const bool hovered = ImGui::IsItemHovered();
	const ImVec2 a(p.x, p.y + (frame - h) * 0.5f), b(p.x + w, a.y + h);
	const float r = h * 0.5f, knob = r - std::max(2.0f, h / 7.0f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, b, value ? U(hovered ? kAccentHover : kAccent) : U(hovered ? 0x52555C : 0x45484E), r);
	dl->AddCircleFilled(ImVec2(value ? b.x - r : a.x + r, a.y + r), knob, value ? U(0xFFFFFF) : U(0xD0D2D6));
	return pressed;
}

// rows that change one setting each; they return the value now in effect (for rows that depend on it)
bool SwitchRow(const char* label, const char* help, const char* key, bool value) {
	Label(label, help);
	if (SwitchWidget(key, value)) {
		value = !value;
		Settings::Get().Set(key, value, "overlay");
	}
	return value;
}

void SliderRow(const char* label, const char* help, const char* key, float value, float lo, float hi, const char* format, ImGuiSliderFlags flags = 0) {
	Label(label, help);
	ImGui::PushID(key);
	if (ImGui::SliderFloat("##v", &value, lo, hi, format, flags)) {
		Settings::Get().Set(key, value, "overlay");
	}
	ImGui::PopID();
}

void IntSliderRow(const char* label, const char* help, const char* key, int value, int lo, int hi, const char* format = "%d") {
	Label(label, help);
	ImGui::PushID(key);
	if (ImGui::SliderInt("##v", &value, lo, hi, format)) {
		Settings::Get().Set(key, value, "overlay");
	}
	ImGui::PopID();
}

// a choice stored as a string (values[i] shown as texts[i])
void ChoiceRow(const char* label, const char* help, const char* key, const std::string& current, const char* const* values, const char* const* texts, int count) {
	Label(label, help);
	int at = 0;
	for (int i = 0; i < count; ++i) {
		if (current == values[i]) {
			at = i;
		}
	}
	ImGui::PushID(key);
	if (ImGui::BeginCombo("##v", texts[at])) {
		for (int i = 0; i < count; ++i) {
			if (ImGui::Selectable(texts[i], i == at)) {
				Settings::Get().Set(key, values[i], "overlay");
			}
		}
		ImGui::EndCombo();
	}
	ImGui::PopID();
}

// a text value in the control column (read only)
void ValueRow(const char* label, const char* help, const std::string& value) {
	Label(label, help);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(value.c_str());
}

std::string KeyName(int vk) {
	if (vk >= VK_F1 && vk <= VK_F24) {
		return Format("F%d", vk - VK_F1 + 1);
	}
	UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
	wchar_t name[64] = {};
	if (scan && GetKeyNameTextW(LONG(scan << 16), name, 64) > 0) {
		return WideToUtf8(name);
	}
	return Format("key 0x%02X", vk);
}

// ------------------------------------------------------------------------------------------------ quality presets

// What a preset sets: the switches and levels that decide the cost of a frame. Colour, sky, camera effects and the
// overlay's own options are left as they are.
struct Preset {
	const char* name;
	const char* text;
	std::vector<std::pair<const char*, json>> values;
};

const std::vector<Preset>& Presets() {
	static const std::vector<Preset> presets = {
		{"low", "Low", {
			{"quality", "low"}, {"shadows", true}, {"ssao", true}, {"antialiasing", true}, {"ssr", false}, {"reflectionProbe", false},
			{"rayTracedReflections", false}, {"reflectionDenoiser", "off"}, {"rayTracedLighting", false}, {"radianceCascades", false},
			{"probeGi", false}, {"rayTracedGi", false}, {"indirectLight", false}, {"volumetricFog", false}, {"reflectionResolution", "half"}, {"skinnedRayUpdates", 0}, {"lampShadows", false}, {"anisotropicLevel", 4}, {"lodBias", 1.5}, {"pathTracing", "freeCamera"}}},
		{"medium", "Medium", {
			{"quality", "medium"}, {"shadows", true}, {"ssao", true}, {"antialiasing", true}, {"ssr", true}, {"reflectionProbe", true},
			{"rayTracedReflections", false}, {"reflectionDenoiser", "off"}, {"rayTracedLighting", false}, {"radianceCascades", false},
			{"probeGi", true}, {"rayTracedGi", false}, {"indirectLight", true}, {"volumetricFog", true}, {"reflectionResolution", "half"}, {"skinnedRayUpdates", 0}, {"lampShadows", false}, {"anisotropicLevel", 8}, {"lodBias", 2.0}, {"pathTracing", "freeCamera"}}},
		{"high", "High", {
			{"quality", "high"}, {"shadows", true}, {"ssao", true}, {"antialiasing", true}, {"ssr", true}, {"reflectionProbe", true},
			{"rayTracedReflections", true}, {"reflectionDenoiser", "nrd"}, {"rayTracedLighting", true}, {"radianceCascades", true},
			{"probeGi", true}, {"rayTracedGi", false}, {"indirectLight", true}, {"volumetricFog", true}, {"reflectionResolution", "full"}, {"skinnedRayUpdates", 8}, {"lampShadows", true}, {"anisotropicLevel", 8}, {"lodBias", 2.8}, {"pathTracing", "freeCamera"}}},
		{"ultra", "Ultra", {
			{"quality", "ultra"}, {"shadows", true}, {"ssao", true}, {"antialiasing", true}, {"ssr", true}, {"reflectionProbe", true},
			{"rayTracedReflections", true}, {"reflectionDenoiser", "nrd"}, {"rayTracedLighting", true}, {"radianceCascades", true},
			{"probeGi", true}, {"rayTracedGi", true}, {"indirectLight", true}, {"volumetricFog", true}, {"reflectionResolution", "full"}, {"skinnedRayUpdates", 8}, {"lampShadows", true}, {"anisotropicLevel", 16}, {"lodBias", 4.0}, {"pathTracing", "freeCamera"}}},
		// Ultra with the screen space and raster stand-ins off: reflections only from rays (no screen space pass, no
		// probe cube), the ambient light only from long rays per pixel (no screen space bounce). The sun's shadow map
		// stays: it is used for casters the rays do not see and for the fog's light shafts.
		{"fullRaytrace", "Full Raytrace", {
			{"quality", "ultra"}, {"shadows", true}, {"ssao", true}, {"antialiasing", true}, {"ssr", false}, {"reflectionProbe", false},
			{"rayTracedReflections", true}, {"reflectionDenoiser", "nrd"}, {"rayTracedLighting", true}, {"radianceCascades", true},
			{"probeGi", true}, {"rayTracedGi", true}, {"indirectLight", false}, {"volumetricFog", true}, {"reflectionResolution", "full"}, {"skinnedRayUpdates", 8}, {"lampShadows", true}, {"anisotropicLevel", 16}, {"lodBias", 4.0}, {"pathTracing", "freeCamera"}}},
		// Full Raytrace, and the whole picture path traced all the time (not only with the free camera), denoised by DLSS
		// Ray Reconstruction: the most accurate light, at a fraction of the frame rate
		{"pathTracing", "Path Tracing", {
			{"quality", "ultra"}, {"shadows", true}, {"ssao", true}, {"antialiasing", true}, {"ssr", false}, {"reflectionProbe", false},
			{"rayTracedReflections", true}, {"reflectionDenoiser", "nrd"}, {"rayTracedLighting", true}, {"radianceCascades", true},
			{"probeGi", true}, {"rayTracedGi", true}, {"indirectLight", false}, {"volumetricFog", true}, {"reflectionResolution", "full"}, {"skinnedRayUpdates", 8}, {"lampShadows", true}, {"anisotropicLevel", 16}, {"lodBias", 4.0},
			{"pathTracing", "always"}, {"pathTracingDenoiser", "dlss_rr"}}},
	};
	return presets;
}

bool SameValue(const json& a, const json& b) {
	if (a.is_number() && b.is_number()) {
		return std::abs(a.get<double>() - b.get<double>()) < 1e-3;
	}
	return a == b;
}

// the preset the settings match now, or -1 (custom)
int CurrentPreset(const json& values) {
	const auto& presets = Presets();
	for (size_t i = 0; i < presets.size(); ++i) {
		bool all = true;
		for (const auto& [key, value] : presets[i].values) {
			all = all && values.contains(key) && SameValue(values[key], value);
		}
		if (all) {
			return int(i);
		}
	}
	return -1;
}

void ApplyPreset(const Preset& p) {
	for (const auto& [key, value] : p.values) {
		std::string why = Settings::Get().Set(key, value, "overlay");
		if (!why.empty()) {
			AE_LOG(Warning, "Overlay", "Preset %s: %s", p.name, why.c_str());
		}
	}
	AE_LOG(Info, "Overlay", "Quality preset %s applied", p.name);
}

// four buttons side by side, the chosen one filled with the accent
void PresetButtons() {
	const json values = Settings::Get().ToJson();
	const int current = CurrentPreset(values);
	const auto& presets = Presets();
	const float gap = 6 * Sc();
	const float w = (ImGui::GetContentRegionAvail().x - gap * float(presets.size() - 1)) / float(presets.size());
	for (size_t i = 0; i < presets.size(); ++i) {
		if (i) {
			ImGui::SameLine(0, gap);
		}
		const bool chosen = int(i) == current;
		ImGui::PushStyleColor(ImGuiCol_Button, chosen ? Rgb(kAccent) : Rgb(kField));
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, chosen ? Rgb(kAccentHover) : Rgb(kFieldHover));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, chosen ? Rgb(kAccent) : Rgb(kChosen));
		ImGui::PushStyleColor(ImGuiCol_Text, chosen ? Rgb(0xFFFFFF) : Rgb(kText));
		if (ImGui::Button(presets[i].text, ImVec2(w, ImGui::GetFrameHeight() * 1.3f))) {
			ApplyPreset(presets[i]);
		}
		ImGui::PopStyleColor(4);
	}
	ImGui::Dummy(ImVec2(0, 2 * Sc()));
	if (current < 0) {
		Secondary("Custom: your settings differ from every preset. Pick one to start again from it.");
	} else {
		Secondary(Format("%s is in use.%s", presets[current].text, std::string(presets[current].name) == "fullRaytrace" ? " Shadows, occlusion, reflections and ambient light all come from rays; screen space reflections, the reflection probe and screen space bounce light are off." : std::string(presets[current].name) == "pathTracing" ? " The whole picture is path traced all the time and denoised by DLSS Ray Reconstruction: the most accurate light, at a far lower frame rate (about 25 fps at 4K on an RTX 4070 Ti). Glass, smoke and particles are drawn over it as usual." : "").c_str());
	}
}

void PerformanceTab() {
	Profiler& prof = Profiler::Get();
	const auto& iv = g_ui.intervals;
	double sum = 0;
	size_t n = 0;
	float worst = 0;
	for (size_t i = iv.size() > 120 ? iv.size() - 120 : 0; i < iv.size(); ++i, ++n) {
		sum += iv[i];
		worst = std::max(worst, iv[i]);
	}
	double avg = n ? sum / double(n) : 0;
	ImGui::Text("FPS %.1f    frame %.2f ms    worst (120 frames) %.2f ms", avg > 0 ? 1000.0 / avg : 0.0, avg, worst);
	if (!iv.empty()) {
		ImGui::PlotLines("##frametime", iv.data(), int(iv.size()), 0, "frame time (ms)", 0.0f, std::max(33.3f, worst * 1.1f), ImVec2(-1, 90));
		ImGui::PlotLines("##gputime", g_ui.gpuMs.data(), int(g_ui.gpuMs.size()), 0, "GPU frame time (ms)", 0.0f, std::max(33.3f, worst * 1.1f), ImVec2(-1, 70));
	}
	if (ImGui::BeginTable("perf", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		UnityRow("Unity CPU main thread (ms)", kUnityCpuMainThreadMs);
		UnityRow("Unity CPU render thread (ms)", kUnityCpuRenderThreadMs);
		UnityRow("Unity present wait (ms)", kUnityCpuPresentWaitMs);
		UnityRow("Unity GPU frame (ms)", kUnityGpuFrameMs);
		UnityRow("Unity draw calls", kUnityDrawCalls);
		UnityRow("Unity batches", kUnityBatches);
		UnityRow("Unity SetPass calls", kUnitySetPassCalls);
		UnityRow("Unity triangles", kUnityTriangles);
		UnityRow("Unity vertices", kUnityVertices);
		UnityRow("Shadow casters", kUnityShadowCasters);
		ImGui::EndTable();
	}
	json bench = prof.BenchmarkStatus();
	std::string state = bench.value("state", "idle");
	if (state == "running") {
		ImGui::ProgressBar(float(bench.value("elapsedSeconds", 0.0) / std::max(0.1, bench.value("requestedSeconds", 1.0))), ImVec2(-1, 0), "benchmark running");
		if (ImGui::Button("Stop benchmark")) {
			prof.StopBenchmark();
		}
	} else {
		if (ImGui::Button("Run 10 s benchmark")) {
			prof.StartBenchmark("overlay", 10.0, Runtime::Get().Subdir(L"benchmarks"), {{"startedBy", "overlay"}, {"scene", Runtime::Get().ManagedState("scene")}});
		}
		if (state == "done") {
			const json& r = bench["result"];
			ImGui::SameLine();
			ImGui::Text("last: %.1f fps avg, %.1f fps 1%% low, %zu frames", r.value("averageFps", 0.0), r.value("onePercentLowFps", 0.0), r.value("frames", size_t(0)));
		}
	}
}

void GpuTab() {
	const json& g = g_ui.gpu;
	if (!g.value("available", false)) {
		ImGui::TextUnformatted("The D3D12 device has not been seen yet.");
		return;
	}
	if (g.contains("adapter") && ImGui::BeginTable("adapter", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		const json& a = g["adapter"];
		KeyValue("Vendor", a.value("vendor", ""));
		KeyValue("Name", a.value("name", ""));
		KeyValue("Device ID", a.value("deviceId", ""));
		KeyValue("Driver", a.value("driverVersion", ""));
		KeyValue("Dedicated VRAM", Bytes(a.value("dedicatedVideoMemoryBytes", uint64_t(0))));
		KeyValue("Shared system memory", Bytes(a.value("sharedSystemMemoryBytes", uint64_t(0))));
		KeyValue("Device state", g.value("deviceState", ""));
		ImGui::EndTable();
	}
	if (g.contains("features") && ImGui::CollapsingHeader("D3D12 feature support", ImGuiTreeNodeFlags_DefaultOpen)) {
		JsonTree("features", g["features"]);
	}
}

void VramTab() {
	const VideoMemory& vm = g_ui.vm;
	ResourceRegistry& reg = ResourceRegistry::Get();
	uint64_t aetherGpu = reg.GpuBytes();
	if (vm.valid) {
		float fraction = vm.localBudget ? float(double(vm.localUsage) / double(vm.localBudget)) : 0.0f;
		ImGui::Text("Dedicated VRAM (this process)");
		ImGui::ProgressBar(fraction, ImVec2(-1, 0), Format("%s / %s budget", Bytes(vm.localUsage).c_str(), Bytes(vm.localBudget).c_str()).c_str());
		ImGui::Text("Shared GPU memory: %s / %s budget", Bytes(vm.nonLocalUsage).c_str(), Bytes(vm.nonLocalBudget).c_str());
	} else {
		ImGui::TextUnformatted("Process video memory is not available yet.");
	}
	ImGui::Separator();
	if (ImGui::BeginTable("vram", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		KeyValue("Aether GPU resources", Bytes(aetherGpu));
		KeyValue("Aether upload / readback (system memory)", Bytes(reg.TotalBytes() - aetherGpu));
		if (vm.valid) {
			KeyValue("Not Aether (Unity / HDRP, assets, driver)", Bytes(vm.localUsage > aetherGpu ? vm.localUsage - aetherGpu : 0));
		}
		for (int c = 0; c < int(ResourceCategory::Count); ++c) {
			uint64_t b = reg.CategoryBytes(ResourceCategory(c));
			if (b) {
				KeyValue(Format("  Aether %s", CategoryName(ResourceCategory(c))).c_str(), Bytes(b));
			}
		}
		UnityRow("Unity render textures", kUnityRenderTexturesBytes, true);
		UnityRow("Unity render texture count", kUnityRenderTexturesCount);
		UnityRow("Unity textures in use", kUnityUsedTexturesBytes, true);
		UnityRow("Unity texture count", kUnityUsedTexturesCount);
		UnityRow("Unity buffers in use", kUnityUsedBuffersBytes, true);
		UnityRow("Texture streaming: current", kUnityTextureStreamingCurrentBytes, true);
		UnityRow("Texture streaming: all mips", kUnityTextureTotalBytes, true);
		UnityRow("Unity graphics driver memory", kUnityGfxDriverBytes, true);
		ImGui::EndTable();
	}
	if (ImGui::CollapsingHeader("Aether GPU resource manager (descriptors, frame memory, budget)")) {
		JsonTree("manager", g_ui.gpuManager);
	}
	if (ImGui::CollapsingHeader("Unity render targets (HDRP side)")) {
		if (ImGui::Button("Refresh")) {
			Runtime::Get().RequestManaged(kRequestRenderTextures);
		}
		if (g_ui.renderTextures.is_null()) {
			ImGui::TextDisabled("Not collected yet: press Refresh.");
		} else {
			ImGui::SameLine();
			ImGui::TextDisabled("%.0f s old", g_ui.rtAge);
			JsonTree("renderTextures", g_ui.renderTextures);
		}
	}
}

void ResourcesTab() {
	ImGui::Text("%zu resources created by Aether, %s", g_ui.resources.size(), Bytes(ResourceRegistry::Get().TotalBytes()).c_str());
	if (ImGui::BeginTable("resources", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Name");
		ImGui::TableSetupColumn("Category");
		ImGui::TableSetupColumn("Size");
		ImGui::TableSetupColumn("Dimensions");
		ImGui::TableSetupColumn("Heap");
		ImGui::TableSetupColumn("Age (s)");
		ImGui::TableHeadersRow();
		double now = QpcSeconds();
		for (const ResourceRecord& r : g_ui.resources) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(CategoryName(r.category));
			ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(Bytes(r.bytes).c_str());
			ImGui::TableSetColumnIndex(3); ImGui::Text("%ux%u", r.width, r.height);
			ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(r.heap == D3D12_HEAP_TYPE_DEFAULT ? "default" : r.heap == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "readback");
			ImGui::TableSetColumnIndex(5); ImGui::Text("%.0f", now - r.createdTime);
		}
		ImGui::EndTable();
	}
}

void ShadersTab() {
	if (ImGui::Button("Reload shaders")) {
		ShaderManager::Get().RequestReload();
	}
	const json& list = g_ui.shaders;
	ImGui::SameLine();
	ImGui::TextDisabled("%s", list.value("directory", "").c_str());
	if (!list.contains("shaders")) {
		return;
	}
	for (const json& s : list["shaders"]) {
		bool ok = s.value("ok", false);
		std::string output = s.value("compilerOutput", "");
		ImGui::TextColored(ok ? ImVec4(0.5f, 1, 0.5f, 1) : ImVec4(1, 0.4f, 0.4f, 1), "%s", ok ? "OK   " : "ERROR");
		ImGui::SameLine();
		ImGui::Text("%s  (%.1f ms, %zu bytes)", s.value("key", "").c_str(), s.value("compileMs", 0.0), s.value("bytecodeBytes", size_t(0)));
		if (!output.empty()) {
			ImGui::Indent();
			ImGui::PushStyleColor(ImGuiCol_Text, ok ? ImVec4(1, 0.9f, 0.5f, 1) : ImVec4(1, 0.6f, 0.6f, 1));
			ImGui::TextWrapped("%s", output.c_str());
			ImGui::PopStyleColor();
			ImGui::Unindent();
		}
	}
}

void SceneTab() {
	if (ImGui::Button("Refresh scene")) {
		Runtime::Get().RequestManaged(kRequestScene);
	}
	ImGui::SameLine();
	if (ImGui::Button("Collect materials")) {
		Runtime::Get().RequestManaged(kRequestMaterials);
	}
	if (!g_ui.managed.is_null() && ImGui::CollapsingHeader("Game / Unity", ImGuiTreeNodeFlags_DefaultOpen)) {
		JsonTree("managed", g_ui.managed);
	}
	if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
		if (g_ui.scene.is_null()) {
			ImGui::TextDisabled("Not published yet.");
		} else {
			ImGui::TextDisabled("%.0f s old", g_ui.sceneAge);
			JsonTree("scene", g_ui.scene);
		}
	}
	if (ImGui::CollapsingHeader("Aether scene (what the native renderer holds)", ImGuiTreeNodeFlags_DefaultOpen)) {
		JsonTree("aetherScene", g_ui.sceneStore);
	}
	if (ImGui::CollapsingHeader("Materials")) {
		if (g_ui.materials.is_null()) {
			ImGui::TextDisabled("Not collected yet: press Collect materials (walks every renderer once).");
		} else {
			ImGui::TextDisabled("%.0f s old", g_ui.materialsAge);
			JsonTree("materials", g_ui.materials);
		}
	}
}

void LogTab() {
	static const char* levels[] = {"debug", "info", "warning", "error"};
	ImGui::SetNextItemWidth(140);
	ImGui::Combo("Minimum severity", &g_ui.logSeverity, levels, 4);
	ImGui::SameLine();
	ImGui::Checkbox("Auto scroll", &g_ui.logAutoScroll);
	ImGui::SameLine();
	ImGui::TextDisabled("%u errors this session", Log::Get().ErrorCount());
	ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
	for (const LogEntry& e : g_ui.logs) {
		if (int(e.severity) < g_ui.logSeverity) {
			continue;
		}
		ImVec4 color = e.severity >= Severity::Error ? ImVec4(1, 0.45f, 0.45f, 1) : e.severity == Severity::Warning ? ImVec4(1, 0.85f, 0.4f, 1) : ImVec4(0.85f, 0.85f, 0.85f, 1);
		ImGui::PushStyleColor(ImGuiCol_Text, color);
		ImGui::Text("%s [%s] %s%s%s: %s", e.timestamp.c_str() + 11, SeverityName(e.severity), e.component.c_str(), e.code.empty() ? "" : " ", e.code.c_str(), e.message.c_str());
		ImGui::PopStyleColor();
	}
	if (g_ui.logAutoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
		ImGui::SetScrollHereY(1.0f);
	}
	ImGui::EndChild();
}

void ApprovalWindow(UINT width, UINT height) {
	ApprovalPrompt prompt;
	if (!Runtime::Get().CurrentApproval(prompt)) {
		return;
	}
	ImGui::SetNextWindowPos(ImVec2(float(width) * 0.5f, float(height) * 0.3f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(520.0f * ImGui::GetStyle().FontScaleMain, 0));
	ImGui::Begin("Aether Graphics: an AI agent asks for a change", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
	ImGui::TextWrapped("Request: %s", prompt.what.c_str());
	ImGui::TextWrapped("%s", prompt.detail.c_str());
	ImGui::Text("Denied automatically in %.0f s", std::max(0.0, prompt.deadline - QpcSeconds()));
	if (ImGui::Button("Approve", ImVec2(120, 0))) {
		Runtime::Get().ResolveApproval(prompt.id, true);
	}
	ImGui::SameLine();
	if (ImGui::Button("Deny", ImVec2(120, 0))) {
		Runtime::Get().ResolveApproval(prompt.id, false);
	}
	ImGui::End();
}

// ------------------------------------------------------------------------------------------------ settings pages

void QualityPage(const SettingsData& s) {
	CardBegin("Preset", "Sets shadow detail, ray tracing, global illumination, reflections, fog, texture filtering, level of detail and path tracing. Colour, sky and camera effects stay as they are.", false);
	PresetButtons();
	CardEnd(false);

	CardBegin("Image");
	static const char* qualities[] = {"low", "medium", "high", "ultra"};
	static const char* qualityText[] = {"Low", "Medium", "High", "Ultra"};
	ChoiceRow("Shadow detail", "Resolution and distance of the sun's shadow cascades.", "quality", s.quality, qualities, qualityText, 4);
	const bool aa = SwitchRow("Antialiasing", "Temporal antialiasing.", "antialiasing", s.antialiasing);
	{
		// TAA, or NVIDIA DLSS: DLAA at the full size, super resolution from a smaller one (Renderer/Dlss.h). Without
		// DLSS the row is disabled and says why; the setting then resolves with TAA whatever it holds.
		static const char* upscalers[] = {"taa", "dlaa", "dlss_quality", "dlss_balanced", "dlss_performance", "dlss_ultra_performance"};
		static const char* upscalerText[] = {"TAA", "DLAA (native)", "DLSS Quality", "DLSS Balanced", "DLSS Performance", "DLSS Ultra Performance"};
		Dlss& dlss = Dlss::Get();
		const bool known = dlss.InitTried(), available = dlss.Available();
		const char* help = !Dlss::Built() ? "NVIDIA DLSS is not in this build (its SDK was missing): TAA."
			: known && !available ? "NVIDIA DLSS is not available here (it needs an NVIDIA RTX GPU, a recent driver and nvngx_dlss.dll next to the native runtime): TAA."
			: s.upscaler == "taa" ? "DLAA: NVIDIA DLSS at the full size. DLSS Quality to Ultra Performance draw the scene smaller and DLSS brings it up to the screen's size."
			: s.upscaler == "dlaa" ? "NVIDIA DLSS at the full size: sharper and steadier than TAA, at some GPU cost."
			: "The scene is drawn smaller and NVIDIA DLSS brings it up to the screen's size; post-processing runs at the full size.";
		ImGui::BeginDisabled(!aa || !Dlss::Built() || (known && !available));
		ChoiceRow("Anti-aliasing and upscaling", help, "upscaler", s.upscaler, upscalers, upscalerText, 6);
		ImGui::EndDisabled();
	}
	IntSliderRow("Texture filtering", "Anisotropic filtering level: sharper textures at a glancing angle.", "anisotropicLevel", s.anisotropicLevel, 1, 16, "%dx");
	SliderRow("Level of detail", "Higher keeps detailed models up to a greater distance.", "lodBias", s.lodBias, 0.3f, 8.0f, "%.1f");
	Label("Vertical sync", nullptr);
	static const char* vsyncText[] = {"Off", "Every refresh", "Every second refresh"};
	int vsync = std::min(std::max(s.vsync, 0), 2);
	if (ImGui::Combo("##vsync", &vsync, vsyncText, 3)) {
		Settings::Get().Set("vsync", vsync, "overlay");
	}
	CardEnd();
}

void LightingPage(const SettingsData& s) {
	CardBegin("Sun shadows");
	bool shadows = SwitchRow("Sun shadows", nullptr, "shadows", s.shadows);
	ImGui::BeginDisabled(!shadows);
	SliderRow("Softness", "How far the shadow edges are blurred, in shadow map texels (0 is hard).", "shadowSoftness", s.shadowSoftness, 0.0f, 4.0f, "%.1f");
	ImGui::EndDisabled();
	SwitchRow("Lamp shadows", "Street lamps and head lamps cast shadows, traced by rays (up to four lamps per pixel).", "lampShadows", s.lampShadows);
	SwitchRow("Ray traced shadows and occlusion", "The sun's shadow and the ambient occlusion from rays, denoised by NVIDIA NRD. Off: shadow map and screen space occlusion.", "rayTracedLighting", s.rayTracedLighting);
	CardEnd();

	CardBegin("Ambient occlusion");
	bool ao = SwitchRow("Ambient occlusion", nullptr, "ssao", s.ssao);
	ImGui::BeginDisabled(!ao);
	SliderRow("Strength", "1 is as measured; above that, darker.", "aoStrength", s.aoStrength, 0.0f, 2.0f, "%.2f");
	SliderRow("Radius", "How far around a point it looks, in metres.", "aoRadius", s.aoRadius, 0.2f, 4.0f, "%.2f m");
	ImGui::EndDisabled();
	CardEnd();

	CardBegin("Global illumination", "Light bounced off the surroundings.");
	Label("Method", "Radiance cascades and the probe grid need the scene's ray tracing geometry.");
	static const char* giText[] = {"Radiance cascades", "Probe grid", "Off"};
	int gi = s.radianceCascades ? 0 : s.probeGi ? 1 : 2;
	if (ImGui::Combo("##gi", &gi, giText, 3)) {
		Settings::Get().Set("radianceCascades", gi == 0, "overlay");
		Settings::Get().Set("probeGi", gi == 1, "overlay");
	}
	ImGui::BeginDisabled(!s.rayTracedLighting);
	SwitchRow("Per pixel ambient from rays", "Long rays per pixel instead of the cascades or the probes (NVIDIA NRD). Needs ray traced shadows and occlusion.", "rayTracedGi", s.rayTracedGi);
	ImGui::EndDisabled();
	bool bounce = SwitchRow("Nearby bounce light", "Light bounced off nearby surfaces, in screen space.", "indirectLight", s.indirectLight);
	(void)bounce;
	SliderRow("Strength", "Factor on the bounced light.", "indirectStrength", s.indirectStrength, 0.0f, 3.0f, "%.2f");
	CardEnd();
}

void ReflectionsPage(const SettingsData& s) {
	CardBegin("Reflections");
	SwitchRow("Screen space reflections", "As the game's volumes set them.", "ssr", s.ssr);
	SwitchRow("Reflection probe", "The surroundings drawn into a cube around the camera every frame.", "reflectionProbe", s.reflectionProbe);
	bool rt = SwitchRow("Ray traced reflections", "Smooth surfaces mirror what rays through the scene find, also off screen.", "rayTracedReflections", s.rayTracedReflections);
	ImGui::BeginDisabled(!rt);
	static const char* resModes[] = {"full", "half"};
	static const char* resText[] = {"Every pixel", "One per 2 x 2 pixels"};
	ChoiceRow("Ray resolution", "Every pixel keeps fine shapes (mesh wheels, rim lips) steady; one per 2 x 2 costs a quarter.", "reflectionResolution", s.reflectionResolution, resModes, resText, 2);
	Label("Glossy, denoised", "Rays spread over each surface's gloss, denoised by NVIDIA NRD. Off: mirror rays only.");
	if (SwitchWidget("reflectionDenoiser", s.reflectionDenoiser == "nrd")) {
		Settings::Get().Set("reflectionDenoiser", s.reflectionDenoiser == "nrd" ? "off" : "nrd", "overlay");
	}
	SliderRow("Moving cars in rays", "How many cars and people are brought up to their current pose for rays each frame, in turn (on the GPU). 0: they are not in the rays at all (no ray traced reflections or shadows of them).", "skinnedRayUpdates", s.skinnedRayUpdates, 0.0f, 64.0f, "%.0f");
	ImGui::EndDisabled();
	CardEnd();
}

void SkyPage(const SettingsData& s) {
	CardBegin("Sky and sun");
	static const char* skyModes[] = {"physical", "scene"};
	static const char* skyText[] = {"Physical sky (time of day)", "The scene's own sky"};
	ChoiceRow("Sky", nullptr, "skyMode", s.skyMode, skyModes, skyText, 2);
	ImGui::BeginDisabled(s.skyMode == "scene");
	SliderRow("Time of day", nullptr, "timeOfDay", s.timeOfDay, 0.0f, 24.0f, "%.2f h");
	SliderRow("Sun heading", "Turns the sun's path about the up axis.", "sunHeading", s.sunHeading, 0.0f, 360.0f, "%.0f deg");
	SliderRow("Clouds", "How much of the sky the clouds cover.", "cloudCoverage", s.cloudCoverage, 0.0f, 1.0f, "%.2f");
	SliderRow("Cloud speed", nullptr, "cloudSpeed", s.cloudSpeed, 0.0f, 4.0f, "%.1f");
	ImGui::EndDisabled();
	CardEnd();

	CardBegin("Night");
	SwitchRow("Follow the game's night", "The physical sky turns to night when the game's scene does.", "followSceneNight", s.followSceneNight);
	SliderRow("Moonlight", "1: a night you can see the yard in. 0.25: nearly black.", "moonlight", s.moonlight, 0.0f, 4.0f, "%.2f");
	CardEnd();

	CardBegin("Fog");
	SwitchRow("Volumetric fog", "Fog lit by the sun through the shadow map (light shafts), where the scene has fog.", "volumetricFog", s.volumetricFog);
	SliderRow("Density", "Factor on the fog: the physical sky's haze, or else the scene's fog.", "fogStrength", s.fogStrength, 0.0f, 20.0f, "%.1f");
	CardEnd();
}

void CameraPage(const SettingsData& s) {
	CardBegin("Tone and exposure");
	static const char* tonemaps[] = {"host", "agx", "agx_punchy", "aces", "neutral", "none"};
	static const char* tonemapText[] = {"The scene's own curve", "AgX", "AgX Punchy", "ACES", "Neutral", "None"};
	ChoiceRow("Tone mapping", s.tonemap == "agx" ? "AgX is flat on purpose: bright colours run to white and it looks paler than the game's curve. AgX Punchy adds contrast and colour." : nullptr, "tonemap", s.tonemap, tonemaps, tonemapText, 6);
	SwitchRow("The game's colour grading", s.hostGrading ? "The game's tone curve, post exposure and grading, as its volumes set them." : (s.tonemap == "host" ? "Off: no game curve or grading; the scene's curve is drawn with AgX." : "Off: no game curve or grading."), "hostGrading", s.hostGrading);
	static const char* autoModes[] = {"always", "scene"};
	static const char* autoText[] = {"Always (eye adaptation)", "As the scene sets it"};
	ChoiceRow("Automatic exposure", "Always: the brightness adapts as eyes do, fast towards light and slowly towards the dark, also where the scene has a fixed exposure.", "autoExposure", s.autoExposure, autoModes, autoText, 2);
	SliderRow("Exposure", "Bias in stops on top of the scene's exposure.", "exposureEV", s.exposureEV, -3.0f, 3.0f, "%+.2f EV");
	CardEnd();

	CardBegin("Lens");
	bool bloom = SwitchRow("Bloom", nullptr, "bloom", s.bloom);
	ImGui::BeginDisabled(!bloom);
	SliderRow("Bloom strength", "Factor on what the scene asks for.", "bloomStrength", s.bloomStrength, 0.0f, 2.0f, "%.2f");
	ImGui::EndDisabled();
	bool flare = SwitchRow("Lens flare", "Ghosts, a halo and rays from the sun and bright lamps.", "lensFlare", s.lensFlare);
	ImGui::BeginDisabled(!flare);
	SliderRow("Lens flare strength", nullptr, "lensFlareStrength", s.lensFlareStrength, 0.0f, 3.0f, "%.2f");
	ImGui::EndDisabled();
	bool blur = SwitchRow("Motion blur", nullptr, "motionBlur", s.motionBlur);
	ImGui::BeginDisabled(!blur);
	SliderRow("Motion blur strength", "Factor on what the scene asks for.", "motionBlurStrength", s.motionBlurStrength, 0.0f, 2.0f, "%.2f");
	ImGui::EndDisabled();
	CardEnd();
}

void EffectsPage(const SettingsData& s) {
	CardBegin("Particles");
	bool particles = SwitchRow("Particles and tyre smoke", nullptr, "particles", s.particles);
	ImGui::BeginDisabled(!particles);
	SliderRow("Tyre smoke opacity", nullptr, "smokeOpacity", s.smokeOpacity, 0.0f, 2.0f, "%.2f");
	ImGui::EndDisabled();
	CardEnd();
}

void FreeCameraPage(const SettingsData& s) {
	CardBegin("Free camera", Format("%s switches it on and off. The game's UI is hidden while it is on.", KeyName(s.freeCameraHotkey).c_str()).c_str(), false);
	Overlay::Get().FreeCameraStatus();
	CardEnd(false);

	CardBegin("Camera");
	SliderRow("Speed", "Shift: five times faster. Ctrl: five times slower.", "freeCameraSpeed", s.freeCameraSpeed, 0.2f, 100.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
	SliderRow("Mouse sensitivity", "Degrees per mouse count.", "freeCameraSensitivity", s.freeCameraSensitivity, 0.01f, 1.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
	CardEnd();

	CardBegin("Path tracing", "A noise free picture builds up while the camera stands still. It needs DXR 1.1.");
	static const char* ptModes[] = {"off", "freeCamera", "always"};
	static const char* ptText[] = {"Off", "While the free camera is on", "Always"};
	Dlss& dlss = Dlss::Get();
	const bool rr = s.pathTracingDenoiser == "dlss_rr" && dlss.RrAvailable();
	ChoiceRow("Path tracing", s.pathTracing == "always" && !rr ? "Always: the picture is noisy while anything moves." : nullptr, "pathTracing", s.pathTracing, ptModes, ptText, 3);
	ImGui::BeginDisabled(s.pathTracing == "off");
	{
		// NVIDIA DLSS Ray Reconstruction (Renderer/Dlss.h): a fresh sample every frame, denoised. Without it the row is
		// disabled and says why; the setting then averages the samples whatever it holds.
		static const char* denoisers[] = {"off", "dlss_rr"};
		static const char* denoiserText[] = {"None (average samples)", "DLSS Ray Reconstruction"};
		const bool known = dlss.InitTried(), available = dlss.RrAvailable();
		static std::string why;
		why = !Dlss::Built() ? std::string("NVIDIA DLSS is not in this build (its SDK was missing): the samples are averaged.")
			: known && !available ? "DLSS Ray Reconstruction is not available here (" + dlss.RrReason() + "): the samples are averaged."
			: s.pathTracingDenoiser == "dlss_rr" ? std::string("A fresh sample every frame, denoised by NVIDIA DLSS Ray Reconstruction: clean while the camera moves.")
			: std::string("The samples are averaged while nothing changes: noisy while anything moves.");
		ImGui::BeginDisabled(!Dlss::Built() || (known && !available));
		ChoiceRow("Denoiser", why.c_str(), "pathTracingDenoiser", s.pathTracingDenoiser, denoisers, denoiserText, 2);
		ImGui::EndDisabled();
	}
	IntSliderRow("Bounces", nullptr, "pathTracingBounces", s.pathTracingBounces, 1, 12);
	if (rr) {
		IntSliderRow("Samples per frame", "Paths per pixel each frame, averaged before the denoiser: less noise to denoise, slower.", "pathTracingSamplesPerFrame", s.pathTracingSamplesPerFrame, 1, 4);
	} else {
		Label("Samples per pixel", "The picture stays as it is after that many.");
		int samples = s.pathTracingMaxSamples;
		if (ImGui::InputInt("##samples", &samples, 256, 4096)) {
			Settings::Get().Set("pathTracingMaxSamples", std::min(std::max(samples, 1), 100000), "overlay");
		}
	}
	ImGui::EndDisabled();
	CardEnd();
}

void GeneralPage(const SettingsData& s) {
	CardBegin("Overlay");
	Label("UI scale", "1 is sized for a 1440p screen; larger screens scale up by their height. Applied when you let go of the slider.");
	static float scale = 1.0f;
	static bool dragging = false;
	if (!dragging) {
		scale = s.overlayScale;
	}
	ImGui::SliderFloat("##scale", &scale, 0.5f, 3.0f, "%.2f");
	dragging = ImGui::IsItemActive();
	if (ImGui::IsItemDeactivatedAfterEdit()) {
		Settings::Get().Set("overlayScale", scale, "overlay");
	}
	Label("Text size", "The text only; the layout stays the same size.");
	static const float textScales[] = {1.0f, 1.15f, 1.3f};
	static const char* textText[] = {"Normal", "Large", "Larger"};
	int text = s.overlayTextScale >= 1.29f ? 2 : s.overlayTextScale >= 1.14f ? 1 : 0;
	if (ImGui::Combo("##text", &text, textText, 3)) {
		Settings::Get().Set("overlayTextScale", textScales[text], "overlay");
	}
	SwitchRow("Open at start", "Open these settings when the game starts.", "overlayVisibleAtStart", s.overlayVisibleAtStart);
	SwitchRow("Frame rate readout", "A small fps and frame time readout in the corner while the windows are closed.", "overlayHud", s.overlayHud);
	CardEnd();

	CardBegin("Keys", "Change them with overlayHotkey and freeCameraHotkey in aether.json.");
	const std::string key = KeyName(s.overlayHotkey);
	ValueRow("Settings", nullptr, key);
	ValueRow("Debug tools", nullptr, "Shift+" + key);
	ValueRow("Free camera", nullptr, KeyName(s.freeCameraHotkey));
	CardEnd();

	CardBegin("AI agent (MCP)");
	static const char* modes[] = {"deny", "ask", "allow"};
	static const char* modeText[] = {"Deny: read only", "Ask every time", "Allow without asking"};
	ChoiceRow("Change requests", "What an AI agent's request to change a setting needs.", "mcpControl", s.mcpControl, modes, modeText, 3);
	CardEnd();
}

// ------------------------------------------------------------------------------------------------ settings window

struct PageEntry {
	const char* title;
	const char* summary;
	void (*draw)(const SettingsData&);
};

const PageEntry kPages[] = {
	{"Quality", "Presets and how sharp the picture is.", QualityPage},
	{"Lighting", "Shadows, ambient occlusion and bounced light.", LightingPage},
	{"Reflections", "Screen space, probe and ray traced reflections.", ReflectionsPage},
	{"Sky and weather", "Time of day, clouds, night and fog.", SkyPage},
	{"Camera and colour", "Tone mapping, exposure and lens effects.", CameraPage},
	{"Effects", "Particles and tyre smoke.", EffectsPage},
	{"Free camera", "The F11 camera and path tracing.", FreeCameraPage},
	{"General", "The overlay, its keys and the AI agent.", GeneralPage},
};
int g_page = 0;

// Keeps the current window inside the screen (a window placed on a larger screen, or a game window made smaller,
// left it partly outside, its title bar out of reach).
void KeepOnScreen() {
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	ImVec2 size = ImGui::GetWindowSize();
	if (size.x > display.x || size.y > display.y) {
		size = ImVec2(std::min(size.x, display.x), std::min(size.y, display.y));
		ImGui::SetWindowSize(size);
	}
	const ImVec2 pos = ImGui::GetWindowPos();
	const ImVec2 kept(std::clamp(pos.x, 0.0f, std::max(0.0f, display.x - size.x)), std::clamp(pos.y, 0.0f, std::max(0.0f, display.y - size.y)));
	if (kept.x != pos.x || kept.y != pos.y) {
		ImGui::SetWindowPos(kept);
	}
}

// a row of the left rail: the chosen one filled, with the accent bar at its left
bool NavItem(const char* label, bool chosen) {
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() + 6 * Sc();
	const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h));
	const bool hovered = ImGui::IsItemHovered();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (chosen) {
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U(kChosen), 4 * Sc());
		dl->AddRectFilled(p, ImVec2(p.x + 3 * Sc(), p.y + h), U(kAccent), 2 * Sc());
	} else if (hovered) {
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U(kHover), 4 * Sc());
	}
	const float th = ImGui::GetFontSize();
	dl->AddText(ImVec2(p.x + 12 * Sc(), p.y + (h - th) * 0.5f), chosen || hovered ? U(kText) : U(kSecondary), label);
	return pressed;
}

void SettingsWindow(bool& open) {
	ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(920 * Sc(), 640 * Sc()), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSizeConstraints(ImVec2(640 * Sc(), 360 * Sc()), ImVec2(FLT_MAX, FLT_MAX));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	const bool shown = ImGui::Begin("Aether Graphics###aether_settings_v2", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar();
	if (!shown) {
		ImGui::End();
		return;
	}
	KeepOnScreen();
	const SettingsData s = Settings::Get().Snapshot();

	// the rail: pages, then the debug tools and the version at the bottom
	const float railW = 200 * Sc();
	ImGui::PushStyleColor(ImGuiCol_ChildBg, Rgb(kRail));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * Sc(), 10 * Sc()));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * Sc()));
	ImGui::BeginChild("rail", ImVec2(railW, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
	for (int i = 0; i < int(std::size(kPages)); ++i) {
		if (NavItem(kPages[i].title, g_page == i)) {
			g_page = i;
		}
		if (i == int(std::size(kPages)) - 2) {
			ImGui::Dummy(ImVec2(0, 4 * Sc()));
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 6 * Sc(), p.y), ImVec2(p.x + railW - 22 * Sc(), p.y), U(kLine));
			ImGui::Dummy(ImVec2(0, 4 * Sc()));
		}
	}
	const float footer = ImGui::GetFrameHeight() * 2 + ImGui::GetTextLineHeight() + 20 * Sc();
	ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + 8 * Sc(), ImGui::GetWindowHeight() - footer));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 6 * Sc()));
	if (ImGui::Button(Overlay::Get().DebugVisible() ? "Close debug tools" : "Open debug tools", ImVec2(-FLT_MIN, 0))) {
		Overlay::Get().ToggleDebug();
	}
	ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kSecondary));
	ImGui::Text("  Version %s", kAetherVersion);
	ImGui::PopStyleColor();
	ImGui::PopStyleVar();
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	{
		// the line between the rail and the page
		const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
		ImGui::GetWindowDrawList()->AddLine(ImVec2(max.x, min.y), ImVec2(max.x, max.y), U(kLine));
	}

	ImGui::SameLine(0, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * Sc(), 18 * Sc()));
	ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	const PageEntry& page = kPages[g_page];
	Heading(page.title, 1.35f);
	Secondary(page.summary);
	ImGui::Dummy(ImVec2(0, 6 * Sc()));
	Banners();
	page.draw(s);
	ImGui::EndChild();
	ImGui::End();
}

// ------------------------------------------------------------------------------------------------ debug window

void RenderTab() {
	json state = Renderer::Get().State();
	Settings& settings = Settings::Get();
	SettingsData s = settings.Snapshot();
	if (state.contains("lastError") && state["lastError"].is_string()) {
		Banner(kError, state["lastError"].get<std::string>().c_str());
	}
	if (ImGui::BeginTable("renderer", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
		KeyValue("Renderer", state.value("state", ""));
		KeyValue("Main renderer", state.value("mainRenderer", ""));
		if (state.contains("backBuffer") && state["backBuffer"].is_object()) {
			const json& b = state["backBuffer"];
			KeyValue("Back buffer", Format("%ux%u, DXGI format %d, %u buffers", b.value("width", 0u), b.value("height", 0u), b.value("dxgiFormat", 0), b.value("bufferCount", 0u)));
		}
		ImGui::EndTable();
	}

	ImGui::SeparatorText("View");
	ImGui::SetNextItemWidth(220 * Sc());
	if (ImGui::BeginCombo("Render mode", s.renderMode.c_str())) {
		for (const std::string& m : RenderModeNames()) {
			if (ImGui::Selectable(m.c_str(), m == s.renderMode)) {
				settings.Set("renderMode", m, "overlay");
			}
		}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	ImGui::TextDisabled("(?)");
	ImGui::SetItemTooltip("aether: Aether's image. hdrp: the game's renderer. compare: left HDRP, right Aether.");
	ImGui::SetNextItemWidth(220 * Sc());
	if (ImGui::BeginCombo("Debug view", s.debugView.c_str())) {
		for (const std::string& v : DebugViewNames()) {
			if (ImGui::Selectable(v.c_str(), v == s.debugView)) {
				settings.Set("debugView", v, "overlay");
			}
		}
		ImGui::EndCombo();
	}
	bool b = s.sky;
	if (ImGui::Checkbox("Sky", &b)) {
		settings.Set("sky", b, "overlay");
	}
	ImGui::SameLine();
	b = s.gpuTiming;
	if (ImGui::Checkbox("GPU timing", &b)) {
		settings.Set("gpuTiming", b, "overlay");
	}
	ImGui::SameLine();
	b = s.testTriangle;
	if (ImGui::Checkbox("Test triangle", &b)) {
		settings.Set("testTriangle", b, "overlay");
	}
	if (ImGui::Button("Capture frame (PNG)")) {
		g_ui.lastCapture = Renderer::Get().RequestCapture(NewCapturePath(), true);
	}
	if (g_ui.lastCapture) {
		json st = Renderer::Get().CaptureStatus(g_ui.lastCapture);
		ImGui::SameLine();
		ImGui::TextWrapped("%s: %s %s", st.value("state", "").c_str(), st.value("file", "").c_str(), st.value("error", "").c_str());
	}

	ImGui::SeparatorText("Render passes (last frame)");
	std::vector<PassTiming> passes = Profiler::Get().PassSnapshot();
	if (ImGui::BeginTable("passes", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("Pass");
		ImGui::TableSetupColumn("Ran");
		ImGui::TableSetupColumn("CPU ms");
		ImGui::TableSetupColumn("GPU ms");
		ImGui::TableSetupColumn("Draw calls");
		ImGui::TableSetupColumn("Note");
		ImGui::TableHeadersRow();
		for (const PassTiming& p : passes) {
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(p.name.c_str());
			ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(p.executed ? "yes" : "no");
			ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", p.cpuMs);
			ImGui::TableSetColumnIndex(3);
			if (p.gpuMs >= 0) {
				ImGui::Text("%.3f", p.gpuMs);
			} else {
				ImGui::TextDisabled("-");
			}
			ImGui::TableSetColumnIndex(4); ImGui::Text("%u", p.drawCalls);
			ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(p.note.c_str());
		}
		ImGui::EndTable();
	}

	ImGui::SeparatorText("Aether scene renderer");
	const json& r = g_ui.sceneRenderer;
	if (r.is_object() && r.contains("drawCalls")) {
		if (ImGui::BeginTable("scenestats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			KeyValue("Visible instances", Format("%u", r.value("visibleInstances", 0u)));
			KeyValue("Draw calls (opaque / transparent / shadow)", Format("%u (%u / %u / %u)", r.value("drawCalls", 0u), r.value("opaqueDraws", 0u), r.value("transparentDraws", 0u), r.value("shadowCasterDraws", 0u)));
			KeyValue("Triangles", Format("%u", r.value("triangles", 0u)));
			KeyValue("Materials in frame", Format("%u (%u draws with an unsupported material)", r.value("materialsInFrame", 0u), r.value("unsupportedMaterialDraws", 0u)));
			KeyValue("Pipeline states", Format("%u", r.value("pipelineStates", 0u)));
			if (r.contains("lights")) {
				const json& l = r["lights"];
				KeyValue("Lights", Format("%d directional, %u punctual; %u of %u clusters lit, at most %u lights in one", l.value("directional", 0), l.value("punctual", 0u), l.value("clustersWithLights", 0u), l.value("clusters", 0u), l.value("maxLightsInACluster", 0u)));
			}
			if (r.contains("shadows")) {
				const json& sh = r["shadows"];
				KeyValue("Shadow cascades", Format("%u x %u px (%s)", sh.value("cascades", 0u), sh.value("cascadeSize", 0u), Bytes(sh.value("atlasBytes", uint64_t(0))).c_str()));
			}
			ImGui::EndTable();
		}
	} else {
		ImGui::TextDisabled("Not rendering (render mode hdrp, or no scene camera yet).");
	}
	if (ImGui::CollapsingHeader("Render graph (scene)")) {
		JsonTree("sceneGraph", g_ui.sceneGraph);
	}
}

void DebugWindow(bool& open, UINT width) {
	ImGui::SetNextWindowPos(ImVec2(std::max(40.0f, float(width) - 820 * Sc()), 40), ImGuiCond_FirstUseEver);
	ImGui::SetNextWindowSize(ImVec2(780 * Sc(), 640 * Sc()), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Aether Graphics debug tools###aether_debug_v2", &open, ImGuiWindowFlags_NoCollapse)) {
		ImGui::End();
		return;
	}
	KeepOnScreen();
	Banners();
	if (ImGui::BeginTabBar("tabs")) {
		if (ImGui::BeginTabItem("Performance")) { PerformanceTab(); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Render")) { RenderTab(); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("GPU")) { GpuTab(); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("VRAM")) { VramTab(); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Resources")) { ResourcesTab(); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Shaders")) { ShadersTab(); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Scene")) { SceneTab(); ImGui::EndTabItem(); }
		if (ImGui::BeginTabItem("Log")) { LogTab(); ImGui::EndTabItem(); }
		ImGui::EndTabBar();
	}
	ImGui::End();
}

// the frame rate readout in the corner (no input: the mouse goes through it)
void Hud() {
	const auto& iv = g_ui.intervals;
	const auto& gpu = g_ui.gpuMs;
	if (iv.empty()) {
		return;
	}
	double sum = 0, gpuSum = 0;
	size_t n = 0, gn = 0;
	for (size_t i = iv.size() > 60 ? iv.size() - 60 : 0; i < iv.size(); ++i, ++n) {
		sum += iv[i];
	}
	for (size_t i = gpu.size() > 60 ? gpu.size() - 60 : 0; i < gpu.size(); ++i) {
		if (gpu[i] > 0) {
			gpuSum += gpu[i];
			++gn;
		}
	}
	const double ms = n ? sum / double(n) : 0;
	ImGui::SetNextWindowPos(ImVec2(12 * Sc(), 12 * Sc()));
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * Sc(), 6 * Sc()));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * Sc());
	ImGui::Begin("###aether_hud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing);
	ImGui::Text("%.0f fps", ms > 0 ? 1000.0 / ms : 0.0);
	ImGui::SameLine();
	ImGui::PushStyleColor(ImGuiCol_Text, Rgb(kSecondary));
	if (gn) {
		ImGui::Text("%.1f ms  |  GPU %.1f ms", ms, gpuSum / double(gn));
	} else {
		ImGui::Text("%.1f ms", ms);
	}
	ImGui::PopStyleColor();
	ImGui::End();
	ImGui::PopStyleVar(2);
}

} // namespace

// ------------------------------------------------------------------------------------------------ fonts and style

void SetOverlayLayoutScale(float scale) {
	g_layout = scale;
}

void LoadOverlayFonts() {
	ImGuiIO& io = ImGui::GetIO();
	wchar_t windows[MAX_PATH] = {};
	GetWindowsDirectoryW(windows, MAX_PATH);
	auto load = [&](const wchar_t* file) -> ImFont* {
		const std::wstring path = std::wstring(windows) + L"\\Fonts\\" + file;
		if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
			return nullptr;
		}
		return io.Fonts->AddFontFromFileTTF(WideToUtf8(path).c_str(), 16.0f);
	};
	ImFont* regular = load(L"segoeui.ttf");
	if (!regular) {
		regular = io.Fonts->AddFontDefault();
	}
	g_bold = load(L"seguisb.ttf");
	if (!g_bold) {
		g_bold = load(L"segoeuib.ttf");
	}
	g_mono = load(L"consola.ttf");
	io.FontDefault = regular;
	AE_LOG(Info, "Overlay", "Fonts: %s%s", g_bold ? "Segoe UI with Semibold" : "the regular font only", g_mono ? ", Consolas" : "");
}

void ApplyOverlayStyle(ImGuiStyle& st) {
	ImGui::StyleColorsDark(&st);
	st.FontSizeBase = 16.0f;
	st._NextFrameFontSizeBase = 16.0f;
	st.WindowPadding = ImVec2(12, 12);
	st.FramePadding = ImVec2(8, 5);
	st.ItemSpacing = ImVec2(8, 8);
	st.ItemInnerSpacing = ImVec2(6, 4);
	st.CellPadding = ImVec2(6, 5);
	st.ScrollbarSize = 10;
	st.GrabMinSize = 8;
	st.WindowRounding = 8;
	st.ChildRounding = 6;
	st.FrameRounding = 4;
	st.PopupRounding = 6;
	st.ScrollbarRounding = 4;
	st.GrabRounding = 4;
	st.TabRounding = 4;
	st.WindowBorderSize = 1;
	st.ChildBorderSize = 1;
	st.PopupBorderSize = 1;
	st.FrameBorderSize = 0;
	st.TabBorderSize = 0;
	st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
	st.WindowMenuButtonPosition = ImGuiDir_None;
	st.SeparatorTextBorderSize = 1;
	st.DisabledAlpha = 0.45f;
	ImVec4* c = st.Colors;
	c[ImGuiCol_Text] = Rgb(kText);
	c[ImGuiCol_TextDisabled] = Rgb(kSecondary);
	c[ImGuiCol_WindowBg] = Rgb(kBackground);
	c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_PopupBg] = Rgb(kPanel);
	c[ImGuiCol_Border] = Rgb(kLine);
	c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_FrameBg] = Rgb(kField);
	c[ImGuiCol_FrameBgHovered] = Rgb(kFieldHover);
	c[ImGuiCol_FrameBgActive] = Rgb(0x3A3D43);
	c[ImGuiCol_TitleBg] = Rgb(kRail);
	c[ImGuiCol_TitleBgActive] = Rgb(kRail);
	c[ImGuiCol_TitleBgCollapsed] = Rgb(kRail);
	c[ImGuiCol_MenuBarBg] = Rgb(kRail);
	c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab] = Rgb(0x3A3D43);
	c[ImGuiCol_ScrollbarGrabHovered] = Rgb(0x4A4D55);
	c[ImGuiCol_ScrollbarGrabActive] = Rgb(0x5A5E66);
	c[ImGuiCol_CheckMark] = Rgb(0xFFFFFF);
	c[ImGuiCol_CheckboxSelectedBg] = Rgb(kAccent);
	c[ImGuiCol_SliderGrab] = Rgb(kAccent);
	c[ImGuiCol_SliderGrabActive] = Rgb(kAccentHover);
	c[ImGuiCol_Button] = Rgb(kField);
	c[ImGuiCol_ButtonHovered] = Rgb(kFieldHover);
	c[ImGuiCol_ButtonActive] = Rgb(kChosen);
	c[ImGuiCol_Header] = Rgb(kChosen);
	c[ImGuiCol_HeaderHovered] = Rgb(kHover);
	c[ImGuiCol_HeaderActive] = Rgb(kChosen);
	c[ImGuiCol_Separator] = Rgb(kLine);
	c[ImGuiCol_SeparatorHovered] = Rgb(kAccent, 0.6f);
	c[ImGuiCol_SeparatorActive] = Rgb(kAccent);
	c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ResizeGripHovered] = Rgb(kAccent, 0.5f);
	c[ImGuiCol_ResizeGripActive] = Rgb(kAccent);
	c[ImGuiCol_Tab] = Rgb(kRail);
	c[ImGuiCol_TabHovered] = Rgb(kHover);
	c[ImGuiCol_TabSelected] = Rgb(kChosen);
	c[ImGuiCol_TabSelectedOverline] = Rgb(kAccent);
	c[ImGuiCol_TabDimmed] = Rgb(kRail);
	c[ImGuiCol_TabDimmedSelected] = Rgb(kPanel);
	c[ImGuiCol_TabDimmedSelectedOverline] = Rgb(kLine);
	c[ImGuiCol_PlotLines] = Rgb(0x6E9BF0);
	c[ImGuiCol_PlotLinesHovered] = Rgb(0xFFFFFF);
	c[ImGuiCol_PlotHistogram] = Rgb(kAccent);
	c[ImGuiCol_PlotHistogramHovered] = Rgb(kAccentHover);
	c[ImGuiCol_TableHeaderBg] = Rgb(kPanel);
	c[ImGuiCol_TableBorderStrong] = Rgb(kLine);
	c[ImGuiCol_TableBorderLight] = Rgb(0x2C2E33);
	c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
	c[ImGuiCol_TextLink] = Rgb(0x6E9BF0);
	c[ImGuiCol_TextSelectedBg] = Rgb(kChosen);
	c[ImGuiCol_NavCursor] = Rgb(kAccent);
	c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.5f);
}

// The free camera's state in the overlay (nothing is drawn over the picture while it is on: the UI is hidden then).
void Overlay::FreeCameraStatus() {
	const SettingsData s = Settings::Get().Snapshot();
	if (freeCamera_.load()) {
		ImGui::Text("Free camera on (field of view %.0f deg), held while the overlay is open", freeCameraFov_.load());
	} else {
		ImGui::TextDisabled("Free camera off");
	}
	ImGui::TextDisabled("WASD / arrows move, E / Space up, Q / C down, mouse looks, wheel zooms, R resets the zoom, Shift faster, Ctrl slower.");
	const SceneRenderer::PathTraceStatus pt = SceneRenderer::Get().PathTracing();
	if (pt.active && pt.denoised) {
		ImGui::Text("Path tracing: DLSS Ray Reconstruction, %d sample%s per pixel each frame (%u frames since the last reset)", s.pathTracingSamplesPerFrame, s.pathTracingSamplesPerFrame == 1 ? "" : "s", pt.samples);
	} else if (pt.active) {
		ImGui::Text("Path tracing: %u / %d samples per pixel%s", pt.samples, s.pathTracingMaxSamples, pt.samples >= uint32_t(s.pathTracingMaxSamples) ? " (done)" : "");
	} else if (s.pathTracing != "off" && (freeCamera_.load() || s.pathTracing == "always")) {
		ImGui::TextDisabled("Path tracing not running (it needs DXR 1.1 and the scene's ray tracing geometry)");
	}
}

void Overlay::BuildUi(UINT width, UINT height) {
	ApprovalWindow(width, height);
	Profiler::Get().History(g_ui.intervals, g_ui.gpuMs, 300);
	if (!Visible()) {
		if (hud_.load() && !freeCamera_.load()) {
			Hud();
		}
		return;
	}
	double now = QpcSeconds();
	Runtime& rt = Runtime::Get();
	if (now >= g_ui.nextSlow) {
		g_ui.nextSlow = now + 0.5;
		g_ui.gpu = GpuInfo::Get().ToJson();
		g_ui.vm = GpuInfo::Get().QueryVideoMemory();
		g_ui.resources = ResourceRegistry::Get().Snapshot();
		g_ui.shaders = ShaderManager::Get().List();
		g_ui.logs = Log::Get().Snapshot(400);
		g_ui.scene = rt.ManagedState("scene", &g_ui.sceneAge);
		g_ui.materials = rt.ManagedState("materials", &g_ui.materialsAge);
		g_ui.renderTextures = rt.ManagedState("renderTextures", &g_ui.rtAge);
		g_ui.managed = rt.ManagedState("managed");
		g_ui.sceneRenderer = SceneRenderer::Get().Stats();
		g_ui.sceneGraph = SceneRenderer::Get().GraphDescription();
		g_ui.sceneStore = SceneStore::Get().Summary();
		g_ui.gpuManager = GpuResources::Get().Report();
	}
	if (visible_.load()) {
		bool open = true;
		SettingsWindow(open);
		if (!open) {
			SetVisible(false);
		}
	}
	if (debug_.load()) {
		bool open = true;
		DebugWindow(open, width);
		if (!open) {
			SetDebugVisible(false);
		}
	}
}

} // namespace aether
