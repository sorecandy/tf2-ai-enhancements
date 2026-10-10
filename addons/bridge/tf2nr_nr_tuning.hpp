// tf2nr_nr_tuning.hpp - live "DLSS 5 Adjustments" sliders: pushes the TF2NR.GraphicsToggles convars tf2nr_nr_* into
// renodx-dlss5 while the game runs.
//
// 2026-09-28 (research\renodx_internals.md, verified): renodx-dlss5 v7 reads [RenoDX.DLSS5] from ReShade's config
// exactly once, at DLL load; reshade::set_config_value alone changes nothing until a restart. But every setting lives
// in a std::atomic global that renodx re-reads on every NR evaluate, and the DLL carries a descriptor table in .rdata:
//   { const char *key, std::atomic<float> *f, std::atomic<uint32_t> *u, std::atomic<bool> *b, float default, u32 class,
//     char key_text[] }            (key points 40 bytes ahead at key_text for every record but the first)
// This header finds those records at runtime (no hard-coded RVAs, so it survives rebuilds that keep the layout),
// verifies every pointer lands in renodx's .data, then writes the atomics on a menu change (next evaluate picks them
// up) and persists the value with set_config_value so the next launch loads the same thing. Anything that does not
// check out disables the live writer: the values still persist and apply at the next start.
//
// NVIDIA's runtime resets NR's temporal history on every model-control change, so model keys are debounced (a slider
// drag must not restart NR every frame). The Look keys reshape NR's result after the model and are cheap.
//
// Include after tf2nr_menu_toggles.hpp; call tf2nr::tuning::update(logf) once per present after tf2nr::on_present().
#pragma once
#include <cmath>
#include <cstring>
#include <iterator>
#include "tf2nr_menu_toggles.hpp"

namespace tf2nr::tuning
{
	enum class kind : uint8_t { f32, u32, b8 };

	struct key
	{
		const char *cvar;      // mod.json convar ("" = internal, not menu-driven)
		const char *ini;       // [RenoDX.DLSS5] key == descriptor key text
		kind type;
		float lo, hi;          // hard clamp (Mod Settings text boxes accept any float)
		bool model;            // changes what the model makes -> NR history restarts -> long debounce
		cvar::var_ref ref { nullptr };
		volatile void *live = nullptr;   // renodx atomic (nullptr = not found -> persist-only)
		float seen = NAN;
		uint64_t seen_ms = 0;
		float pushed = NAN;
	};

	inline key g_keys[] = {
		{ "tf2nr_nr_intensity",     "NRIntensity",      kind::f32,  0.0f, 2.0f, true  },
		{ "tf2nr_nr_local_tone",    "NRLocalTone",      kind::f32,  0.0f, 2.0f, true  },
		{ "tf2nr_nr_structure",     "NRLocalStructure", kind::f32,  0.0f, 2.0f, true  },
		{ "tf2nr_nr_skin",          "NRSkinStructure",  kind::f32, -1.0f, 1.0f, true  },
		{ "tf2nr_nr_style",         "NRStyle",          kind::u32,  0.0f, 2.0f, true  },
		{ "tf2nr_nr_look_strength", "NRLookStrength",   kind::f32,  0.0f, 2.0f, false },
		{ "tf2nr_nr_look_colour",   "NRLookColour",     kind::f32,  0.0f, 2.0f, false },
		{ "tf2nr_nr_look_hue",      "NRLookHue",        kind::f32,  0.0f, 2.0f, false },
		{ "tf2nr_nr_look_darken",   "NRLookDarken",     kind::f32,  0.0f, 2.0f, false },
		{ "tf2nr_nr_look_detail",   "NRLookDetail",     kind::f32,  0.0f, 2.0f, false },
		{ "tf2nr_nr_stabilize",     "NRLookStabilize",  kind::u32,  0.0f, 2.0f, false },
	};
	// Internal keys located by the same walk: the Look sliders only act while NRLookMode is on, and NeuralUplift is
	// renodx's live on/off (its ini copy is saved lazily, so the menu sync reads this instead when found).
	inline key g_look_mode { "", "NRLookMode", kind::b8, 0.0f, 1.0f, false };
	inline key g_uplift { "", "NeuralUplift", kind::b8, 0.0f, 1.0f, false };
	inline cvar::var_ref g_enable { "tf2nr_nr_menu_tuning" };

	constexpr uint64_t kDebounceLookMs = 150, kDebounceModelMs = 400, kModelBatchGapMs = 500;
	constexpr char kSection[] = "RenoDX.DLSS5";

	inline bool g_walked = false, g_all_resolved = false, g_first_sync_done = false, g_logged_idle = false;
	inline int g_found = 0;
	inline uint64_t g_last_model_push_ms = 0, g_next_try_ms = 0;

	// ---------------------------------------------------------------- renodx descriptor walk (SEH only, no C++ objects)
	// Overflow-safe: a garbage qword near 2^64 must not wrap past the end check. Northstar's crash handler is a vectored
	// exception handler that kills the game on ANY access violation, even one our __except would catch - so the walk
	// must never fault, and the SEH below is only a last line of defence.
	struct span { uintptr_t lo = 0, hi = 0; bool has(uintptr_t v, size_t n = 1) const { return v >= lo && v < hi && n <= hi - v; } };

	inline bool seh_sections(HMODULE m, span *rdata, span *data)
	{
		__try
		{
			const auto base = reinterpret_cast<const uint8_t *>(m);
			const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
			const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
			for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
			{
				const uintptr_t lo = reinterpret_cast<uintptr_t>(base) + s[i].VirtualAddress;
				const uintptr_t hi = lo + s[i].Misc.VirtualSize;
				if (memcmp(s[i].Name, ".rdata", 6) == 0) { rdata->lo = lo; rdata->hi = hi; }
				else if (memcmp(s[i].Name, ".data", 5) == 0 && s[i].Name[5] == 0) { data->lo = lo; data->hi = hi; }
			}
			return rdata->lo != 0 && data->lo != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// Record at A: key* at A (a C string in .rdata equal to `name`), then f/u/b atomic pointers: the one for `type`
	// must be in .data (bool atomics are 1 byte, others 4-aligned); the others must be null or in .data.
	inline volatile void *seh_find_record(span rdata, span data, const char *name, kind type)
	{
		__try
		{
			const size_t n = strlen(name) + 1;
			volatile void *hit = nullptr;
			for (uintptr_t a = (rdata.lo + 7) & ~uintptr_t(7); a + 40 <= rdata.hi; a += 8)
			{
				const uintptr_t k = *reinterpret_cast<const uintptr_t *>(a);
				if (!rdata.has(k, n) || memcmp(reinterpret_cast<const char *>(k), name, n) != 0) continue;
				const uintptr_t f = *reinterpret_cast<const uintptr_t *>(a + 8);
				const uintptr_t u = *reinterpret_cast<const uintptr_t *>(a + 16);
				const uintptr_t b = *reinterpret_cast<const uintptr_t *>(a + 24);
				auto ok_or_null = [&](uintptr_t p) { return p == 0 || data.has(p); };
				if (!ok_or_null(f) || !ok_or_null(u) || !ok_or_null(b)) continue;
				const uintptr_t want = type == kind::f32 ? f : (type == kind::u32 ? u : b);
				if (want == 0 || !data.has(want, type == kind::b8 ? 1 : 4)) continue;
				if (type != kind::b8 && (want & 3) != 0) continue;
				if (hit != nullptr && hit != reinterpret_cast<volatile void *>(want)) return nullptr;   // ambiguous: refuse
				hit = reinterpret_cast<volatile void *>(want);
			}
			return hit;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
	}

	inline bool seh_read(volatile void *p, kind t, float *out)
	{
		__try
		{
			if (t == kind::f32) *out = *static_cast<volatile float *>(p);
			else if (t == kind::u32) *out = static_cast<float>(*static_cast<volatile uint32_t *>(p));
			else *out = *static_cast<volatile uint8_t *>(p) ? 1.0f : 0.0f;
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	inline bool seh_write(volatile void *p, kind t, float v)
	{
		__try
		{
			if (t == kind::f32) { LONG bits; memcpy(&bits, &v, 4); InterlockedExchange(static_cast<volatile LONG *>(p), bits); }
			else if (t == kind::u32) InterlockedExchange(static_cast<volatile LONG *>(p), static_cast<LONG>(v));
			else InterlockedExchange8(static_cast<volatile char *>(p), v != 0.0f ? 1 : 0);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	inline float quantize(const key &k, float v)
	{
		v = v < k.lo ? k.lo : (v > k.hi ? k.hi : v);
		return k.type == kind::f32 ? std::round(v * 1000.0f) / 1000.0f : std::round(v);
	}

	// Live value renodx is using right now (NAN if the record was not found).
	inline float live_value(const key &k)
	{
		float v = NAN;
		if (k.live == nullptr || !seh_read(k.live, k.type, &v)) return NAN;
		return v;
	}

	// renodx's live NR on/off: 1/0, or -1 when unknown (then fall back to the ini copy).
	inline int live_uplift()
	{
		const float v = live_value(g_uplift);
		return std::isnan(v) ? -1 : (v != 0.0f ? 1 : 0);
	}

	inline void walk(log_fn log)
	{
		HMODULE m = GetModuleHandleW(L"renodx-dlss5.addon64");
		g_walked = true;
		if (m == nullptr)
		{
			// renodx not loaded: the sliders still persist to [RenoDX.DLSS5], where tf2-nr-linux reads them live. Before,
			// update() retried this walk (and logged "convars resolved") every second for the whole session.
			if (log) log("tuning: renodx-dlss5 not loaded - adjustments persist to [RenoDX.DLSS5] only (tf2-nr-linux reads them)");
			return;
		}
		span rdata, data;
		if (!seh_sections(m, &rdata, &data)) { if (log) log("tuning: renodx sections not found - sliders apply at the next start only"); return; }
		auto find = [&](key &k) {
			k.live = seh_find_record(rdata, data, k.ini, k.type);
			float v = NAN;
			if (k.live != nullptr && (!seh_read(k.live, k.type, &v) || !(v >= k.lo - 1e-3f && v <= k.hi + 1e-3f)))
				k.live = nullptr;   // value outside the key's documented range: not the record we think it is
			if (k.live != nullptr) ++g_found;
			if (log) log("tuning: renodx %s %s (live %g)", k.ini, k.live ? "found" : "NOT found - persists only", v);
		};
		for (key &k : g_keys) find(k);
		find(g_look_mode);
		find(g_uplift);
		if (log) log("tuning: %d of %zu renodx settings writable live", g_found, std::size(g_keys) + 2);
	}

	// ---------------------------------------------------------------- push
	inline void push(key &k, float v, log_fn log, const char *why)
	{
		const bool live = k.live != nullptr && seh_write(k.live, k.type, v);
		if (k.type == kind::f32) reshade::set_config_value(nullptr, kSection, k.ini, v);
		else reshade::set_config_value(nullptr, kSection, k.ini, static_cast<int>(v));
		if (log) log("tuning: %s = %g (%s, %s)%s", k.ini, v, why, live ? "live" : "applies at next start", k.model ? " [NR history restarts]" : "");
		k.pushed = v;
	}

	inline bool read_float(cvar::var_ref &v, float *out)
	{
		if (v.ptr == nullptr) return false;
		__try
		{
			const auto p = static_cast<const uint8_t *>(v.ptr);
			const uint8_t *parent = *reinterpret_cast<const uint8_t *const *>(p + cvar::kOffParent);
			if (parent == nullptr) parent = p;
			*out = *reinterpret_cast<const volatile float *>(parent + 0x58);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { v.ptr = nullptr; return false; }
		return std::isfinite(*out);
	}

	inline void update(log_fn log)
	{
		if (cvar::g_icvar == nullptr || tf2nr::g_var_nr.ptr == nullptr) return;
		const uint64_t now = GetTickCount64();
		// ICvar::FindVar from the render thread races the main thread registering ConVars during Host_Init (crashed
		// in vstdlib 2026-09-28 19:08). Start 20 s after the base menu convars resolved: the game is past init then.
		static uint64_t s_base_ms = 0;
		if (s_base_ms == 0) s_base_ms = now;
		if (now - s_base_ms < 20000) return;
		if (!g_walked || !g_all_resolved)
		{
			if (now < g_next_try_ms) return;
			g_next_try_ms = now + 1000;
			if (!g_walked) walk(log);
			bool all = cvar::resolve(g_enable);
			for (key &k : g_keys)
			{
				if (k.ref.name == nullptr) k.ref.name = k.cvar;   // the table leaves ref unnamed; FindVar(nullptr) crashes vstdlib
				all = cvar::resolve(k.ref) && all;
			}
			if (!all)
			{
				if (!g_logged_idle && log) { log("tuning: DLSS 5 adjustment convars not registered (older menu mod) - idle"); g_logged_idle = true; }
				return;
			}
			g_all_resolved = true;
			if (log) log("tuning: %zu adjustment convars resolved", std::size(g_keys));
		}

		int enabled = 1;
		if (!cvar::read_int(g_enable, &enabled)) { g_all_resolved = false; return; }
		if (enabled == 0) { g_first_sync_done = false; return; }

		for (key &k : g_keys)
		{
			float raw;
			if (!read_float(k.ref, &raw)) { g_all_resolved = false; return; }
			// enum rows are int convars: their float field mirrors the int in Source ConVars, so one read covers both
			const float v = quantize(k, raw);
			if (!(v == k.seen)) { k.seen = v; k.seen_ms = now; }
		}

		// First sync after load (or after re-enabling): renodx loaded the ini; align it with the menu in one batch,
		// writing only keys that differ, and make sure the Look stage is on so the Look sliders act.
		if (!g_first_sync_done)
		{
			for (key &k : g_keys)
			{
				float cur = live_value(k);
				if (std::isnan(cur) && !reshade::get_config_value(nullptr, kSection, k.ini, cur)) cur = NAN;
				if (std::isnan(cur) || std::fabs(quantize(k, cur) - k.seen) > 1e-4f) push(k, k.seen, log, "menu value at start");
				else k.pushed = k.seen;
			}
			if (live_value(g_look_mode) == 0.0f) push(g_look_mode, 1.0f, log, "Look sliders need the Look stage");
			g_first_sync_done = true;
			g_last_model_push_ms = now;
			return;
		}

		bool model_due = false;
		for (key &k : g_keys)
		{
			if (k.seen == k.pushed) continue;
			if (now - k.seen_ms < (k.model ? kDebounceModelMs : kDebounceLookMs)) continue;   // still being dragged
			if (k.model) { model_due = true; continue; }
			push(k, k.seen, log, "menu");
		}
		if (model_due && now - g_last_model_push_ms >= kModelBatchGapMs)
		{
			for (key &k : g_keys)
				if (k.model && k.seen != k.pushed && now - k.seen_ms >= kDebounceModelMs) push(k, k.seen, log, "menu");
			g_last_model_push_ms = now;
		}
	}
}
