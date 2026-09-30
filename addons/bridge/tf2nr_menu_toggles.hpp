// tf2nr_menu_toggles.hpp - read the TF2NR.GraphicsToggles convars (Northstar mod.json ConVars) from inside the
// ReShade add-on, READ-ONLY, and apply them:
//   tf2nr_dlss5_nr  0/1  -> DLSS5_Feed@DLSS5_Feed.fx + TF2_MotionVectors@TF2_MotionVectors.fx technique state
//   tf2nr_dlss_sr   0..5 -> tf2nr::g_sr_mode (consumed by the future DLSS SR path)
//
// Engine facts this relies on (checked 2026-09-28 against the installed binaries + NorthstarLauncher v1.31.9 source):
//  * vstdlib.dll exports CreateInterface(const char* name, int* rc); "VEngineCvar007" -> global CCvar.
//  * CCvar vtable slot 16 = FindVar(name): calls slot 14 FindCommandBase, returns nullptr if IsCommand() (vtbl+8).
//    (vstdlib.dll: vtable @ RVA 0x5B558, slot16 = RVA 0xABB0; Northstar cvar.h: M_VMETHOD FindVar 16.)
//  * ConVar layout (engine.dll ConVar::Create @ RVA 0x417230 writes these; Northstar convar.h agrees):
//      +0x00 vtable (engine.dll)   +0x18 const char* name   +0x28 int flags   +0x38 ConVar* parent (= this)
//      +0x40 default string        +0x48 char* string       +0x50 int64 strlen+1
//      +0x58 float value           +0x5C int value (= (int)float, cvttss2si)
//  * Northstar allocates mod ConVars with `new ConVar` and never frees them (reload_mods keeps them), so a
//    resolved pointer stays valid for the whole process.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <reshade.hpp>

namespace tf2nr
{
	// ---- public state -------------------------------------------------------------------------------------------
	// SR quality enum shared by the menu (video.menu list values), the convar and the future SR code.
	enum class sr_mode : int { off = 0, dlaa = 1, quality = 2, balanced = 3, performance = 4, ultra_performance = 5 };
	// Per-axis render scale for each mode (DLSS Programming Guide 310.6.0, 3.7.1 "Scaling Ratio 3.0 2.0 1.724 1.5 1.0").
	inline float sr_render_scale(sr_mode m)
	{
		switch (m)
		{
		case sr_mode::quality:           return 1.0f / 1.5f;
		case sr_mode::balanced:          return 1.0f / 1.724f;
		case sr_mode::performance:       return 1.0f / 2.0f;
		case sr_mode::ultra_performance: return 1.0f / 3.0f;
		default:                         return 1.0f;   // off, dlaa
		}
	}
	// NVSDK_NGX_PerfQuality_Value for each mode (MaxPerf 0, Balanced 1, MaxQuality 2, UltraPerformance 3, DLAA 5).
	inline int sr_ngx_perf_quality(sr_mode m)
	{
		switch (m)
		{
		case sr_mode::quality:           return 2;
		case sr_mode::balanced:          return 1;
		case sr_mode::performance:       return 0;
		case sr_mode::ultra_performance: return 3;
		case sr_mode::dlaa:              return 5;
		default:                         return -1;     // off: no SR feature
		}
	}

	inline volatile int g_nr_enabled = 1;           // last value read from tf2nr_dlss5_nr (1 = default/fallback)
	inline volatile int g_nr_read = 0;              // 1 once tf2nr_dlss5_nr was read (sr_next 0.4.1: an UNREAD convar never counts as NR on)
	inline volatile int g_sr_mode = 0;              // last value read from tf2nr_dlss_sr, clamped 0..5

	// ---- convar reader ------------------------------------------------------------------------------------------
	namespace cvar
	{
		using create_interface_fn = void *(*)(const char *name, int *return_code);
		using find_var_fn = void *(*)(void *self, const char *name);

		constexpr size_t kFindVarSlot = 16;
		constexpr size_t kOffVtable = 0x00, kOffName = 0x18, kOffParent = 0x38, kOffInt = 0x5C;

		struct range { uintptr_t lo = 0, hi = 0; bool has(const void *p) const { const auto v = reinterpret_cast<uintptr_t>(p); return v >= lo && v < hi; } };

		inline range module_range(HMODULE m)
		{
			range r;
			if (m == nullptr) return r;
			const auto base = reinterpret_cast<const uint8_t *>(m);
			const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE) return r;
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
			if (nt->Signature != IMAGE_NT_SIGNATURE) return r;
			r.lo = reinterpret_cast<uintptr_t>(base);
			r.hi = r.lo + nt->OptionalHeader.SizeOfImage;
			return r;
		}

		// All raw-pointer work lives in these small SEH functions (no C++ objects with destructors inside __try).
		inline void *seh_create_icvar(create_interface_fn ci)
		{
			__try { int rc = 1; void *p = ci("VEngineCvar007", &rc); return rc == 0 ? p : nullptr; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		}
		inline bool seh_icvar_ok(void *icvar, range vstd)
		{
			__try
			{
				void *const *vtbl = *reinterpret_cast<void *const *const *>(icvar);
				return vstd.has(vtbl) && vstd.has(vtbl[kFindVarSlot]);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		inline void *seh_find_var(void *icvar, const char *name)
		{
			__try
			{
				void *const *vtbl = *reinterpret_cast<void *const *const *>(icvar);
				return reinterpret_cast<find_var_fn>(vtbl[kFindVarSlot])(icvar, name);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		}
		inline bool seh_var_ok(const void *var, const char *name, range engine)
		{
			__try
			{
				const auto p = static_cast<const uint8_t *>(var);
				const void *vtbl = *reinterpret_cast<const void *const *>(p + kOffVtable);
				const char *n = *reinterpret_cast<const char *const *>(p + kOffName);
				return engine.has(vtbl) && n != nullptr && std::strcmp(n, name) == 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		inline bool seh_read_int(const void *var, int *out)
		{
			__try
			{
				const auto p = static_cast<const uint8_t *>(var);
				const uint8_t *parent = *reinterpret_cast<const uint8_t *const *>(p + kOffParent);
				if (parent == nullptr) parent = p;                          // engine sets parent = this; be lenient
				*out = *reinterpret_cast<const volatile int *>(parent + kOffInt);  // aligned 32-bit: atomic on x64
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		struct var_ref
		{
			const char *name;
			void *ptr = nullptr;
		};

		inline void *g_icvar = nullptr;
		inline range g_vstd, g_engine;

		// Cheap to call often; does real work only until both pointers are resolved.
		inline bool resolve(var_ref &v)
		{
			if (v.ptr != nullptr) return true;
			if (g_icvar == nullptr)
			{
				HMODULE vstd = GetModuleHandleW(L"vstdlib.dll");
				HMODULE eng = GetModuleHandleW(L"engine.dll");
				if (vstd == nullptr || eng == nullptr) return false;
				const auto ci = reinterpret_cast<create_interface_fn>(GetProcAddress(vstd, "CreateInterface"));
				if (ci == nullptr) return false;
				g_vstd = module_range(vstd);
				g_engine = module_range(eng);
				void *icvar = seh_create_icvar(ci);
				if (icvar == nullptr || !seh_icvar_ok(icvar, g_vstd)) return false;
				g_icvar = icvar;
			}
			void *p = seh_find_var(g_icvar, v.name);   // nullptr until Northstar registered the mod's ConVars
			if (p == nullptr || !seh_var_ok(p, v.name, g_engine)) return false;
			v.ptr = p;
			return true;
		}

		inline bool read_int(var_ref &v, int *out)
		{
			if (v.ptr == nullptr) return false;
			if (!seh_read_int(v.ptr, out)) { v.ptr = nullptr; return false; }  // re-resolve next time
			return true;
		}
	}

	// ---- applying the toggles -----------------------------------------------------------------------------------
	using log_fn = void (*)(const char *fmt, ...);

	inline cvar::var_ref g_var_nr { "tf2nr_dlss5_nr" };
	inline cvar::var_ref g_var_sr { "tf2nr_dlss_sr" };
	inline uint64_t g_poll_frame = 0;
	inline int g_applied_nr = -1;     // technique state last pushed (-1 = unknown -> push on next chance)
	inline bool g_logged_missing = false;

	// Push the desired state into the preset techniques. Returns false while ReShade is (re)loading effects
	// (find_technique returns {0} then) so the caller retries next frame.
	inline bool apply_nr(reshade::api::effect_runtime *rt, bool on, log_fn log)
	{
		const reshade::api::effect_technique feed = rt->find_technique("DLSS5_Feed.fx", "DLSS5_Feed");
		const reshade::api::effect_technique mv = rt->find_technique("TF2_MotionVectors.fx", "TF2_MotionVectors");
		if (feed.handle == 0) return false;
		const reshade::api::effect_technique hdr_in = rt->find_technique("TF2_HDRCut.fx", "TF2_HDR_Expose");
		const reshade::api::effect_technique hdr_out = rt->find_technique("TF2_HDRCut.fx", "TF2_HDR_Unexpose");
		bool took = true;
		for (const reshade::api::effect_technique tech : { feed, mv, hdr_in, hdr_out })
		{
			if (tech.handle == 0) continue;
			if (rt->get_technique_state(tech) != on) rt->set_technique_state(tech, on);
			took = took && rt->get_technique_state(tech) == on;   // another add-on can veto via reshade_set_technique_state
		}
		static bool vetoed_logged = false;
		if (!took)
		{
			if (log && !vetoed_logged) log("menu: DLSS 5 NR %s REQUESTED but a technique state change was refused - will retry", on ? "ON" : "OFF");
			vetoed_logged = true;
			return false;
		}
		vetoed_logged = false;
		if (log) log("menu: DLSS 5 NR %s (DLSS5_Feed%s%s)", on ? "ON" : "OFF", mv.handle ? " + TF2_MotionVectors" : "", hdr_in.handle ? " + TF2_HDRCut" : "");
		return true;
	}

	// Call once per frame from the add-on's present callback (NOT from a reshade_* callback), with the bridge's
	// current effect runtime. Also call force_reapply() from init_effect_runtime / reshade_reloaded_effects.
	inline void on_present(reshade::api::effect_runtime *rt, log_fn log)
	{
		const uint64_t frame = ++g_poll_frame;

		// Resolve at most every 60 frames until found (normally succeeds on the first try: Northstar registers mod
		// ConVars at engine.dll load, seconds before the swapchain exists).
		if ((g_var_nr.ptr == nullptr || g_var_sr.ptr == nullptr) && (frame % 60) == 1)
		{
			const bool a = cvar::resolve(g_var_nr), b = cvar::resolve(g_var_sr);
			if (a && b && log) log("menu: convars resolved (tf2nr_dlss5_nr=%p, tf2nr_dlss_sr=%p)", g_var_nr.ptr, g_var_sr.ptr);
			if (!(a && b) && frame > 60 * 60 && !g_logged_missing)
			{
				g_logged_missing = true;   // mod disabled/missing: keep the preset as-is (NR stays whatever it was)
				if (log) log("menu: TF2NR.GraphicsToggles convars not found after 60 s - menu toggles inactive");
			}
		}

		int v = 0;
		if (cvar::read_int(g_var_nr, &v)) { g_nr_enabled = v != 0 ? 1 : 0; g_nr_read = 1; }
		if (cvar::read_int(g_var_sr, &v))
		{
			const int m = v < 0 ? 0 : (v > 5 ? 5 : v);
			if (m != g_sr_mode && log) log("menu: DLSS SR mode %d -> %d (render scale %.3f)", g_sr_mode, m, sr_render_scale(static_cast<sr_mode>(m)));
			g_sr_mode = m;
		}

		if (rt == nullptr || g_var_nr.ptr == nullptr) return;   // no convar -> never touch the preset
		const int want = g_nr_enabled;
		// Push on change; also re-assert twice a second so the ReShade overlay / a preset reload cannot drift from
		// the menu (the convar is the single source of truth once the mod is installed).
		if (want != g_applied_nr || (frame % 30) == 0)
		{
			if (want != g_applied_nr)
			{
				if (apply_nr(rt, want != 0, log)) g_applied_nr = want;
			}
			else
			{
				apply_nr(rt, want != 0, nullptr);
			}
		}
	}

	inline void force_reapply() { g_applied_nr = -1; }
}
