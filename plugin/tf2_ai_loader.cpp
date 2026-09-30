// tf2_ai_loader.cpp - "TF2 AI Enhancements" Northstar plugin (Northstar plugin API v4: PluginId001 + PluginCallbacks001).
//
// Job: make the Thunderstore package self-contained. When Northstar loads this plugin (InitialiseNorthstar, long before
// launcher.dll creates the D3D11 device) it loads ReShade (6.8 add-on build) from the package's plugins\lib folder:
//   PROXY mode (default): plugins\lib\proxy\dxgi.dll, loaded before anything else loads dxgi.dll, so every later "dxgi.dll"
//                         binds to it - the same route as a ReShade dxgi.dll in bin\x64_retail
//   HOOK mode (fallback): plugins\lib\ReShade64.dll (same binary) hooking the System32 dxgi / d3d11 exports
// with
//   RESHADE_DISABLE_INPUT_HOOK=1        (ReShade's user32 input hook breaks TF2's raw-input mouse look)
//   RESHADE_BASE_PATH_OVERRIDE=<lib>    (ReShade.ini, ReShade.log and the *.addon64 add-ons all live in plugins\lib)
// ReShade then hooks the system dxgi.dll / d3d11.dll and loads every *.addon64 in plugins\lib (tf2-dlss-sr, tf2-nr-bridge,
// tf2-fg, and a user-supplied renodx-dlss5 if present).
//
// It does NOTHING (logs why) when:
//   - a ReShade is already loaded in the process (a module exporting ReShadeRegisterAddon / ReShadeVersion),
//   - a ReShade proxy DLL (dxgi.dll, d3d11.dll, ...) sits in the game folder or bin\x64_retail (the game would load that
//     one later, and a second ReShade refuses to load -> the game's own dxgi.dll load fails): the "manual install" setup,
//   - the process is a dedicated server (-dedicated), or the command line has -tf2ai_off,
//   - neither plugins\lib\proxy\dxgi.dll nor plugins\lib\ReShade64.dll is present.
// HOOK mode instead of PROXY mode when dxgi.dll is already loaded, when another (non-ReShade) dxgi.dll wrapper sits in the
// game folder or bin\x64_retail (so the game still loads it), when proxy\dxgi.dll is missing, or with -tf2ai_hookmode.
// Command line: -tf2ai_off (load nothing), -tf2ai_hookmode (force HOOK mode, diagnostics). Both are read from the process
// command line and from Northstar's GetCommandLineA (which also carries ns_startup_args.txt).
// DLL search: every DLL this plugin loads is loaded by full path (LoadLibraryExW + LOAD_WITH_ALTERED_SEARCH_PATH), except
// the system version.dll, which is loaded from System32 only (LOAD_LIBRARY_SEARCH_SYSTEM32). The import table is KERNEL32.dll
// only: a static VERSION.dll import would be resolved through Northstar's LoadLibraryExA(SEARCH_USER_DIRS |
// SEARCH_DEFAULT_DIRS), i.e. the game folder and every package's plugins\lib before System32 (1.0.0 review).
// Northstar loads plugins\*.dll only (non-recursive) and adds plugins\lib to the DLL search path,
// which is why the proxy copy lives one level deeper (plugins\lib\proxy\dxgi.dll) and is never offered to a by-name load.
//
// Safety (Northstar's vectored exception handler kills the game on ANY access violation, even inside __try): no pointer is
// dereferenced without a check, all file reads are bounded, every callback body is wrapped in try/catch(...), no convars
// are touched, no hooks are installed, nothing is ever freed (ReShade stays loaded for the life of the process).
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <vector>

#define TF2AI_VERSION "1.0.0"
static const char kVersionTag[] = "TF2AIEnhancements-version=" TF2AI_VERSION;   // read by build_package.py

// ---------------------------------------------------------------------------------------------- Northstar plugin ABI
// Mirrors NorthstarLauncher primedev/plugins/interfaces/IPluginId.h + IPluginCallbacks.h (v1.2x .. 1.31.x).
struct CSquirrelVM;

enum class PluginString : int { NAME = 0, LOG_NAME = 1, DEPENDENCY_NAME = 2 };
enum class PluginField : int { CONTEXT = 0, COLOR = 1 };
namespace PluginContext { enum : uint64_t { DEDICATED = 0x1, CLIENT = 0x2 }; }

struct PluginNorthstarData
{
	HMODULE pluginHandle;
	uint64_t size;
};

class IPluginId
{
public:
	virtual const char *GetString(PluginString prop) = 0;
	virtual int64_t GetField(PluginField prop) = 0;
};

class IPluginCallbacks
{
public:
	virtual void Init(HMODULE northstarModule, const PluginNorthstarData *initData, bool reloaded) = 0;
	virtual void Finalize() = 0;
	virtual bool Unload() = 0;
	virtual void OnSqvmCreated(CSquirrelVM *sqvm) = 0;
	virtual void OnSqvmDestroying(CSquirrelVM *sqvm) = 0;
	virtual void OnLibraryLoaded(HMODULE module, const char *name) = 0;
	virtual void RunFrame() = 0;
};

namespace
{
	// ------------------------------------------------------------------------------------------------------ state + log
	HMODULE g_self = nullptr;
	std::wstring g_plugin_dir;   // ...\plugins
	std::wstring g_lib_dir;      // ...\plugins\lib
	FILE *g_log = nullptr;
	SRWLOCK g_log_lock = SRWLOCK_INIT;
	bool g_did_init = false;
	bool g_loaded_reshade = false;
	HMODULE g_reshade = nullptr;
	int g_proxy_warned = 0;

	std::string narrow(const std::wstring &w)
	{
		if (w.empty()) return std::string();
		const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		if (n <= 0) return std::string();
		std::string s(static_cast<size_t>(n), '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
		return s;
	}

	void logf(const char *fmt, ...)
	{
		char msg[2048];
		va_list ap;
		va_start(ap, fmt);
		const int n = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
		va_end(ap);
		(void)n;
		SYSTEMTIME t;
		GetLocalTime(&t);
		AcquireSRWLockExclusive(&g_log_lock);
		if (g_log)
		{
			fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
			fflush(g_log);
		}
		ReleaseSRWLockExclusive(&g_log_lock);
		char dbg[2100];
		_snprintf_s(dbg, sizeof(dbg), _TRUNCATE, "[TF2AI] %s\n", msg);
		OutputDebugStringA(dbg);
	}

	std::wstring module_path(HMODULE m)
	{
		std::vector<wchar_t> buf(1024);
		for (int tries = 0; tries < 6; ++tries)
		{
			const DWORD n = GetModuleFileNameW(m, buf.data(), static_cast<DWORD>(buf.size()));
			if (n == 0) return std::wstring();
			if (n < buf.size() - 1) return std::wstring(buf.data(), n);
			buf.resize(buf.size() * 2);
		}
		return std::wstring();
	}

	std::wstring parent_dir(const std::wstring &p)
	{
		const size_t s = p.find_last_of(L"\\/");
		return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
	}

	bool file_exists(const std::wstring &p)
	{
		const DWORD a = GetFileAttributesW(p.c_str());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool dir_exists(const std::wstring &p)
	{
		const DWORD a = GetFileAttributesW(p.c_str());
		return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool cmdline_has_in(const wchar_t *cl, const wchar_t *flag)
	{
		if (!cl || !flag || !*flag) return false;
		const size_t fl = wcslen(flag);
		for (const wchar_t *p = cl; (p = wcsstr(p, flag)) != nullptr; p += fl)
		{
			const bool start_ok = (p == cl) || p[-1] == L' ' || p[-1] == L'\t' || p[-1] == L'"';
			const wchar_t e = p[fl];
			const bool end_ok = e == L'\0' || e == L' ' || e == L'\t' || e == L'"' || e == L'=' || e == L'\r' || e == L'\n';
			if (start_ok && end_ok) return true;
		}
		return false;
	}

	// The process command line (GetCommandLineW) plus Northstar's view of it: Northstar 1.31.x hooks GetCommandLineA in
	// HookSys_Init (before plugins load) and appends ns_startup_args.txt there, so a flag written into that file is only
	// visible through GetCommandLineA. Our flags are ASCII: the A string is widened byte by byte.
	bool cmdline_has(const wchar_t *flag)
	{
		if (cmdline_has_in(GetCommandLineW(), flag)) return true;
		const char *a = GetCommandLineA();
		if (!a) return false;
		const size_t n = strnlen(a, 1u << 16);
		std::wstring w(n, L'\0');
		for (size_t i = 0; i < n; ++i) w[i] = static_cast<wchar_t>(static_cast<unsigned char>(a[i]));
		return cmdline_has_in(w.c_str(), flag);
	}

	// ------------------------------------------------------------------------------------------------ ReShade detection
	// A module in this process that exports the ReShade add-on / version API (ReShade under any file name).
	HMODULE loaded_reshade_module()
	{
		HMODULE mods[1024];
		DWORD needed = 0;
		if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
		DWORD count = needed / sizeof(HMODULE);
		if (count > 1024) count = 1024;
		for (DWORD i = 0; i < count; ++i)
		{
			if (!mods[i] || mods[i] == g_self) continue;
			// pin the module while its export table is read: GetModuleHandleExW(FROM_ADDRESS) only succeeds for an address
			// inside a module that is still loaded (no fault if another thread unloaded it since the enumeration)
			HMODULE pinned = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(mods[i]), &pinned) || !pinned)
				continue;
			const bool is_reshade = pinned == mods[i] &&
				(GetProcAddress(pinned, "ReShadeRegisterAddon") != nullptr || GetProcAddress(pinned, "ReShadeVersion") != nullptr);
			FreeLibrary(pinned);   // drops only the reference taken above
			if (is_reshade) return mods[i];
		}
		return nullptr;
	}

	// version.dll from System32 only, resolved at run time (never through the DLL search path). Unavailable = the version
	// resource check reports "not ReShade" and the export-name scan (bytes_say_reshade) still runs.
	using PFN_GetFileVersionInfoSizeW = DWORD(WINAPI *)(LPCWSTR, LPDWORD);
	using PFN_GetFileVersionInfoW = BOOL(WINAPI *)(LPCWSTR, DWORD, DWORD, LPVOID);
	using PFN_VerQueryValueW = BOOL(WINAPI *)(LPCVOID, LPCWSTR, LPVOID *, PUINT);
	struct VersionApi
	{
		PFN_GetFileVersionInfoSizeW size = nullptr;
		PFN_GetFileVersionInfoW info = nullptr;
		PFN_VerQueryValueW query = nullptr;
		bool tried = false;
	};
	VersionApi g_ver;

	bool version_api()
	{
		if (!g_ver.tried)
		{
			g_ver.tried = true;
			const HMODULE v = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
			const DWORD err = v ? 0 : GetLastError();
			if (v)
			{
				g_ver.size = reinterpret_cast<PFN_GetFileVersionInfoSizeW>(reinterpret_cast<void *>(GetProcAddress(v, "GetFileVersionInfoSizeW")));
				g_ver.info = reinterpret_cast<PFN_GetFileVersionInfoW>(reinterpret_cast<void *>(GetProcAddress(v, "GetFileVersionInfoW")));
				g_ver.query = reinterpret_cast<PFN_VerQueryValueW>(reinterpret_cast<void *>(GetProcAddress(v, "VerQueryValueW")));
			}
			if (!g_ver.size || !g_ver.info || !g_ver.query)
			{
				g_ver.size = nullptr;
				g_ver.info = nullptr;
				g_ver.query = nullptr;
				logf("note: System32 version.dll unavailable (error %lu) - ReShade detection uses the export-name scan only", err);
			}
			else
				logf("version.dll: %s", narrow(module_path(v)).c_str());
		}
		return g_ver.size && g_ver.info && g_ver.query;
	}

	// Version resource says ReShade (ProductName "ReShade", OriginalFilename "ReShade64.dll" / "ReShade32.dll").
	bool version_says_reshade(const std::wstring &path)
	{
		if (!version_api()) return false;
		DWORD dummy = 0;
		const DWORD size = g_ver.size(path.c_str(), &dummy);
		if (size == 0 || size > (16u << 20)) return false;
		std::vector<unsigned char> data(size + 16, 0);
		if (!g_ver.info(path.c_str(), 0, size, data.data())) return false;
		struct LangCp { WORD lang, cp; } *tr = nullptr;
		UINT tr_len = 0;
		if (!g_ver.query(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void **>(&tr), &tr_len) || !tr ||
			tr_len < sizeof(LangCp))
			return false;
		const UINT n = tr_len / sizeof(LangCp);
		for (UINT i = 0; i < n && i < 8; ++i)
		{
			const wchar_t *keys[] = { L"ProductName", L"OriginalFilename", L"FileDescription" };
			for (const wchar_t *k : keys)
			{
				wchar_t q[128];
				swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\%s", tr[i].lang, tr[i].cp, k);
				wchar_t *val = nullptr;
				UINT vlen = 0;
				if (g_ver.query(data.data(), q, reinterpret_cast<void **>(&val), &vlen) && val && vlen > 0 && vlen < 1024)
				{
					std::wstring v(val, wcsnlen(val, vlen));
					for (auto &c : v) c = static_cast<wchar_t>(towlower(c));
					if (v.find(L"reshade") != std::wstring::npos) return true;
				}
			}
		}
		return false;
	}

	// The file contains ReShade's add-on export name (bounded, chunked read; catches builds without a version resource).
	bool bytes_say_reshade(const std::wstring &path)
	{
		HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
			OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return false;
		static const char needle[] = "ReShadeRegisterAddon";
		const size_t nl = sizeof(needle) - 1;
		std::vector<char> buf(1 << 20);
		size_t keep = 0;
		bool found = false;
		uint64_t total = 0;
		for (;;)
		{
			DWORD got = 0;
			if (!ReadFile(h, buf.data() + keep, static_cast<DWORD>(buf.size() - keep), &got, nullptr) || got == 0) break;
			total += got;
			const size_t avail = keep + got;
			if (avail >= nl)
			{
				for (size_t i = 0; i + nl <= avail; ++i)
				{
					if (buf[i] == 'R' && memcmp(&buf[i], needle, nl) == 0) { found = true; break; }
				}
			}
			if (found || total > (256ull << 20)) break;
			keep = avail >= nl ? nl - 1 : avail;
			if (keep) memmove(buf.data(), buf.data() + avail - keep, keep);
		}
		CloseHandle(h);
		return found;
	}

	// ReShade proxy file names (ReShade's DllMain treats d3d*, dxgi, opengl32, dinput* as proxies).
	const wchar_t *const kProxyNames[] = { L"dxgi.dll", L"d3d11.dll", L"d3d10.dll", L"d3d10_1.dll", L"d3d9.dll", L"d3d12.dll",
		L"dinput8.dll", L"dinput.dll", L"opengl32.dll" };

	// Returns the first ReShade proxy found in bin\x64_retail, then the game folder (empty = none). bin\x64_retail comes
	// first: that is the copy Northstar's game DLLs bind to (a game-root copy next to it is not loaded), so the log names the
	// file that matters. Other (non-ReShade) proxy DLLs are only logged; a non-ReShade dxgi.dll (another DXGI wrapper) is
	// reported through *other_dxgi, because loading our proxy under the name dxgi.dll first would stop the game from ever
	// loading that wrapper.
	std::wstring find_reshade_proxy_on_disk(const std::wstring &game_dir, std::wstring *other_dxgi)
	{
		const std::wstring dirs[] = { game_dir + L"\\bin\\x64_retail", game_dir };
		for (const auto &d : dirs)
		{
			if (d.empty() || !dir_exists(d)) continue;
			for (const wchar_t *name : kProxyNames)
			{
				const std::wstring p = d + L"\\" + name;
				if (!file_exists(p)) continue;
				const bool by_ver = version_says_reshade(p);
				const bool by_bytes = by_ver ? true : bytes_say_reshade(p);
				if (by_ver || by_bytes)
				{
					logf("found a ReShade proxy on disk: %s (%s)", narrow(p).c_str(), by_ver ? "version resource" : "ReShade add-on export name");
					return p;
				}
				logf("note: non-ReShade proxy DLL present: %s (not a ReShade)", narrow(p).c_str());
				if (other_dxgi && other_dxgi->empty() && _wcsicmp(name, L"dxgi.dll") == 0) *other_dxgi = p;
			}
		}
		return std::wstring();
	}

	// -------------------------------------------------------------------------------------------------- ReShade.ini
	const char kDefaultIni[] =
		"[ADDON]\r\n"
		"DisabledAddons=\r\n"
		"\r\n"
		"[GENERAL]\r\n"
		"NoReloadOnInit=0\r\n"
		"\r\n"
		"[OVERLAY]\r\n"
		"TutorialProgress=4\r\n"
		"\r\n"
		"[RenoDX.DLSS5]\r\n"
		"DX11Source=native\r\n"
		"EnableHooks=2\r\n"
		"NeuralUplift=0\r\n"
		"NRPreUpscale=1\r\n"
		"NRFollowInputRes=1\r\n";

	bool ensure_ini(const std::wstring &ini)
	{
		if (file_exists(ini)) return true;
		HANDLE h = CreateFileW(ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) return file_exists(ini);
		DWORD wrote = 0;
		const BOOL ok = WriteFile(h, kDefaultIni, static_cast<DWORD>(sizeof(kDefaultIni) - 1), &wrote, nullptr);
		CloseHandle(h);
		logf("ReShade.ini was missing: wrote defaults (%s)", ok ? "ok" : "FAILED");
		return file_exists(ini);
	}

	void list_lib_dir()
	{
		WIN32_FIND_DATAW fd;
		HANDLE f = FindFirstFileW((g_lib_dir + L"\\*").c_str(), &fd);
		if (f == INVALID_HANDLE_VALUE) return;
		std::string names;
		int count = 0;
		do
		{
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			const std::wstring n = fd.cFileName;
			const size_t dot = n.find_last_of(L'.');
			const std::wstring ext = dot == std::wstring::npos ? L"" : n.substr(dot);
			if (_wcsicmp(ext.c_str(), L".addon64") == 0 || _wcsicmp(ext.c_str(), L".dll") == 0 || _wcsicmp(ext.c_str(), L".ini") == 0)
			{
				if (count++) names += ", ";
				names += narrow(n);
			}
		} while (FindNextFileW(f, &fd) && count < 64);
		FindClose(f);
		logf("plugins\\lib: %s", names.empty() ? "(empty)" : names.c_str());
		const bool renodx = file_exists(g_lib_dir + L"\\renodx-dlss5.addon64");
		const bool nr = file_exists(g_lib_dir + L"\\nvngx_dlssnr.dll");
		const bool dlss = file_exists(g_lib_dir + L"\\nvngx_dlss.dll");
		logf("DLSS runtime nvngx_dlss.dll: %s | DLSS 5 (renodx-dlss5.addon64 + nvngx_dlssnr.dll): %s%s", dlss ? "present" : "MISSING (DLSS unavailable: no upscaler, no frame generation)",
			renodx && nr ? "present" : "not installed", (renodx != nr) ? " (only one of the two files is present)" : "");
	}

	// ----------------------------------------------------------------------------------------------------- the loader
	void do_init(bool reloaded)
	{
		if (g_did_init) { logf("Init called again (reloaded=%d): nothing to do", reloaded ? 1 : 0); return; }
		g_did_init = true;

		const std::wstring exe = module_path(nullptr);
		const std::wstring game_dir = parent_dir(exe);
		logf("TF2 AI Enhancements loader %s | exe %s | plugin %s", kVersionTag + 26, narrow(exe).c_str(), narrow(module_path(g_self)).c_str());

		if (cmdline_has(L"-dedicated")) { logf("dedicated server: not loading ReShade"); return; }
		if (cmdline_has(L"-tf2ai_off")) { logf("-tf2ai_off on the command line: not loading ReShade"); return; }

		if (const HMODULE m = loaded_reshade_module())
		{
			logf("SKIP: a ReShade is already loaded in this process (%s) - using that one, not loading a second copy",
				narrow(module_path(m)).c_str());
			return;
		}
		std::wstring other_dxgi;
		const std::wstring proxy = find_reshade_proxy_on_disk(game_dir, &other_dxgi);
		if (!proxy.empty())
		{
			const std::wstring bin_prefix = game_dir + L"\\bin\\x64_retail\\";
			const bool in_bin = proxy.size() > bin_prefix.size() && _wcsnicmp(proxy.c_str(), bin_prefix.c_str(), bin_prefix.size()) == 0;
			if (in_bin)
				logf("SKIP: ReShade is installed in the game folder (%s). The game will load that one; the package's ReShade is NOT "
					 "loaded (a second ReShade would refuse to load and break the game's dxgi.dll). Put the package's *.addon64 next to "
					 "that ReShade if you want to use them with it.", narrow(proxy).c_str());
			else
				logf("SKIP: ReShade is installed in the game folder (%s: the Titanfall2 folder itself, not bin\\x64_retail). Northstar "
					 "normally loads the game's DLLs from bin\\x64_retail, so that copy is probably unused, but to never run two ReShades "
					 "the package's ReShade is NOT loaded. Delete that file (or move your ReShade to bin\\x64_retail) to use the "
					 "package's ReShade.", narrow(proxy).c_str());
			return;
		}

		// PROXY mode (preferred): load the package's ReShade under the name dxgi.dll (plugins\lib\proxy\dxgi.dll) before anything
		// else loads dxgi.dll. Windows then resolves every later "dxgi.dll" (the game's imports, GetModuleHandle("dxgi.dll") of
		// overlays) to this module, exactly like a ReShade dxgi.dll in bin\x64_retail - the route every add-on was developed and
		// tested on. Overlays that hook Present through a dummy swap chain (EA app igo64, Steam) then patch ReShade's proxy, not
		// System32 dxgi.dll's shared swap-chain Present code, which tf2-fg's presenter 2 must find unpatched (test run 1: with
		// hook mode that code was patched and frame generation stayed off).
		// HOOK mode (fallback): ReShade64.dll inline-hooks the System32 dxgi / d3d11 exports. Upscaling works; frame generation's
		// presenter 2 may refuse. Used when proxy\dxgi.dll is missing, dxgi.dll is already loaded, or with -tf2ai_hookmode.
		const std::wstring proxy_dll = g_lib_dir + L"\\proxy\\dxgi.dll";
		const std::wstring hook_dll = g_lib_dir + L"\\ReShade64.dll";
		const HMODULE pre_dxgi = GetModuleHandleW(L"dxgi.dll");
		bool use_proxy = file_exists(proxy_dll);
		if (!use_proxy) logf("note: %s missing - hook mode", narrow(proxy_dll).c_str());
		if (use_proxy && pre_dxgi) { use_proxy = false; logf("note: dxgi.dll is already loaded (%s) - proxy mode impossible, hook mode", narrow(module_path(pre_dxgi)).c_str()); }
		if (use_proxy && !other_dxgi.empty()) { use_proxy = false; logf("note: another (non-ReShade) dxgi.dll is installed (%s) - hook mode, so the game still loads that one", narrow(other_dxgi).c_str()); }
		if (use_proxy && cmdline_has(L"-tf2ai_hookmode")) { use_proxy = false; logf("note: -tf2ai_hookmode - hook mode"); }
		const std::wstring reshade = use_proxy ? proxy_dll : hook_dll;
		if (!file_exists(reshade)) { logf("ERROR: %s is missing - nothing loaded", narrow(reshade).c_str()); return; }

		// GAME\ReShade.ini [INSTALL] BasePath takes precedence over RESHADE_BASE_PATH_OVERRIDE in ReShade's get_base_path
		wchar_t bp[1024] = {};
		const std::wstring root_ini = game_dir + L"\\ReShade.ini";
		if (file_exists(root_ini) && GetPrivateProfileStringW(L"INSTALL", L"BasePath", L"", bp, 1024, root_ini.c_str()) > 0)
			logf("WARNING: %s has [INSTALL] BasePath=%s - ReShade will use that folder instead of plugins\\lib (remove that key)",
				narrow(root_ini).c_str(), narrow(bp).c_str());

		if (!ensure_ini(g_lib_dir + L"\\ReShade.ini")) logf("WARNING: could not create plugins\\lib\\ReShade.ini (ReShade may refuse to load)");
		list_lib_dir();

		SetEnvironmentVariableW(L"RESHADE_DISABLE_INPUT_HOOK", L"1");
		SetEnvironmentVariableW(L"RESHADE_BASE_PATH_OVERRIDE", g_lib_dir.c_str());
		logf("env: RESHADE_DISABLE_INPUT_HOOK=1, RESHADE_BASE_PATH_OVERRIDE=%s", narrow(g_lib_dir).c_str());

		const DWORD t0 = GetTickCount();
		HMODULE h = LoadLibraryExW(reshade.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!h)
		{
			logf("ERROR: LoadLibraryExW(%s) failed, GetLastError %lu (ReShade refuses to load when another ReShade is present or "
				 "ReShade.ini is missing; see plugins\\lib\\ReShade.log)", narrow(reshade).c_str(), GetLastError());
			return;
		}
		g_reshade = h;
		g_loaded_reshade = true;
		const bool api = GetProcAddress(h, "ReShadeRegisterAddon") != nullptr;
		logf("LOADED ReShade from the package in %s mode: %s (0x%p, %lu ms, add-on API %s). ReShade log: plugins\\lib\\ReShade.log",
			use_proxy ? "PROXY" : "HOOK", narrow(reshade).c_str(), static_cast<void *>(h), GetTickCount() - t0, api ? "present" : "MISSING");
		if (use_proxy)
		{
			const HMODULE by_name = GetModuleHandleW(L"dxgi.dll");
			logf("GetModuleHandle(\"dxgi.dll\") -> %s%s", narrow(module_path(by_name)).c_str(),
				by_name == h ? " (the package proxy: later dxgi.dll loads bind to it)" : " - WARNING: not the package proxy");
		}
	}

	// --------------------------------------------------------------------------------------------- interface objects
	class PluginId final : public IPluginId
	{
	public:
		const char *GetString(PluginString prop) override
		{
			switch (prop)
			{
			case PluginString::NAME: return "TF2 AI Enhancements";
			case PluginString::LOG_NAME: return "TF2AI";
			case PluginString::DEPENDENCY_NAME: return "TF2AIEnhancements";
			default: return nullptr;
			}
		}
		int64_t GetField(PluginField prop) override
		{
			switch (prop)
			{
			case PluginField::CONTEXT: return static_cast<int64_t>(PluginContext::CLIENT);
			case PluginField::COLOR: return 0x00B4FF40;   // R 0x40 G 0xFF B 0xB4 (low byte = red)
			default: return 0;
			}
		}
	};

	class PluginCallbacks final : public IPluginCallbacks
	{
	public:
		void Init(HMODULE, const PluginNorthstarData *, bool reloaded) override
		{
			try { do_init(reloaded); }
			catch (...) { logf("ERROR: exception in Init (caught)"); }
		}
		void Finalize() override {}
		bool Unload() override
		{
			try { logf("Unload: ReShade stays loaded (%s)", g_loaded_reshade ? "loaded by this plugin" : "not loaded by this plugin"); }
			catch (...) {}
			return true;
		}
		void OnSqvmCreated(CSquirrelVM *) override {}
		void OnSqvmDestroying(CSquirrelVM *) override {}
		void OnLibraryLoaded(HMODULE module, const char *name) override
		{
			try
			{
				if (!module || !name) return;
				// LoadLibraryEx(LOAD_LIBRARY_AS_DATAFILE / AS_IMAGE_RESOURCE) returns a tagged handle (low bits set): not an
				// executable module, skip it before any module API sees it
				if (reinterpret_cast<uintptr_t>(module) & 3) return;
				const size_t n = strnlen(name, 512);
				if (n == 0 || n >= 512) return;
				const char *base = name;
				for (size_t i = 0; i < n; ++i) if (name[i] == '\\' || name[i] == '/') base = name + i + 1;
				// diagnostics only: which dxgi / d3d11 / NGX runtime / add-on file the process really loaded
				const size_t bl = strlen(base);
				const bool addon = bl > 8 && _stricmp(base + bl - 8, ".addon64") == 0;
				if (_stricmp(base, "dxgi.dll") != 0 && _stricmp(base, "d3d11.dll") != 0 && _strnicmp(base, "nvngx", 5) != 0 && !addon) return;
				const std::string path = narrow(module_path(module));
				const bool is_reshade = GetProcAddress(module, "ReShadeRegisterAddon") != nullptr;
				logf("library loaded: %s -> %s%s", base, path.c_str(), is_reshade ? " (a ReShade proxy)" : "");
				if (is_reshade && g_loaded_reshade && module != g_reshade && g_proxy_warned++ == 0)
					logf("WARNING: a ReShade proxy was loaded after the package's ReShade - two ReShades in one process");
			}
			catch (...) {}
		}
		void RunFrame() override {}
	};

	PluginId g_id;
	PluginCallbacks g_callbacks;

	void open_log()
	{
		const std::wstring self = module_path(g_self);
		g_plugin_dir = parent_dir(self);
		g_lib_dir = g_plugin_dir + L"\\lib";
		const std::wstring dir = dir_exists(g_lib_dir) ? g_lib_dir : g_plugin_dir;
		if (dir.empty()) return;
		g_log = _wfsopen((dir + L"\\tf2-ai-loader.log").c_str(), L"w", _SH_DENYNO);
	}
}

extern "C" __declspec(dllexport) void *CreateInterface(const char *name, int *status)
{
	try
	{
		if (name)
		{
			if (strcmp(name, "PluginId001") == 0) { if (status) *status = 0; return static_cast<IPluginId *>(&g_id); }
			if (strcmp(name, "PluginCallbacks001") == 0) { if (status) *status = 0; return static_cast<IPluginCallbacks *>(&g_callbacks); }
			logf("CreateInterface: unknown interface '%.64s'", name);
		}
	}
	catch (...) {}
	if (status) *status = 1;
	return nullptr;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_self = module;
		DisableThreadLibraryCalls(module);
		try { open_log(); }
		catch (...) {}
	}
	return TRUE;
}
