// Aether's own loader: a version.dll in the game folder (UnityPlayer.dll imports VERSION.dll, so Windows loads it from
// there). It starts Aether without a mod loader, beside KSL:
//   - it hooks nothing. KSL (winhttp.dll) and Unity Doorstop both take Mono's start by replacing GetProcAddress in
//     UnityPlayer.dll's import table, and only one of them can; this loader leaves that to KSL.
//   - a thread of its own waits for Mono's root domain, attaches to it and calls AetherGraphics.AetherBootstrap.Start(gameRoot)
//     in <game>\Aether\AetherGraphics.dll. The managed side waits for the game's scripts and moves to Unity's main thread.
//   - version.dll's own functions are forwarded to the system's version.dll.
// Its log: <game>\Aether\logs\loader.log. Nothing happens when <game>\Aether\AetherGraphics.dll is missing.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

// ---- version.dll's functions, forwarded (every argument is an integer or a pointer: eight of them cover all)
HMODULE g_real = nullptr;
INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
	wchar_t path[MAX_PATH];
	UINT n = GetSystemDirectoryW(path, MAX_PATH);
	if (n > 0 && n < MAX_PATH - 16) {
		wcscat_s(path, L"\\version.dll");
		g_real = LoadLibraryW(path);
	}
	return TRUE;
}

FARPROC Real(const char* name) {
	InitOnceExecuteOnce(&g_realOnce, LoadReal, nullptr, nullptr);
	return g_real ? GetProcAddress(g_real, name) : nullptr;
}

using Forwarded = uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

} // namespace

#define AETHER_FORWARD(name)                                                                                                       \
	extern "C" uintptr_t Fwd_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f, uintptr_t g, uintptr_t h) { \
		static Forwarded fn = reinterpret_cast<Forwarded>(Real(#name));                                                             \
		if (!fn) {                                                                                                                  \
			SetLastError(ERROR_PROC_NOT_FOUND);                                                                                     \
			return 0;                                                                                                               \
		}                                                                                                                           \
		return fn(a, b, c, d, e, f, g, h);                                                                                          \
	}
AETHER_FORWARD(GetFileVersionInfoA)
AETHER_FORWARD(GetFileVersionInfoByHandle)
AETHER_FORWARD(GetFileVersionInfoExA)
AETHER_FORWARD(GetFileVersionInfoExW)
AETHER_FORWARD(GetFileVersionInfoSizeA)
AETHER_FORWARD(GetFileVersionInfoSizeExA)
AETHER_FORWARD(GetFileVersionInfoSizeExW)
AETHER_FORWARD(GetFileVersionInfoSizeW)
AETHER_FORWARD(GetFileVersionInfoW)
AETHER_FORWARD(VerFindFileA)
AETHER_FORWARD(VerFindFileW)
AETHER_FORWARD(VerInstallFileA)
AETHER_FORWARD(VerInstallFileW)
AETHER_FORWARD(VerLanguageNameA)
AETHER_FORWARD(VerLanguageNameW)
AETHER_FORWARD(VerQueryValueA)
AETHER_FORWARD(VerQueryValueW)

namespace {

// ---- the start of Aether
std::wstring g_root;   // the game folder (the executable's)
std::wstring g_aether; // <game>\Aether
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
	if (!g_log) {
		return;
	}
	SYSTEMTIME t;
	GetLocalTime(&t);
	fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
	va_list args;
	va_start(args, fmt);
	vfprintf(g_log, fmt, args);
	va_end(args);
	fputc('\n', g_log);
	fflush(g_log);
}

std::string Utf8(const std::wstring& s) {
	int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
	std::string out(n > 0 ? n - 1 : 0, '\0');
	if (n > 1) {
		WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), n, nullptr, nullptr);
	}
	return out;
}

// Mono's embedding API (mono-2.0-bdwgc.dll, Unity's Mono)
struct Mono {
	void* (*get_root_domain)();
	void* (*thread_attach)(void*);
	void (*thread_detach)(void*);
	void* (*domain_assembly_open)(void*, const char*);
	void* (*assembly_get_image)(void*);
	void* (*class_from_name)(void*, const char*, const char*);
	void* (*class_get_method_from_name)(void*, const char*, int);
	void* (*runtime_invoke)(void*, void*, void**, void**);
	void* (*string_new)(void*, const char*);
	void* (*object_to_string)(void*, void**);
	char* (*string_to_utf8)(void*);
	void (*free)(void*);
};

template <class F> bool Resolve(HMODULE m, F& f, const char* name) {
	f = reinterpret_cast<F>(GetProcAddress(m, name));
	if (!f) {
		Log("Mono has no %s", name);
	}
	return f != nullptr;
}

DWORD WINAPI Boot(void*) {
	// Mono: loaded by UnityPlayer.dll a little after this DLL
	HMODULE module = nullptr;
	for (int i = 0; i < 1200 && !module; ++i) { // 2 minutes
		module = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
		if (!module) {
			Sleep(100);
		}
	}
	if (!module) {
		Log("Mono (mono-2.0-bdwgc.dll) did not load in 2 minutes: Aether is not started");
		return 0;
	}
	Mono m{};
	bool ok = Resolve(module, m.get_root_domain, "mono_get_root_domain") && Resolve(module, m.thread_attach, "mono_thread_attach") &&
		Resolve(module, m.thread_detach, "mono_thread_detach") && Resolve(module, m.domain_assembly_open, "mono_domain_assembly_open") &&
		Resolve(module, m.assembly_get_image, "mono_assembly_get_image") && Resolve(module, m.class_from_name, "mono_class_from_name") &&
		Resolve(module, m.class_get_method_from_name, "mono_class_get_method_from_name") && Resolve(module, m.runtime_invoke, "mono_runtime_invoke") &&
		Resolve(module, m.string_new, "mono_string_new") && Resolve(module, m.object_to_string, "mono_object_to_string") &&
		Resolve(module, m.string_to_utf8, "mono_string_to_utf8") && Resolve(module, m.free, "mono_free");
	if (!ok) {
		return 0;
	}
	void* domain = nullptr;
	for (int i = 0; i < 1200 && !domain; ++i) {
		domain = m.get_root_domain();
		if (!domain) {
			Sleep(100);
		}
	}
	if (!domain) {
		Log("Mono's root domain did not appear in 2 minutes: Aether is not started");
		return 0;
	}
	Log("Mono's root domain is up");
	void* thread = m.thread_attach(domain);
	std::string path = Utf8(g_aether + L"\\AetherGraphics.dll");
	void* assembly = m.domain_assembly_open(domain, path.c_str());
	void* image = assembly ? m.assembly_get_image(assembly) : nullptr;
	void* klass = image ? m.class_from_name(image, "AetherGraphics", "AetherBootstrap") : nullptr;
	void* method = klass ? m.class_get_method_from_name(klass, "Start", 1) : nullptr;
	if (!method) {
		Log("%s: %s", path.c_str(), !assembly ? "could not be loaded" : "has no AetherGraphics.AetherBootstrap.Start(string) (built without -p:Loader=Standalone?)");
	} else {
		void* args[1] = {m.string_new(domain, Utf8(g_root).c_str())};
		void* exception = nullptr;
		Log("calling AetherGraphics.AetherBootstrap.Start");
		m.runtime_invoke(method, nullptr, args, &exception);
		if (exception) {
			void* inner = nullptr;
			void* text = m.object_to_string(exception, &inner);
			char* utf8 = text && !inner ? m.string_to_utf8(text) : nullptr;
			Log("AetherGraphics.AetherBootstrap.Start threw: %s", utf8 ? utf8 : "(no message)");
			if (utf8) {
				m.free(utf8);
			}
		} else {
			Log("AetherGraphics.AetherBootstrap.Start returned");
		}
	}
	m.thread_detach(thread);
	return 0;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
	if (reason != DLL_PROCESS_ATTACH) {
		return TRUE;
	}
	DisableThreadLibraryCalls(instance);
	wchar_t exe[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) {
		return TRUE;
	}
	g_root.assign(exe, n);
	g_root.resize(g_root.find_last_of(L"\\/"));
	g_aether = g_root + L"\\AetherGraphics";
	if (GetFileAttributesW((g_aether + L"\\AetherGraphics.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
		return TRUE; // not installed: only version.dll's own functions
	}
	CreateDirectoryW((g_aether + L"\\logs").c_str(), nullptr);
	g_log = _wfsopen((g_aether + L"\\logs\\loader.log").c_str(), L"w", _SH_DENYWR);
	Log("Aether loader in %s", Utf8(exe).c_str());
	// (CreateThread is allowed under the loader lock; the thread itself starts once DllMain returns)
	HANDLE thread = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	} else {
		Log("CreateThread failed (%lu)", GetLastError());
	}
	return TRUE;
}
