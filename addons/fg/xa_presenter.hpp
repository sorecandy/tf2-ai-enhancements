// xa_presenter.hpp - tf2-fg 0.4 Presenter=2 ("xa + DComp", research\fg_hdr\pacing.md section 6).
// Included by tf2_fg.cpp INSIDE its anonymous namespace, after logf / qpc_ms / safe_release / module_of / readable.
//
// Path of one game frame k (FG ACTIVE, presenter 2 engaged):
//   render thread, reshade_present(k): FSR writes G(k-1/2) straight into the shared slot G[s] (s = job % 3), R(k) (the back
//     buffer, HUD + ReShade overlay included) is copied into R[s], X.jobs[job] is published (atomic release) and the shared
//     D3D11 fence A is signalled with the job id (ID3D11DeviceContext4::Signal). No native Present, no GPU wait, no Flush
//     (the game's own proxied Present right after reshade_present submits the work). The game's own present still runs every
//     frame; while the DComp visual is attached it is hidden underneath.
//   pacer thread (THREAD_PRIORITY_HIGHEST): woken by fence A >= job (SetEventOnCompletion on N's view of A). N (own D3D12
//     device on the RTX, COPY queue = copy engine, concurrent with the game's 3D work) copies G then R into buffers of a
//     D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER heap, signalling fX = 2*job after G and 2*job+1 after R. M (own D3D12 device on the
//     display adapter, the Radeon 780M) copies each buffer into its composition flip swap chain (DComp topmost visual on the
//     game HWND) and presents: G as soon as fX >= 2*job, R at t(G present) + D/2 + PaceBiasMs (D = EMA of fence-A completion
//     intervals), then signals fM = job (M finished reading the X slot) and publishes `released` once N's copies are done
//     (CPU-side slot check for the render thread: slot s is reused by job+3 only when released >= job).
//   telemetry: GetFrameStatistics of the composition chain (samples with SyncRefreshCount == PresentRefreshCount only), QPC.
// States: the visual is attached after the first present of an engaged period (never shows an empty buffer) and detached
//   (SetRoot(nullptr) + Commit: DComp is not "sticky", the game's own frames are visible again at once) when the render
//   thread stops issuing jobs (suspend / OFF / minimize / cut of the presenter) - after the last issued job was presented.
// Lifetime: three layers, each created and released ONLY on the render thread (or at device/swap-chain destruction):
//   devices (N, M, queues, allocators, lists, local fences, DComp device) - once per game device, released on destroy_device,
//     errors, or 600 frames with FG OFF;
//   surfaces (shared slots, fence A, cross-adapter heap + buffers, fX/fM, composition swap chain, DComp target + visual) - per
//     swap chain / size, released on destroy_swapchain (resize too), device errors, OFF;
//   pacer thread - started after the surfaces, stopped (stop flag + event, joined with a timeout) BEFORE any surface or
//     device is released. A pacer that does not exit within 2 s is abandoned: nothing it may still use is ever released
//     (leaked on purpose), presenter 2 is off for the session. The thread pins the add-on module (GetModuleHandleEx) and
//     leaves through FreeLibraryAndExitThread, so an add-on unload can never unmap code it runs.
// Crash safety (Northstar VEH): every HRESULT is checked, pointers validated, every handle created once and closed once after
//   the thread that waits on it has exited, all waits bounded, no integer division by runtime values (D is a double).
// ReShade: DXGI through the System32 dxgi.dll's own CreateDXGIFactory2 (ReShade's proxy factory never sees the composition
//   swap chain); D3D12 devices through ID3D12DeviceFactory (independent devices, never the per-adapter singleton that
//   renodx-dlss5's private NR device is). 0.4.1: if ReShade wraps a device anyway (an init_device event fires during the
//   create) or the composition swap chain (init_swapchain / effect runtime during its create), presenter 2 REFUSES it (add-ons
//   would otherwise get events for our objects on the builder / pacer threads).
//
// 0.4.1 (M4 review fixes):
//   - The builder thread never touches the game's D3D11 device. ReShade holds the immediate context's ID3D11Multithread
//     critical section (protection ON) for the whole present / reshade_present processing, and while it is held other threads'
//     DXGI calls on the device's objects block (fg\tools\xa_lockprobe: IDXGIResource1::CreateSharedHandle waited 1.5 s). The
//     D3D11 slots, views, fence A and their NT handles are created on the render thread (create_surfaces_d3d11, ~1 ms after the
//     first time); the builder only opens the handles on N and creates the D3D12 / DXGI-composition / DComp objects.
//   - A teardown requested inside a ReShade event while the builder runs is DEFERRED (build_cancel + pend_level), never waited
//     for; poll_builder() on a later frame joins the finished builder and runs it. Only destroy_device may wait (bounded).
//   - Stalled pacer: the render thread watches the slot check (busy for >= 30 frames and >= 300 ms = stalled): it detaches the
//     visual itself (emergency_detach under dc_mx, which serialises every DComp call), asks the pacer to stop and polls for its
//     exit without blocking (poll_stall); a pacer that does not exit within 10 s is abandoned. The pacer also detaches on its
//     own when no job arrived for max(150 ms, 6 D) (hitches, a runtime that stopped calling reshade_present).
//   - Attach only after the R flip of a job with G: G was flipped D/2 earlier, so the first composition that shows the visual
//     shows this job's G or R, never the chain's stale front buffer (m4g: a 7 s old frame flashed on 2 of 2 resumes).
//   - DXGI frame statistics are sampled on a steady ~2 ms cadence (also between jobs); D = EMA of the jobs' publish intervals.
//   - The composition chain's Present / Present1 / GetFrameStatistics must be dxgi.dll's own code (no overlay hook on the pacer
//     thread) unless AllowPresentHooks=1; listed overlay DLLs (other than nvspcap64 / igo64) make presenter 2 not eligible.
//   - A G held behind the previous R waits a full refresh after R (at most D/2 - 0.5 ms), not half a refresh (XaGHold=1):
//     its copy is mostly done by then, so it reached the screen ~3 ms after R, in R's refresh half the time (m4r_moving10).

namespace xa
{
	constexpr int kSlots = 3;
	constexpr int kJobs = 64;
	constexpr int kNAlloc = 4;
	constexpr int kMAlloc = 8;
	constexpr int kRec = 1024;              // present records (indexed by present count)
	enum : int { T_R = 1, T_G = 2 };

	// ---------------------------------------------------------------- config (set by load_config)
	int cfg_presenter = -1;                 // -1 auto, 0 pass-through, 1 in-queue, 2 xa
	int cfg_spin_us = 500;                  // spin before the R target
	int cfg_device_mode = 0;                // 0 = ID3D12DeviceFactory, 1 = D3D12CreateDevice (singleton)
	int cfg_present_log = 0;                // 1 = tf2-fg-presents.csv (P / S rows) for dupmon joins
	int cfg_log_window = 600;               // pacer window (jobs)
	int cfg_allow_hooks = 0;                // AllowPresentHooks: accept a hooked composition Present / listed overlay DLLs
	int cfg_test_build_teardown = 0;        // XaTestTeardownInBuild=N (debug): a teardown while the N-th builder runs
	int cfg_test_stall_ms = 0;              // XaTestStallMs (debug): the pacer blocks once for this long ...
	int cfg_test_stall_at = 1500;           // ... at this job id (XaTestStallAtJob)
	int cfg_g_hold = 1;                     // XaGHold: G behind the previous R flipped no earlier than 0 = R + half a refresh (0.4.0;
	                                        // a G whose copy is already done: a full refresh), 1 = R + a full refresh (<= D/2 - 0.5 ms)
	int cfg_g_hold_ab = 0;                  // XaGHoldAB (debug): > 0 = alternate XaGHold 0 / 1 every N s (QPC time: even = 0, odd = 1)
	double cfg_off_clamp_ms = 2.0;          // |R offset from the latency EMAs| limit (0.4.0: 2 ms; m4r_moving10 with 1 ms: pinned at -1)

	// ---------------------------------------------------------------- topology (render thread)
	struct topo_t
	{
		bool known = false, eligible = false, windowed = true, same_adapter = false;
		LUID render = {}, display = {};
		UINT render_vendor = 0, display_vendor = 0;
		HMONITOR mon = nullptr;
		HWND hwnd = nullptr;
		wchar_t render_name[128] = L"", display_name[128] = L"", out_name[40] = L"";
		double refresh_hz = 0.0;
		uint32_t client_w = 0, client_h = 0, dpi = 0;
		const char *why = "";
	} topo;

	// ---------------------------------------------------------------- unwrapped DXGI (System32 dxgi.dll)
	using PFN_CDXGIF2 = HRESULT(WINAPI *)(UINT, REFIID, void **);
	HMODULE sys_dxgi = nullptr;
	PFN_CDXGIF2 create_factory2 = nullptr;
	bool dxgi_logged = false, dxgi_failed = false;

	// ---------------------------------------------------------------- devices layer
	struct dev_t
	{
		bool ok = false, failed = false;     // failed = creation failed this session (no retry)
		const char *fail = "";
		LUID render_luid = {}, display_luid = {};   // the adapters N and M were created on (0.4.1: from build_args, never `topo`)
		IDXGIFactory4 *factory = nullptr;
		IUnknown *proxyN = nullptr, *proxyM = nullptr;
		ID3D12Device *N = nullptr, *M = nullptr;
		bool wrappedN = false, wrappedM = false;
		ID3D12CommandQueue *nq = nullptr, *mq = nullptr;
		ID3D12CommandAllocator *nal[kNAlloc][2] = {}, *mal[kMAlloc] = {};
		uint64_t nal_v[kNAlloc] = {}, mal_v[kMAlloc] = {};
		ID3D12GraphicsCommandList *ncl[2] = {}, *mcl = nullptr;
		ID3D12Fence *nlocal = nullptr, *mlocal = nullptr;
		uint64_t nlocal_v = 0, mlocal_v = 0;
		HMODULE dcomp_mod = nullptr;
		IDCompositionDevice *dc = nullptr;
		double create_ms = 0.0;
	} D;

	// ---------------------------------------------------------------- surfaces layer
	struct surf_t
	{
		bool ok = false, failed = false, g_direct = true;
		const char *fail = "";
		uint64_t fail_frame = 0;
		uint32_t w = 0, h = 0;
		HWND hwnd = nullptr;
		uint64_t sc_gen = 0;
		ID3D11Device5 *dev5 = nullptr;
		ID3D11DeviceContext4 *ctx4 = nullptr;
		ID3D11Texture2D *R[kSlots] = {}, *G[kSlots] = {};
		ID3D11RenderTargetView *G_rtv[kSlots] = {};
		ID3D11UnorderedAccessView *G_uav[kSlots] = {};
		ID3D11Fence *fA = nullptr;
		ID3D12Resource *nR[kSlots] = {}, *nG[kSlots] = {};
		ID3D12Fence *nfA = nullptr, *nfX = nullptr, *nfM = nullptr;
		ID3D12Heap *nheap = nullptr, *mheap = nullptr;
		ID3D12Resource *nX[2 * kSlots] = {}, *mX[2 * kSlots] = {};
		ID3D12Fence *mfX = nullptr, *mfM = nullptr;
		IDXGISwapChain3 *msc = nullptr;
		ID3D12Resource *mbb[3] = {};
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fpN = {}, fpM = {};
		uint64_t bufsize = 0;
		IDCompositionTarget *dct = nullptr;
		IDCompositionVisual *dcv = nullptr;
		double create_ms = 0.0, create_d3d11_ms = 0.0;
	} S;
	std::timed_mutex dc_mx;   // every DComp call on the visual tree (pacer attach/detach, render-thread emergency detach, release)

	// ---------------------------------------------------------------- render thread <-> pacer
	struct job_t
	{
		uint64_t id = 0, frame = 0;
		int slot = 0;
		bool has_g = false;
		uint32_t seq_g = 0, seq_r = 0;
		double t_pub = 0.0;
	};
	struct shared_t
	{
		std::atomic<uint64_t> pub { 0 };          // last job published by the render thread
		std::atomic<uint64_t> released { 0 };     // last job whose slots N has finished copying (pacer)
		std::atomic<uint64_t> detach_req { 0 };   // render thread: detach after this job (monotonic)
		std::atomic<bool> attached { false };
		std::atomic<bool> stop { false };
		std::atomic<bool> running { false };
		std::atomic<int> error { 0 };             // pacer: fatal (it detached and exited)
		std::atomic<uint64_t> presented { 0 }, skipped { 0 }, attaches { 0 }, detaches { 0 };
		job_t jobs[kJobs];
	} X;
	HANDLE thread = nullptr, ev_fence = nullptr, ev_ctl = nullptr, ev_wait = nullptr, timer = nullptr;
	bool abandoned = false;                         // a pacer did not exit: nothing is released any more this session
	uint64_t next_job = 1;                          // render thread
	bool engaged = false;                           // render thread issued a job last frame
	uint64_t busy = 0, busy_total = 0, jobs_g = 0, jobs_r = 0;   // render-thread window counters
	const char *disabled_why = nullptr;             // presenter 2 off for the session (error), reason
	uint64_t off_since = 0;                         // frame at which FG went OFF with devices alive
	std::string evidence;                           // wrapping evidence summary (for the window log)
	// 0.4.1 render-thread watchdog / stall handling
	uint32_t busy_run = 0;                          // consecutive engaged frames whose slot was busy
	double t_last_pub = 0.0;                        // QPC ms of the last published job (or of the (re)engage)
	bool stalling = false;                          // stall detected: visual detached, pacer asked to stop, not joined yet
	double stall_t0 = 0.0;
	int stall_count = 0;
	uint64_t stalls_total = 0;
	// 0.4.1 present hooks: listed overlay DLLs loaded (render thread, scan_modules) / the composition chain's Present hooked
	// (hook_mods: the listed overlay DLLs other than nvspcap64 - declared in tf2_fg.cpp before scan_modules, which fills it)
	bool chain_hooked = false;
	char hook_why[200] = "";
	std::atomic<void *> last_cap_dev { nullptr };   // ReShade's device object from an init_device during our create (destroy log)
	const char *pacer_err_name(int e)
	{
		switch (e)
		{
		case 1: return "device removed (N fence)"; case 2: return "N copy timeout"; case 3: return "M allocator timeout";
		case 4: return "Present failed"; case 5: return "command list error"; case 6: return "exception"; case 7: return "fence event";
		case 8: return "N allocator timeout"; case 9: return "DComp busy"; default: return "none";
		}
	}

	// thread_local capture of init_device during our own device creation (ReShade wrapping evidence)
	thread_local int t_creating = 0;
	thread_local uint64_t t_cap_native = 0;
	thread_local void *t_cap_dev = nullptr;
	uint64_t n_init_d3d12 = 0;                      // init_device(d3d12) events seen (any source)
	thread_local int t_creating_sc = 0;             // inside CreateSwapChainForComposition
	uint64_t n_foreign_sc = 0, n_sc_during_ours = 0, n_foreign_rt = 0;   // init_swapchain (non-D3D11) / during ours / foreign effect runtimes
	uint64_t n_rt_during_ours = 0;                  // effect runtimes created on this thread during our composition swap chain create

	bool luid_eq(const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

	std::string bytes_at(const void *fn)
	{
		std::string s = module_of(fn);
		if (fn && readable(fn, 8))
		{
			const uint8_t *p = static_cast<const uint8_t *>(fn);
			char b[96]; snprintf(b, sizeof(b), " [%02x %02x %02x %02x %02x %02x]", p[0], p[1], p[2], p[3], p[4], p[5]);
			s += b;
			const void *target = nullptr;
			if (p[0] == 0xE9) { int32_t rel; memcpy(&rel, p + 1, 4); target = p + 5 + rel; }
			else if (p[0] == 0xFF && p[1] == 0x25) { int32_t rel; memcpy(&rel, p + 2, 4); const uint8_t *slot = p + 6 + rel; if (readable(slot, 8)) memcpy(&target, slot, 8); }
			if (target) s += " -> JMP " + module_of(target);
		}
		return s;
	}

	// 0.4.1: true when `fn` is not System32 dxgi.dll's own code: it lies in another module (vtable hook) or starts with a jump
	// into another module (inline hook). `why` names the module.
	bool fn_foreign(const void *fn, std::string *why)
	{
		HMODULE m = nullptr;
		if (!fn || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(fn), &m) || !m)
		{ *why = "no module"; return true; }
		if (m != sys_dxgi) { *why = module_of(fn); return true; }
		if (!readable(fn, 16)) { *why = "code not readable"; return true; }
		const uint8_t *p = static_cast<const uint8_t *>(fn);
		const void *target = nullptr;
		if (p[0] == 0xE9) { int32_t rel; memcpy(&rel, p + 1, 4); target = p + 5 + rel; }
		else if (p[0] == 0xFF && p[1] == 0x25) { int32_t rel; memcpy(&rel, p + 2, 4); const uint8_t *slot = p + 6 + rel; if (readable(slot, 8)) memcpy(&target, slot, 8); }
		else if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0) memcpy(&target, p + 2, 8);
		if (!target) return false;
		HMODULE tm = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(target), &tm) && tm == sys_dxgi) return false;
		*why = "inline jump -> " + module_of(target);
		return true;
	}

	// ---------------------------------------------------------------- unwrapped DXGI factory
	IDXGIFactory4 *sys_factory()
	{
		if (dxgi_failed) return nullptr;
		if (!create_factory2)
		{
			wchar_t sys[MAX_PATH] = {};
			const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
			if (n == 0 || n > MAX_PATH - 16) { dxgi_failed = true; logf("xa: GetSystemDirectory failed"); return nullptr; }
			std::wstring path = std::wstring(sys) + L"\\dxgi.dll";
			sys_dxgi = LoadLibraryExW(path.c_str(), nullptr, 0);
			const HMODULE rs = reshade::internal::get_reshade_module_handle();
			if (!sys_dxgi || sys_dxgi == rs)
			{
				dxgi_failed = true;
				logf("xa: %ls %s - presenter 2 unavailable", path.c_str(), sys_dxgi ? "is ReShade's own module" : "could not be loaded");
				return nullptr;
			}
			create_factory2 = reinterpret_cast<PFN_CDXGIF2>(GetProcAddress(sys_dxgi, "CreateDXGIFactory2"));
			if (!create_factory2) { dxgi_failed = true; logf("xa: System32 dxgi.dll has no CreateDXGIFactory2"); return nullptr; }
			logf("xa: unwrapped DXGI: %ls module %p (ReShade %p); CreateDXGIFactory2 = %s", path.c_str(), sys_dxgi, rs, bytes_at(reinterpret_cast<const void *>(create_factory2)).c_str());
		}
		IDXGIFactory4 *f = nullptr;
		HRESULT hr = E_FAIL;
		try { hr = create_factory2(0, __uuidof(IDXGIFactory4), reinterpret_cast<void **>(&f)); } catch (...) { hr = E_FAIL; f = nullptr; }
		if (FAILED(hr) || !f) { if (!dxgi_logged) logf("xa: System32 CreateDXGIFactory2 FAILED 0x%08x", static_cast<unsigned>(hr)); dxgi_logged = true; return nullptr; }
		if (!dxgi_logged)
		{
			dxgi_logged = true;
			void *const *vt = *reinterpret_cast<void *const *const *>(f);
			if (readable(vt, sizeof(void *) * 25))
				logf("xa: unwrapped factory %p: vtable QueryInterface -> %s | CreateSwapChainForComposition (slot 24) -> %s", f, bytes_at(vt[0]).c_str(), bytes_at(vt[24]).c_str());
		}
		return f;
	}

	// ---------------------------------------------------------------- topology
	// Display adapter = the adapter that really scans out the window's monitor. NOT the DXGI output enumeration: in a process
	// that runs on the dGPU of a hybrid laptop, DXGI reports the integrated panel's outputs under the dGPU (hybrid output
	// redirection, seen in the game: DISPLAY1 listed under the RTX). The kernel's view (gdi32
	// D3DKMTOpenAdapterFromGdiDisplayName on the monitor's GDI device name) gives the owning adapter's LUID.
	struct kmt_open_gdi { WCHAR DeviceName[32]; UINT hAdapter; LUID AdapterLuid; UINT VidPnSourceId; };
	struct kmt_close { UINT hAdapter; };
	using PFN_KMT_OPEN = LONG(APIENTRY *)(kmt_open_gdi *);
	using PFN_KMT_CLOSE = LONG(APIENTRY *)(const kmt_close *);
	bool kmt_display_luid(const wchar_t *gdi_name, LUID *out, LONG *status)
	{
		*status = -1;
		HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
		if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
		if (!gdi) return false;
		const auto op = reinterpret_cast<PFN_KMT_OPEN>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromGdiDisplayName"));
		const auto cl = reinterpret_cast<PFN_KMT_CLOSE>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
		if (!op || !cl) return false;
		kmt_open_gdi o = {};
		wcsncpy_s(o.DeviceName, gdi_name, _TRUNCATE);
		*status = op(&o);
		if (*status != 0) return false;
		*out = o.AdapterLuid;
		kmt_close c = { o.hAdapter };
		cl(&c);
		return true;
	}

	void evaluate_topology(IDXGISwapChain *sc, ID3D11Device *dev, uint32_t bb_w, uint32_t bb_h)
	{
		topo_t t;
		if (!sc || !dev) { t.why = "no swap chain / device"; topo = t; return; }
		DXGI_SWAP_CHAIN_DESC sd = {};
		if (FAILED(sc->GetDesc(&sd))) { t.why = "swap chain GetDesc failed"; topo = t; return; }
		t.hwnd = sd.OutputWindow; t.windowed = sd.Windowed != FALSE;
		IDXGIDevice *dx = nullptr;
		if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(&dx))) && dx)
		{
			IDXGIAdapter *ad = nullptr;
			if (SUCCEEDED(dx->GetAdapter(&ad)) && ad)
			{
				DXGI_ADAPTER_DESC d = {};
				if (SUCCEEDED(ad->GetDesc(&d))) { t.render = d.AdapterLuid; t.render_vendor = d.VendorId; wcsncpy_s(t.render_name, d.Description, _TRUNCATE); }
				ad->Release();
			}
			dx->Release();
		}
		t.mon = t.hwnd ? MonitorFromWindow(t.hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
		bool found = false;
		wchar_t dxgi_owner[128] = L"-";   // what DXGI output enumeration says (diagnostic only)
		MONITORINFOEXW mi = {}; mi.cbSize = sizeof(mi);
		if (t.mon && GetMonitorInfoW(t.mon, &mi))
		{
			wcsncpy_s(t.out_name, mi.szDevice, _TRUNCATE);
			LUID l = {}; LONG st = 0;
			if (kmt_display_luid(mi.szDevice, &l, &st))
			{
				if (IDXGIFactory4 *f = sys_factory())
				{
					IDXGIAdapter1 *a = nullptr;
					if (SUCCEEDED(f->EnumAdapterByLuid(l, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&a))) && a)
					{
						DXGI_ADAPTER_DESC1 ad = {};
						if (SUCCEEDED(a->GetDesc1(&ad))) { found = true; t.display = l; t.display_vendor = ad.VendorId; wcsncpy_s(t.display_name, ad.Description, _TRUNCATE); }
						a->Release();
					}
					// diagnostic: the adapter DXGI lists the output under in this process
					for (UINT i = 0; ; ++i)
					{
						IDXGIAdapter1 *e = nullptr;
						if (f->EnumAdapters1(i, &e) == DXGI_ERROR_NOT_FOUND || !e) break;
						for (UINT o = 0; ; ++o)
						{
							IDXGIOutput *out = nullptr;
							if (e->EnumOutputs(o, &out) == DXGI_ERROR_NOT_FOUND || !out) break;
							DXGI_OUTPUT_DESC od = {};
							if (SUCCEEDED(out->GetDesc(&od)) && od.Monitor == t.mon) { DXGI_ADAPTER_DESC1 ed = {}; e->GetDesc1(&ed); wcsncpy_s(dxgi_owner, ed.Description, _TRUNCATE); }
							out->Release();
						}
						e->Release();
					}
					f->Release();
				}
			}
			else logf("xa: D3DKMTOpenAdapterFromGdiDisplayName(%ls) failed (status 0x%08lx)", mi.szDevice, static_cast<unsigned long>(st));
		}
		if (found)
		{
			DEVMODEW dm = {}; dm.dmSize = sizeof(dm);
			if (EnumDisplaySettingsW(t.out_name, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) t.refresh_hz = static_cast<double>(dm.dmDisplayFrequency);
		}
		RECT rc = {};
		if (t.hwnd && GetClientRect(t.hwnd, &rc)) { t.client_w = static_cast<uint32_t>(rc.right - rc.left); t.client_h = static_cast<uint32_t>(rc.bottom - rc.top); }
		t.dpi = t.hwnd ? GetDpiForWindow(t.hwnd) : 0;
		t.known = found && t.render_vendor != 0;
		t.same_adapter = t.known && luid_eq(t.render, t.display);
		if (!found) t.why = "the window's monitor has no known display adapter (topology unknown)";
		else if (!t.windowed) t.why = "exclusive fullscreen (DComp needs a windowed/borderless swap chain)";
		else if (t.render_vendor != 0x10DE) t.why = "the game does not render on the NVIDIA GPU";
		else if (t.same_adapter) t.why = "the display is driven by the render adapter (MUX dGPU mode / external monitor on the RTX)";
		else if (t.client_w != bb_w || t.client_h != bb_h) t.why = "window client size differs from the back buffer (DComp shows the chain 1:1)";
		else { t.eligible = true; t.why = "hybrid: renders on the NVIDIA GPU, the window's display is on another adapter"; }
		topo = t;
		logf("xa: topology: render %ls (vendor %04x LUID %08lx:%08lx) | window %p monitor %p output %ls scanned out by %ls (vendor %04x LUID %08lx:%08lx; kernel view, DXGI lists it under %ls) %.0f Hz | "
		     "client %ux%u (DPI %u), back buffer %ux%u, windowed %d -> %s: %s",
			t.render_name, t.render_vendor, static_cast<unsigned long>(t.render.HighPart), static_cast<unsigned long>(t.render.LowPart), t.hwnd, t.mon,
			t.out_name, t.display_name, t.display_vendor, static_cast<unsigned long>(t.display.HighPart), static_cast<unsigned long>(t.display.LowPart), dxgi_owner, t.refresh_hz,
			t.client_w, t.client_h, t.dpi, bb_w, bb_h, t.windowed ? 1 : 0, t.eligible ? "presenter 2 ELIGIBLE" : "presenter 2 not eligible", t.why);
	}

	// ---------------------------------------------------------------- waits (bounded, re-checked: events may be stale)
	bool wait12(ID3D12Fence *f, uint64_t v, HANDLE ev, DWORD ms)
	{
		if (!f || !ev) return false;
		const double t_end = qpc_ms() + static_cast<double>(ms);
		for (;;)
		{
			const uint64_t c = f->GetCompletedValue();
			if (c == UINT64_MAX) return false;   // device removed
			if (c >= v) return true;
			const double now = qpc_ms();
			if (now >= t_end) return false;
			if (FAILED(f->SetEventOnCompletion(v, ev))) return false;
			const double rem = t_end - now;
			WaitForSingleObject(ev, static_cast<DWORD>(rem < 1.0 ? 1.0 : rem + 0.5));
		}
	}

	// ---------------------------------------------------------------- devices
	using PFN_D3D12GI = HRESULT(WINAPI *)(REFCLSID, REFIID, void **);
	using PFN_D3D12CD = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
	using PFN_DCOMP2 = HRESULT(WINAPI *)(IUnknown *, REFIID, void **);
	constexpr GUID kCLSID_D3D12DeviceFactory = { 0x114863bf, 0xc386, 0x4aee, { 0xb3, 0x9d, 0x8f, 0x0b, 0xbb, 0x06, 0x29, 0x55 } };
	constexpr GUID kReShadeD3D12DeviceProxy = { 0x2523AFF4, 0x978B, 0x4939, { 0xBA, 0x16, 0x8E, 0xE8, 0x76, 0xA4, 0xCB, 0x2A } };   // ReShade D3D12Device (to confirm a proxy)

	// Creates a D3D12 device on `adapter`. Out: the NATIVE device (AddRef'd) and, if ReShade wrapped the creation, the proxy.
	bool create_d3d12(IDXGIAdapter1 *adapter, const char *name, ID3D12Device **native_out, IUnknown **proxy_out, bool *wrapped)
	{
		*native_out = nullptr; *proxy_out = nullptr; *wrapped = false;
		HMODULE d12 = GetModuleHandleW(L"d3d12.dll");
		if (!d12) d12 = LoadLibraryW(L"d3d12.dll");
		if (!d12) { logf("xa: d3d12.dll could not be loaded"); return false; }
		const auto gi = reinterpret_cast<PFN_D3D12GI>(GetProcAddress(d12, "D3D12GetInterface"));
		const auto cd = reinterpret_cast<PFN_D3D12CD>(GetProcAddress(d12, "D3D12CreateDevice"));
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			logf("xa: d3d12.dll exports: D3D12GetInterface = %s | D3D12CreateDevice = %s", bytes_at(reinterpret_cast<const void *>(gi)).c_str(), bytes_at(reinterpret_cast<const void *>(cd)).c_str());
		}
		ID3D12Device *dev = nullptr;
		HRESULT hr = E_FAIL;
		const char *path = "?";
		t_cap_native = 0; t_cap_dev = nullptr;
		++t_creating;
		try
		{
			if (cfg_device_mode == 0 && gi)
			{
				ID3D12DeviceFactory *df = nullptr;
				hr = gi(kCLSID_D3D12DeviceFactory, __uuidof(ID3D12DeviceFactory), reinterpret_cast<void **>(&df));
				if (SUCCEEDED(hr) && df)
				{
					void *const *vt = *reinterpret_cast<void *const *const *>(df);
					static bool vt_logged = false;
					if (!vt_logged && readable(vt, sizeof(void *) * 10)) { vt_logged = true; logf("xa: ID3D12DeviceFactory %p: CreateDevice (vtable slot 9) = %s", df, bytes_at(vt[9]).c_str()); }
					df->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);
					hr = df->CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&dev));
					df->Release();
					path = "ID3D12DeviceFactory::CreateDevice (independent device)";
				}
				else { logf("xa: D3D12GetInterface(CLSID_D3D12DeviceFactory) FAILED 0x%08x", static_cast<unsigned>(hr)); dev = nullptr; }
			}
			if (!dev && cfg_device_mode == 1 && cd)
			{
				hr = cd(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&dev));
				path = "D3D12CreateDevice (per-adapter singleton)";
			}
		}
		catch (...) { hr = E_FAIL; dev = nullptr; }
		--t_creating;
		if (FAILED(hr) || !dev) { logf("xa: %s device creation FAILED 0x%08x (%s)", name, static_cast<unsigned>(hr), path); if (dev) dev->Release(); return false; }
		IUnknown *probe = nullptr;
		const bool answers_proxy = SUCCEEDED(dev->QueryInterface(kReShadeD3D12DeviceProxy, reinterpret_cast<void **>(&probe))) && probe;
		if (probe) probe->Release();
		const uint64_t cap = t_cap_native;
		if (cap != 0)
		{
			// 0.4.1: ReShade saw this device (init_device fired during the create): add-ons would get events for our devices,
			// queues and lists on the builder / pacer threads. Refused (presenter 2 unavailable this session).
			*wrapped = true;
			logf("xa: %s D3D12 device via %s: WRAPPED / SEEN by ReShade (init_device fired, native %llx, returned %p, answers ReShade's D3D12Device IID: %s) - REFUSED",
				name, path, static_cast<unsigned long long>(cap), dev, answers_proxy ? "yes" : "no");
			dev->Release();
			return false;
		}
		else
		{
			*native_out = dev;
			logf("xa: %s D3D12 device via %s: %p, NOT wrapped (no init_device event during the create; ReShade D3D12Device IID answered: %s)",
				name, path, dev, answers_proxy ? "YES - unexpected" : "no");
			if (answers_proxy) { dev->Release(); *native_out = nullptr; logf("xa: %s device is a ReShade proxy without an event - refused", name); return false; }
		}
		return true;
	}

	void release_devices()
	{
		for (auto &a : D.nal) for (auto *&x : a) safe_release(x);
		for (auto *&x : D.mal) safe_release(x);
		for (auto *&x : D.ncl) safe_release(x);
		safe_release(D.mcl);
		safe_release(D.nlocal); safe_release(D.mlocal);
		safe_release(D.nq); safe_release(D.mq);
		safe_release(D.dc);
		safe_release(D.N); safe_release(D.M);
		safe_release(D.proxyN); safe_release(D.proxyM);   // after the natives: destroy_device fires for them here
		safe_release(D.factory);
		for (auto &v : D.nal_v) v = 0;
		for (auto &v : D.mal_v) v = 0;
		D.nlocal_v = D.mlocal_v = 0;
		D.ok = false;
	}

	// lr / ld: the render / display adapter LUIDs, copied by the render thread (0.4.1: never read from `topo`, which the render
	// thread may rewrite while the builder runs)
	bool create_devices(LUID lr, LUID ld)
	{
		if (D.ok) return true;
		if (D.failed) return false;
		const double t0 = qpc_ms();
		auto fail = [&](const char *why, HRESULT hr) { logf("xa: devices: %s FAILED 0x%08x - presenter 2 unavailable this session", why, static_cast<unsigned>(hr)); D.fail = why; D.failed = true; release_devices(); return false; };
		D.factory = sys_factory();
		if (!D.factory) return fail("unwrapped DXGI factory", E_FAIL);
		D.render_luid = lr; D.display_luid = ld;
		IDXGIAdapter1 *an = nullptr, *am = nullptr;
		HRESULT hr = D.factory->EnumAdapterByLuid(lr, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&an));
		if (FAILED(hr) || !an) return fail("render adapter by LUID", hr);
		hr = D.factory->EnumAdapterByLuid(ld, __uuidof(IDXGIAdapter1), reinterpret_cast<void **>(&am));
		if (FAILED(hr) || !am) { an->Release(); return fail("display adapter by LUID", hr); }
		const uint64_t init_before = n_init_d3d12;
		const bool okN = create_d3d12(an, "N (render adapter)", &D.N, &D.proxyN, &D.wrappedN);
		const bool okM = okN && create_d3d12(am, "M (display adapter)", &D.M, &D.proxyM, &D.wrappedM);
		an->Release(); am->Release();
		if (!okN || !okM) return fail((D.wrappedN || D.wrappedM) ? "D3D12 device (ReShade wrapped it: refused)" : "D3D12 device", E_FAIL);
		{
			char b[256]; snprintf(b, sizeof(b), "N %s, M %s, init_device(d3d12) events during the creates %llu", D.wrappedN ? "WRAPPED" : "native", D.wrappedM ? "WRAPPED" : "native",
				static_cast<unsigned long long>(n_init_d3d12 - init_before));
			evidence = b;
		}
		D3D12_COMMAND_QUEUE_DESC qd = {};
		qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
		if (FAILED(hr = D.N->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&D.nq))) || !D.nq) return fail("N copy queue", hr);
		qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(hr = D.M->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&D.mq))) || !D.mq) return fail("M direct queue", hr);
		for (auto &a : D.nal) for (auto *&x : a)
			if (FAILED(hr = D.N->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&x))) || !x) return fail("N allocator", hr);
		for (auto *&x : D.mal)
			if (FAILED(hr = D.M->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&x))) || !x) return fail("M allocator", hr);
		for (int i = 0; i < 2; ++i)
		{
			if (FAILED(hr = D.N->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, D.nal[0][i], nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&D.ncl[i]))) || !D.ncl[i]) return fail("N command list", hr);
			D.ncl[i]->Close();
		}
		if (FAILED(hr = D.M->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D.mal[0], nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&D.mcl))) || !D.mcl) return fail("M command list", hr);
		D.mcl->Close();
		if (FAILED(hr = D.N->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&D.nlocal))) || !D.nlocal) return fail("N local fence", hr);
		if (FAILED(hr = D.M->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&D.mlocal))) || !D.mlocal) return fail("M local fence", hr);
		if (!D.dcomp_mod) D.dcomp_mod = LoadLibraryW(L"dcomp.dll");
		const auto dc2 = D.dcomp_mod ? reinterpret_cast<PFN_DCOMP2>(GetProcAddress(D.dcomp_mod, "DCompositionCreateDevice2")) : nullptr;
		if (!dc2) return fail("dcomp.dll DCompositionCreateDevice2", E_FAIL);
		hr = E_FAIL;
		try { hr = dc2(nullptr, __uuidof(IDCompositionDevice), reinterpret_cast<void **>(&D.dc)); } catch (...) { hr = E_FAIL; D.dc = nullptr; }
		if (FAILED(hr) || !D.dc) return fail("DCompositionCreateDevice2", hr);
		D.ok = true;
		D.create_ms = qpc_ms() - t0;
		logf("xa: devices ready in %.1f ms: N %p (copy queue %p), M %p (direct queue %p), DComp device %p | %s", D.create_ms, D.N, D.nq, D.M, D.mq, D.dc, evidence.c_str());
		return true;
	}

	// ---------------------------------------------------------------- surfaces
	// Callers of the release functions: the pacer is not running (joined) and both queues are idle (or abandoned: never called).
	// D3D12 / DXGI-composition / DComp part (created by the builder).
	void release_surfaces_x()
	{
		bool dc_ok = true;
		{
			std::unique_lock<std::timed_mutex> lk(dc_mx, std::defer_lock);
			if (lk.try_lock_for(std::chrono::milliseconds(200)))
			{
				if (S.dcv) S.dcv->SetContent(nullptr);
				if (S.dct) { S.dct->SetRoot(nullptr); if (D.dc) D.dc->Commit(); }
			}
			else { dc_ok = false; logf("xa: release: DComp mutex busy for 200 ms - the DComp target / visual are leaked"); }
		}
		X.attached = false;
		if (dc_ok) { safe_release(S.dcv); safe_release(S.dct); }
		else { S.dcv = nullptr; S.dct = nullptr; }
		for (auto *&b : S.mbb) safe_release(b);
		safe_release(S.msc);
		for (auto *&b : S.mX) safe_release(b);
		for (auto *&b : S.nX) safe_release(b);
		safe_release(S.mheap); safe_release(S.nheap);
		safe_release(S.mfX); safe_release(S.mfM); safe_release(S.nfX); safe_release(S.nfM); safe_release(S.nfA);
		for (auto *&r : S.nR) safe_release(r);
		for (auto *&r : S.nG) safe_release(r);
		S.bufsize = 0;
	}
	// Game-device part (created on the render thread; released only on the render thread / at device destruction).
	void release_surfaces_d3d11()
	{
		for (auto *&v : S.G_uav) safe_release(v);
		for (auto *&v : S.G_rtv) safe_release(v);
		for (auto *&t : S.G) safe_release(t);
		for (auto *&t : S.R) safe_release(t);
		safe_release(S.fA);
		safe_release(S.ctx4); safe_release(S.dev5);
	}
	void release_surfaces()
	{
		release_surfaces_x();
		release_surfaces_d3d11();
		S.ok = false; S.w = S.h = 0; S.hwnd = nullptr;
	}

	HANDLE share_d3d11(ID3D11Resource *r)
	{
		IDXGIResource1 *r1 = nullptr; HANDLE h = nullptr;
		if (SUCCEEDED(r->QueryInterface(__uuidof(IDXGIResource1), reinterpret_cast<void **>(&r1))) && r1)
		{
			if (FAILED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h))) h = nullptr;
			r1->Release();
		}
		return h;
	}

	// NT handles of the game-device slots and fence A: created by create_surfaces_d3d11 (render thread), each closed exactly
	// once by create_surfaces_x after N opened it, or by close_handles on any failure / cancel path.
	struct slot_handles { HANDLE R[kSlots] = {}, G[kSlots] = {}, A = nullptr; };
	void close_handles(slot_handles &hs)
	{
		for (auto &x : hs.R) if (x) { CloseHandle(x); x = nullptr; }
		for (auto &x : hs.G) if (x) { CloseHandle(x); x = nullptr; }
		if (hs.A) { CloseHandle(hs.A); hs.A = nullptr; }
	}

	// RENDER THREAD (0.4.1). The game device's part of the surfaces: 3 x {R, G} shared slots + views, fence A, their NT handles.
	// Never on the builder: the render thread may hold ReShade's D3D11 lock (ID3D11Multithread, protection on), and then DXGI
	// calls on the device's objects from another thread wait for it (xa_lockprobe: CreateSharedHandle blocked 1.5 s).
	// dev/imm: the game's NATIVE D3D11 device / immediate context. fmt_tl: the back buffer's typeless RGBA8 format.
	bool create_surfaces_d3d11(ID3D11Device *dev, ID3D11DeviceContext *imm, uint32_t w, uint32_t h, DXGI_FORMAT fmt_tl, DXGI_FORMAT fmt_srgb, DXGI_FORMAT fmt_unorm, uint64_t frame, slot_handles &hs)
	{
		if (S.ok || S.R[0] || S.fA) { logf("xa: surfaces %ux%u (game-device part): surfaces still alive - not created (the caller tears down first)", w, h); return false; }
		const double t0 = qpc_ms();
		HRESULT hr = S_OK;
		auto fail = [&](const char *why, HRESULT e) {
			logf("xa: surfaces %ux%u (game-device part): %s FAILED 0x%08x - presenter 2 off (retry in 600 frames)", w, h, why, static_cast<unsigned>(e));
			S.fail = why; S.failed = true; S.fail_frame = frame; close_handles(hs); release_surfaces(); return false; };
		if (!dev || !imm || w == 0 || h == 0) return fail("no device / size", E_INVALIDARG);
		if (fmt_tl != DXGI_FORMAT_R8G8B8A8_TYPELESS) return fail("back buffer is not RGBA8", E_INVALIDARG);
		if (FAILED(hr = dev->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void **>(&S.dev5))) || !S.dev5) return fail("ID3D11Device5", hr);
		if (FAILED(hr = imm->QueryInterface(__uuidof(ID3D11DeviceContext4), reinterpret_cast<void **>(&S.ctx4))) || !S.ctx4) return fail("ID3D11DeviceContext4", hr);
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt_tl; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
		td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
		S.g_direct = true;
		for (int i = 0; i < kSlots; ++i)
		{
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &S.R[i])) || !S.R[i]) return fail("shared R slot", hr);
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
			if (FAILED(dev->CreateTexture2D(&td, nullptr, &S.G[i])) || !S.G[i])
			{
				S.G[i] = nullptr; S.g_direct = false;   // no UAV on a shared texture: FSR writes gen, copied into the slot
				td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
				if (FAILED(hr = dev->CreateTexture2D(&td, nullptr, &S.G[i])) || !S.G[i]) return fail("shared G slot", hr);
			}
			D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = fmt_srgb; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			if (FAILED(hr = dev->CreateRenderTargetView(S.G[i], &rd, &S.G_rtv[i])) || !S.G_rtv[i]) return fail("G slot RTV", hr);
			if (S.g_direct)
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {}; ud.Format = fmt_unorm; ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				if (FAILED(dev->CreateUnorderedAccessView(S.G[i], &ud, &S.G_uav[i])) || !S.G_uav[i]) { S.G_uav[i] = nullptr; S.g_direct = false; }
			}
			if (!(hs.R[i] = share_d3d11(S.R[i]))) return fail("R slot shared handle", E_FAIL);
			if (!(hs.G[i] = share_d3d11(S.G[i]))) return fail("G slot shared handle", E_FAIL);
		}
		if (!S.g_direct) for (auto *&v : S.G_uav) safe_release(v);   // all-or-nothing: FSR writes gen for every slot then
		// fence A (game device -> N)
		if (FAILED(hr = S.dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), reinterpret_cast<void **>(&S.fA))) || !S.fA) return fail("D3D11 shared fence", hr);
		if (FAILED(hr = S.fA->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hs.A)) || !hs.A) { hs.A = nullptr; return fail("fence A shared handle", hr); }
		S.create_d3d11_ms = qpc_ms() - t0;
		return true;
	}

	// BUILDER THREAD (or the render thread for a SINGLETHREADED game device). The D3D12 / DXGI-composition / DComp part: opens
	// the slot and fence-A handles on N (closing each one), the cross-adapter heap + buffers, fX / fM, the composition swap
	// chain on M and the DComp target + visual. Never calls the game's D3D11 device. On failure / cancel only these objects are
	// released: the D3D11 slots belong to the render thread, which releases them when it takes the build's result.
	bool create_surfaces_x(uint32_t w, uint32_t h, HWND hwnd, uint64_t sc_gen, uint64_t frame, slot_handles &hs, const std::atomic<bool> *cancel)
	{
		const double t0 = qpc_ms();
		HRESULT hr = S_OK;
		auto fail = [&](const char *why, HRESULT e) {
			logf("xa: surfaces %ux%u: %s FAILED 0x%08x - presenter 2 off (retry in 600 frames)", w, h, why, static_cast<unsigned>(e));
			S.fail = why; S.failed = true; S.fail_frame = frame; close_handles(hs); release_surfaces_x(); return false; };
		auto cancelled = [&]() {
			if (!cancel || !cancel->load()) return false;
			logf("xa: surfaces %ux%u: build cancelled (a teardown was requested while the builder ran)", w, h);
			close_handles(hs); release_surfaces_x(); return true; };
		if (!D.ok || w == 0 || h == 0 || !hwnd || S.ok) return fail("devices not ready / surfaces alive", E_FAIL);
		for (int i = 0; i < kSlots; ++i) if (!hs.R[i] || !hs.G[i] || !S.R[i] || !S.G[i]) return fail("game-device slots / handles missing", E_FAIL);
		if (!hs.A || !S.fA) return fail("fence A / handle missing", E_FAIL);
		for (int i = 0; i < kSlots; ++i)
		{
			hr = D.N->OpenSharedHandle(hs.R[i], __uuidof(ID3D12Resource), reinterpret_cast<void **>(&S.nR[i]));
			CloseHandle(hs.R[i]); hs.R[i] = nullptr;
			if (FAILED(hr) || !S.nR[i]) return fail("N OpenSharedHandle(R slot)", hr);
			hr = D.N->OpenSharedHandle(hs.G[i], __uuidof(ID3D12Resource), reinterpret_cast<void **>(&S.nG[i]));
			CloseHandle(hs.G[i]); hs.G[i] = nullptr;
			if (FAILED(hr) || !S.nG[i]) return fail("N OpenSharedHandle(G slot)", hr);
		}
		hr = D.N->OpenSharedHandle(hs.A, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&S.nfA));
		CloseHandle(hs.A); hs.A = nullptr;
		if (FAILED(hr) || !S.nfA) return fail("N OpenSharedHandle(fence A)", hr);
		if (cancelled()) return false;
		// composition swap chain on M (unwrapped factory, NATIVE queue)
		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = w; sd.Height = h; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 3; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; sd.Scaling = DXGI_SCALING_STRETCH; sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		{
			IDXGISwapChain1 *s1 = nullptr;
			const uint64_t sc_before = n_sc_during_ours, rt_before = n_rt_during_ours, fsc_before = n_foreign_sc, frt_before = n_foreign_rt;
			++t_creating_sc;
			try { hr = D.factory->CreateSwapChainForComposition(D.mq, &sd, nullptr, &s1); } catch (...) { hr = E_FAIL; s1 = nullptr; }
			--t_creating_sc;
			if (FAILED(hr) || !s1) return fail("CreateSwapChainForComposition", hr);
			const bool wrapped = n_sc_during_ours != sc_before || n_rt_during_ours != rt_before;
			logf("xa: composition swap chain %p created through the unwrapped factory: ReShade init_swapchain / effect runtimes on this thread during the create %llu / %llu (any thread %llu / %llu) -> %s", s1,
				static_cast<unsigned long long>(n_sc_during_ours - sc_before), static_cast<unsigned long long>(n_rt_during_ours - rt_before),
				static_cast<unsigned long long>(n_foreign_sc - fsc_before), static_cast<unsigned long long>(n_foreign_rt - frt_before),
				wrapped ? "WRAPPED by ReShade - REFUSED" : "NOT wrapped: no ReShade runtime, no add-on sees it");
			if (wrapped) { s1->Release(); return fail("composition swap chain wrapped by ReShade (refused)", E_FAIL); }
			hr = s1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&S.msc));
			s1->Release();
			if (FAILED(hr) || !S.msc) return fail("IDXGISwapChain3", hr);
		}
		// 0.4.1: the pacer calls Present / GetFrameStatistics of this chain on its own thread. An overlay's Present hook (they
		// patch dxgi.dll's swap-chain code, shared by every chain) would then run on the pacer thread, concurrently with the
		// game's present, on a D3D12 queue of the 780M it never saw - VEH-fatal if it faults. They must be dxgi.dll's own code.
		{
			void *const *vt = *reinterpret_cast<void *const *const *>(S.msc);
			std::string hk;
			if (!readable(vt, sizeof(void *) * 23)) hk = "vtable not readable";
			else
			{
				const int slots[3] = { 8, 16, 22 };
				const char *names[3] = { "Present", "GetFrameStatistics", "Present1" };
				for (int i = 0; i < 3; ++i) { std::string why; if (fn_foreign(vt[slots[i]], &why)) hk += std::string(hk.empty() ? "" : "; ") + names[i] + " -> " + why; }
				logf("xa: composition swap chain vtable %p: Present (slot 8) = %s | GetFrameStatistics (16) = %s | Present1 (22) = %s", vt, bytes_at(vt[8]).c_str(), bytes_at(vt[16]).c_str(), bytes_at(vt[22]).c_str());
			}
			if (!hk.empty())
			{
				snprintf(hook_why, sizeof(hook_why), "composition swap chain hooked: %s", hk.c_str());
				if (!cfg_allow_hooks) { chain_hooked = true; return fail("composition swap chain Present hooked by another module (AllowPresentHooks=1 accepts it)", E_FAIL); }
				logf("xa: %s - accepted (AllowPresentHooks=1)", hook_why);
			}
		}
		for (UINT i = 0; i < 3; ++i)
			if (FAILED(hr = S.msc->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&S.mbb[i]))) || !S.mbb[i]) return fail("composition back buffer", hr);
		// footprints: N side = the slot texture, M side = the back buffer; equal row pitch required
		{
			const D3D12_RESOURCE_DESC dn = S.nR[0]->GetDesc(), dm = S.mbb[0]->GetDesc();
			UINT64 tn = 0, tm = 0;
			D.N->GetCopyableFootprints(&dn, 0, 1, 0, &S.fpN, nullptr, nullptr, &tn);
			D.M->GetCopyableFootprints(&dm, 0, 1, 0, &S.fpM, nullptr, nullptr, &tm);
			if (S.fpN.Footprint.RowPitch != S.fpM.Footprint.RowPitch || S.fpN.Footprint.Width != S.fpM.Footprint.Width || S.fpN.Footprint.Height != S.fpM.Footprint.Height || tn == 0)
			{
				logf("xa: footprints differ: N pitch %u %ux%u (%llu B), M pitch %u %ux%u (%llu B)", S.fpN.Footprint.RowPitch, S.fpN.Footprint.Width, S.fpN.Footprint.Height,
					static_cast<unsigned long long>(tn), S.fpM.Footprint.RowPitch, S.fpM.Footprint.Width, S.fpM.Footprint.Height, static_cast<unsigned long long>(tm));
				return fail("copyable footprints (row pitch)", E_FAIL);
			}
			S.bufsize = ((tn > tm ? tn : tm) + 65535ull) & ~65535ull;
		}
		// cross-adapter heap (N creates, M opens) + placed row-major buffers
		{
			D3D12_HEAP_DESC hd = {};
			hd.SizeInBytes = S.bufsize * 2 * kSlots; hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT; hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
			if (FAILED(hr = D.N->CreateHeap(&hd, __uuidof(ID3D12Heap), reinterpret_cast<void **>(&S.nheap))) || !S.nheap) return fail("cross-adapter heap", hr);
			HANDLE hh = nullptr;
			if (FAILED(hr = D.N->CreateSharedHandle(S.nheap, nullptr, GENERIC_ALL, nullptr, &hh)) || !hh) return fail("heap shared handle", hr);
			hr = D.M->OpenSharedHandle(hh, __uuidof(ID3D12Heap), reinterpret_cast<void **>(&S.mheap));
			CloseHandle(hh);
			if (FAILED(hr) || !S.mheap) return fail("M OpenSharedHandle(heap)", hr);
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = S.bufsize; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
			rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
			for (int i = 0; i < 2 * kSlots; ++i)
			{
				if (FAILED(hr = D.N->CreatePlacedResource(S.nheap, S.bufsize * i, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&S.nX[i]))) || !S.nX[i]) return fail("N placed buffer", hr);
				if (FAILED(hr = D.M->CreatePlacedResource(S.mheap, S.bufsize * i, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&S.mX[i]))) || !S.mX[i]) return fail("M placed buffer", hr);
			}
		}
		// fX (N -> M) and fM (M -> N), cross-adapter shared
		{
			HANDLE hf = nullptr;
			if (FAILED(hr = D.N->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&S.nfX))) || !S.nfX) return fail("fence X", hr);
			if (FAILED(hr = D.N->CreateSharedHandle(S.nfX, nullptr, GENERIC_ALL, nullptr, &hf)) || !hf) return fail("fence X handle", hr);
			hr = D.M->OpenSharedHandle(hf, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&S.mfX));
			CloseHandle(hf); hf = nullptr;
			if (FAILED(hr) || !S.mfX) return fail("M OpenSharedHandle(fence X)", hr);
			if (FAILED(hr = D.M->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&S.mfM))) || !S.mfM) return fail("fence M", hr);
			if (FAILED(hr = D.M->CreateSharedHandle(S.mfM, nullptr, GENERIC_ALL, nullptr, &hf)) || !hf) return fail("fence M handle", hr);
			hr = D.N->OpenSharedHandle(hf, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&S.nfM));
			CloseHandle(hf);
			if (FAILED(hr) || !S.nfM) return fail("N OpenSharedHandle(fence M)", hr);
		}
		if (cancelled()) return false;
		// DComp target (topmost) + visual with the chain as content; attached (SetRoot) by the pacer after its first R flip of a
		// job with G (no pacer runs yet: dc_mx is free)
		{
			std::lock_guard<std::timed_mutex> lk(dc_mx);
			hr = D.dc->CreateTargetForHwnd(hwnd, TRUE, &S.dct);
			if (FAILED(hr) || !S.dct) { S.dct = nullptr; }
			else if (FAILED(hr = D.dc->CreateVisual(&S.dcv)) || !S.dcv) { S.dcv = nullptr; }
			else if (FAILED(hr = S.dcv->SetContent(S.msc))) {}
		}
		if (!S.dct) return fail("DComp CreateTargetForHwnd (topmost; another topmost target on the window?)", hr);
		if (!S.dcv) return fail("DComp CreateVisual", hr);
		if (FAILED(hr)) return fail("DComp SetContent", hr);
		S.w = w; S.h = h; S.hwnd = hwnd; S.sc_gen = sc_gen; S.ok = true; S.failed = false;
		S.create_ms = qpc_ms() - t0;
		logf("xa: surfaces ready in %.1f ms (+ %.1f ms game-device part on the render thread): %ux%u, 3 slots (G %s), cross-adapter heap %llu MB (%d buffers x %llu KB, pitch %u), composition swap chain %p (3 x FLIP_DISCARD R8G8B8A8_UNORM), DComp target %p (topmost) visual %p",
			S.create_ms, S.create_d3d11_ms, w, h, S.g_direct ? "written by FSR directly (UAV)" : "copied from gen (no UAV on shared textures)", static_cast<unsigned long long>(S.bufsize * 2 * kSlots >> 20), 2 * kSlots,
			static_cast<unsigned long long>(S.bufsize >> 10), S.fpN.Footprint.RowPitch, S.msc, S.dct, S.dcv);
		return true;
	}

	// Render thread: a re-check of the composition chain's Present code (a hook installed after the chain was created). true =
	// hooked now (and not accepted): presenter 2 goes off for the session.
	bool chain_hook_recheck()
	{
		if (!S.ok || !S.msc || cfg_allow_hooks || chain_hooked) return chain_hooked;
		void *const *vt = *reinterpret_cast<void *const *const *>(S.msc);
		std::string hk;
		if (!readable(vt, sizeof(void *) * 23)) hk = "vtable not readable";
		else { std::string why; if (fn_foreign(vt[8], &why)) hk = "Present -> " + why; else if (fn_foreign(vt[22], &why)) hk = "Present1 -> " + why; }
		if (hk.empty()) return false;
		snprintf(hook_why, sizeof(hook_why), "composition swap chain hooked after its creation: %s", hk.c_str());
		chain_hooked = true;
		logf("xa: %s - presenter 2 off for this session (AllowPresentHooks=1 accepts it)", hook_why);
		return true;
	}

	// Render thread: fresh surfaces -> fresh job numbering (the pacer is not running).
	void reset_jobs()
	{
		X.pub = 0; X.released = 0; X.detach_req = 0; X.attached = false; X.error = 0;
		next_job = 1; engaged = false; busy_run = 0; t_last_pub = 0.0;
		for (auto &j : X.jobs) j = job_t {};
	}

	// ---------------------------------------------------------------- pacer
	struct prec_t { uint32_t pcount = 0; uint8_t type = 0; uint64_t job = 0; double t_call = 0, t_target = 0, t_ref = 0; int64_t sync = 0; UINT sync_refresh = 0; bool final_ = false; };
	struct pwin_t
	{
		uint64_t jobs = 0, jobs_g = 0, skipped = 0, pres_g = 0, pres_r = 0, shown_g = 0, shown_r = 0, fin_g = 0, fin_r = 0, pairs = 0, errors = 0, stats_err = 0, mismatch = 0;
		uint64_t hist[6] = {};
		uint64_t pat[4][4] = {}, agg_g = 0, agg_r = 0;   // 0.4.1: (G, R) refresh-count pairs of shown triples; refresh sums
		std::vector<float> late, pcpu, wake_g, share, iv, d, g_ready, lat_g, lat_r, offs;
		double t0 = 0.0; uint64_t cpu0 = 0;
		void reset(double now, uint64_t cpu) { *this = pwin_t {}; t0 = now; cpu0 = cpu; }
	};

	uint64_t thread_cpu_100ns()
	{
		FILETIME c, e, k, u;
		if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0;
		return (static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) + (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime);
	}

	struct pacer_args { float bias_ms; double refresh_ms; };
	pacer_args g_pargs;

	struct pacer_t
	{
		prec_t rec[kRec];
		uint32_t last_seen = 0;
		pwin_t w;
		FILE *csv = nullptr;
		std::string buf;
		double D = 0.0, prev_pub = 0.0; uint64_t prev_job = 0;   // D: frame interval EMA from the jobs' publish times
		// on-screen latency EMAs from the frame statistics: G from its READY time, R from its flip call. R's target is
		// shifted by latG - latR so that the two images reach the screen D/2 apart (m4c: G ready->screen ~1.5-2 ms longer
		// than R call->screen, share 0.47 / triple p50 0.42 without it)
		double latG = 0.0, latR = 0.0; uint64_t nlatG = 0, nlatR = 0;
		uint32_t last_pc = 0;   // present count of the last flip
		double last_r_call = 0.0;   // flip call of the last R (the next G is not flipped before it had half a refresh)
		uint64_t held_g = 0;       // G flips held back by that rule (window)
		uint64_t held_g_full = 0;  // of them held to a full refresh after R: G's copy had already finished (window)
		int nai = 0, mai = 0;
		uint64_t pending_release = 0;
		uint64_t registered = 0;   // fence-A value the fence event is registered for
		uint64_t detach_done = 0;
		uint32_t fin_next = 1;     // next present count to finalize
		uint64_t xjob[kSlots] = {}; // last processed job per X slot (skipped jobs never signal fM)
		bool prev_fresh = false;   // 0.4.1: the previous job's wake came from a wait (fence event), not from a busy pacer
		double t_last_job = 0.0;   // 0.4.1: end of the last processed job (idle detach)
		uint64_t idle_detaches = 0, d_skipped = 0;   // window: idle detaches, late wakes (the job was complete before the pacer waited)
		double comp_rate = 0.0;    // DComp current composition rate (Hz), sampled per window
	};
	bool test_stall_done = false;  // XaTestStallMs: once per session
	pacer_t *P = nullptr;   // owned by the pacer thread while it runs (allocated/freed by the render thread around it)

	double qpc_to_ms(int64_t q)
	{
		static double f = 0.0;
		if (f == 0.0) { LARGE_INTEGER x; QueryPerformanceFrequency(&x); f = static_cast<double>(x.QuadPart); }
		return f > 0.0 ? static_cast<double>(q) * 1000.0 / f : 0.0;
	}

	void sample_stats()
	{
		DXGI_FRAME_STATISTICS fs = {};
		const HRESULT hr = S.msc->GetFrameStatistics(&fs);
		if (FAILED(hr)) { ++P->w.stats_err; return; }
		if (fs.PresentCount == 0 || fs.PresentCount == P->last_seen) return;
		P->last_seen = fs.PresentCount;
		if (fs.SyncRefreshCount != fs.PresentRefreshCount) { ++P->w.mismatch; return; }
		prec_t &r = P->rec[fs.PresentCount % kRec];
		if (r.pcount == fs.PresentCount && r.sync == 0)
		{
			r.sync = fs.SyncQPCTime.QuadPart; r.sync_refresh = fs.SyncRefreshCount;
			const double lat = qpc_to_ms(r.sync) - r.t_ref;
			if (r.t_ref > 0.0 && lat > 0.0 && lat < 50.0)
			{
				if (r.type == T_G) { P->latG = P->nlatG++ == 0 ? lat : P->latG + 0.02 * (lat - P->latG); P->w.lat_g.push_back(static_cast<float>(lat)); }
				else { P->latR = P->nlatR++ == 0 ? lat : P->latR + 0.02 * (lat - P->latR); P->w.lat_r.push_back(static_cast<float>(lat)); }
			}
			if (cfg_present_log && P->csv) { char b[96]; snprintf(b, sizeof(b), "S,%u,%lld,%u\n", fs.PresentCount, static_cast<long long>(r.sync), fs.SyncRefreshCount); P->buf += b; }
		}
	}

	// Records older than 16 presents are final: shown (a DXGI display time) or not.
	void finalize(uint32_t pc_now)
	{
		if (pc_now <= 16) return;
		const uint32_t upto = pc_now - 16;
		if (upto > kRec - 32 && P->fin_next < upto - (kRec - 32)) P->fin_next = upto - (kRec - 32);
		for (uint32_t pc = P->fin_next; pc <= upto; ++pc)
		{
			P->fin_next = pc + 1;
			prec_t &r = P->rec[pc % kRec];
			if (r.pcount != pc || r.final_) continue;
			r.final_ = true;
			const bool shown = r.sync != 0;
			if (r.type == T_G) { ++P->w.fin_g; if (shown) ++P->w.shown_g; }
			else { ++P->w.fin_r; if (shown) ++P->w.shown_r; }
			if (!shown) continue;
			const prec_t &p = P->rec[(pc - 1) % kRec];
			if (p.pcount == pc - 1 && p.sync != 0)
			{
				const double iv = qpc_to_ms(r.sync - p.sync);
				if (iv >= 0.0 && iv < 1000.0) P->w.iv.push_back(static_cast<float>(iv));
				const UINT dr = r.sync_refresh - p.sync_refresh;
				P->w.hist[dr > 5 ? 5 : dr]++;
			}
			// G share for (G(j), R(j), G(j+1)): at R(j) with G(j) = pc - 1 and G(j+1) = pc + 1 (also final or at least recorded)
			if (r.type == T_R && p.pcount == pc - 1 && p.type == T_G && p.job == r.job && p.sync != 0)
			{
				const prec_t &n = P->rec[(pc + 1) % kRec];
				if (n.pcount == pc + 1 && n.type == T_G && n.job == r.job + 1 && n.sync != 0)
				{
					const double g2r = qpc_to_ms(r.sync - p.sync), r2g = qpc_to_ms(n.sync - r.sync);
					if (g2r > 0.0 && r2g > 0.0 && g2r + r2g < 200.0) { P->w.share.push_back(static_cast<float>(g2r / (g2r + r2g))); ++P->w.pairs; }
					// 0.4.1: the same triple on the refresh grid (DXGI refresh counts = scanout): (G refreshes, R refreshes)
					const UINT rg = r.sync_refresh - p.sync_refresh, rr = n.sync_refresh - r.sync_refresh;
					if (rg < 50 && rr < 50) { P->w.pat[rg > 3 ? 3 : rg][rr > 3 ? 3 : rr]++; P->w.agg_g += rg; P->w.agg_r += rr; }
				}
			}
		}
	}

	float pctf(std::vector<float> v, double p)
	{
		if (v.empty()) return NAN;
		const size_t i = std::min(v.size() - 1, static_cast<size_t>(p * static_cast<double>(v.size() - 1) + 0.5));
		std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(i), v.end());
		return v[i];
	}

	void pacer_log_window(double now)
	{
		pwin_t &w = P->w;
		const double secs = (now - w.t0) / 1000.0;
		const uint64_t cpu = thread_cpu_100ns();
		const double cpu_ms = static_cast<double>(cpu - w.cpu0) / 10000.0;
		const double gshow = w.fin_g ? 100.0 * static_cast<double>(w.shown_g) / static_cast<double>(w.fin_g) : NAN;
		const double rshow = w.fin_r ? 100.0 * static_cast<double>(w.shown_r) / static_cast<double>(w.fin_r) : NAN;
		// 0.4.1: DComp's composition rate (a fixed 165 Hz grid vs VRR) - diagnostic for the fixed-refresh finding
		{
			std::unique_lock<std::timed_mutex> lk(dc_mx, std::defer_lock);
			DCOMPOSITION_FRAME_STATISTICS fs = {};
			if (D.dc && lk.try_lock_for(std::chrono::milliseconds(5)) && SUCCEEDED(D.dc->GetFrameStatistics(&fs)) && fs.currentCompositionRate.Denominator)
				P->comp_rate = static_cast<double>(fs.currentCompositionRate.Numerator) / static_cast<double>(fs.currentCompositionRate.Denominator);
		}
		uint64_t pt = 0; for (auto &a : w.pat) for (auto v : a) pt += v;
		auto pp = [&](int g, int r) { return pt ? 100.0 * static_cast<double>(w.pat[g][r]) / static_cast<double>(pt) : NAN; };
		const double pother = pt ? 100.0 - pp(1, 1) - pp(1, 2) - pp(2, 1) - pp(2, 2) : NAN;
		logf("presenter2 window %llu jobs (%.1f s, %.1f jobs/s; G %llu, R-only %llu, skipped %llu) | presents G %llu R %llu = %.1f/s | DXGI-sampler view: shown G %.1f %% R %.1f %% (final G %llu R %llu) | "
		     "G share p5 %.3f p50 %.3f p95 %.3f (pairs %llu) | refresh grid: (G,R) refreshes (1,1) %.1f %% (1,2) %.1f %% (2,1) %.1f %% (2,2) %.1f %% other %.1f %% (triples %llu), aggregate G share %.3f | "
		     "on-screen interval ms p5 %.2f p50 %.2f p95 %.2f, refreshes 1:%llu 2:%llu 3:%llu 4:%llu 5+:%llu | "
		     "R lateness ms p50 %.3f p95 %.3f max %.3f | wake->G present ms p50 %.2f p95 %.2f | G call->ready ms p50 %.2f p95 %.2f | to screen ms G (ready) p50 %.2f R (call) p50 %.2f, R offset ms p50 %.2f | present CPU ms p50 %.3f | D ms %.2f (from publish times; late wakes %llu) | pacer CPU %.1f ms = %.3f ms/job (%.1f %% of a core) | "
		     "stats errors %llu, refresh mismatch %llu | G flips held for the previous R %llu (to a full refresh %llu; XaGHold %d) | idle detaches %llu | DComp composition rate %.2f Hz | attached %d",
			w.jobs, secs, secs > 0 ? w.jobs / secs : 0.0, w.jobs_g, w.jobs - w.jobs_g, w.skipped, w.pres_g, w.pres_r, secs > 0 ? (w.pres_g + w.pres_r) / secs : 0.0, gshow, rshow, w.fin_g, w.fin_r,
			pctf(w.share, 0.05), pctf(w.share, 0.5), pctf(w.share, 0.95), w.pairs, pp(1, 1), pp(1, 2), pp(2, 1), pp(2, 2), pother, static_cast<unsigned long long>(pt),
			(w.agg_g + w.agg_r) ? static_cast<double>(w.agg_g) / static_cast<double>(w.agg_g + w.agg_r) : NAN,
			pctf(w.iv, 0.05), pctf(w.iv, 0.5), pctf(w.iv, 0.95), w.hist[1], w.hist[2], w.hist[3], w.hist[4], w.hist[5],
			pctf(w.late, 0.5), pctf(w.late, 0.95), w.late.empty() ? NAN : *std::max_element(w.late.begin(), w.late.end()), pctf(w.wake_g, 0.5), pctf(w.wake_g, 0.95), pctf(w.g_ready, 0.5), pctf(w.g_ready, 0.95), pctf(w.lat_g, 0.5), pctf(w.lat_r, 0.5), pctf(w.offs, 0.5),
			pctf(w.pcpu, 0.5), P->D, static_cast<unsigned long long>(P->d_skipped), cpu_ms, w.jobs ? cpu_ms / static_cast<double>(w.jobs) : 0.0, secs > 0 ? cpu_ms / (secs * 10.0) : 0.0, w.stats_err, w.mismatch,
			static_cast<unsigned long long>(P->held_g), static_cast<unsigned long long>(P->held_g_full), cfg_g_hold_ab > 0 ? -1 : cfg_g_hold, static_cast<unsigned long long>(P->idle_detaches), P->comp_rate, X.attached.load() ? 1 : 0);
		P->held_g = 0; P->held_g_full = 0; P->d_skipped = 0; P->idle_detaches = 0;
		if (cfg_present_log && P->csv && !P->buf.empty()) { fwrite(P->buf.data(), 1, P->buf.size(), P->csv); fflush(P->csv); P->buf.clear(); }
		w.reset(now, cpu);
	}

	// Pacer thread. 0.4.1: every DComp call under dc_mx (the render thread may detach in an emergency).
	void attach(uint64_t job)
	{
		if (X.attached.load() || !S.dct || !S.dcv) return;
		std::lock_guard<std::timed_mutex> lk(dc_mx);
		if (FAILED(S.dct->SetRoot(S.dcv)) || FAILED(D.dc->Commit())) { logf("xa: attach (SetRoot + Commit) FAILED - presenter error"); X.error = 5; return; }
		X.attached = true; ++X.attaches;
		if (X.attaches.load() <= 20) logf("xa: DComp visual ATTACHED after the R flip of job %llu (its G was flipped D/2 earlier: no stale front buffer) (attach #%llu)", job, X.attaches.load());
	}
	void detach(const char *why)
	{
		if (!X.attached.load() || !S.dct) return;
		std::lock_guard<std::timed_mutex> lk(dc_mx);
		S.dct->SetRoot(nullptr);
		if (D.dc) D.dc->Commit();
		X.attached = false; ++X.detaches;
		if (X.detaches.load() <= 20) logf("xa: DComp visual DETACHED (%s, detach #%llu) - the game's own presents are visible", why, X.detaches.load());
	}
	// RENDER THREAD (0.4.1): the pacer is stalled or abandoned - take the visual off the window without it, so the game's own
	// frames are visible again. Only a pacer stuck inside a DComp call itself (holding dc_mx) defeats this.
	void emergency_detach(const char *why)
	{
		if (!S.dct || !D.dc) return;
		std::unique_lock<std::timed_mutex> lk(dc_mx, std::defer_lock);
		if (!lk.try_lock_for(std::chrono::milliseconds(50))) { logf("xa: EMERGENCY detach (%s): DComp mutex held by the pacer for 50 ms - visual NOT detached", why); return; }
		const HRESULT h1 = S.dct->SetRoot(nullptr), h2 = D.dc->Commit();
		const bool was = X.attached.exchange(false);
		if (was) ++X.detaches;
		logf("xa: EMERGENCY detach by the render thread (%s): SetRoot(nullptr) 0x%08x, Commit 0x%08x (visual was %s) - the game's own presents are visible", why, static_cast<unsigned>(h1), static_cast<unsigned>(h2), was ? "attached" : "not attached");
	}

	void sleep_until(double target)
	{
		const double spin = cfg_spin_us / 1000.0;
		for (;;)
		{
			const double rem = target - qpc_ms();
			if (rem <= spin + 0.5 || !timer || X.stop.load()) break;
			// wake every 2 ms and sample the frame statistics (a present shown for a single refresh is otherwise missed)
			const double step = rem - spin > 2.0 ? 2.0 : rem - spin;
			LARGE_INTEGER due; due.QuadPart = -static_cast<LONGLONG>(step * 10000.0);
			if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) break;
			WaitForSingleObject(timer, static_cast<DWORD>(step + 20.0));
			sample_stats();
		}
		while (qpc_ms() < target && !X.stop.load()) YieldProcessor();
	}

	// M: record + submit the copy of X buffer `xb` into the CURRENT back buffer, behind a queue wait for fX >= fx_value, then
	// signal mlocal. Out: the mlocal value that marks "copy done" (= the image is ready for its flip). false = fatal.
	bool m_copy(int xb, uint64_t fx_value, uint64_t *ready_v)
	{
		const int a = P->mai++ % kMAlloc;
		if (!wait12(D.mlocal, D.mal_v[a], ev_wait, 100)) { X.error = 3; return false; }
		const UINT bi = S.msc->GetCurrentBackBufferIndex();
		if (bi >= 3 || !S.mbb[bi]) { X.error = 5; return false; }
		if (FAILED(D.mal[a]->Reset()) || FAILED(D.mcl->Reset(D.mal[a], nullptr))) { X.error = 5; return false; }
		D3D12_RESOURCE_BARRIER b = {};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = S.mbb[bi]; b.Transition.Subresource = 0;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		D.mcl->ResourceBarrier(1, &b);
		D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = S.mbb[bi]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = S.mX[xb]; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = S.fpM;
		D.mcl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
		D.mcl->ResourceBarrier(1, &b);
		if (FAILED(D.mcl->Close())) { X.error = 5; return false; }
		D.mq->Wait(S.mfX, fx_value);
		ID3D12CommandList *l = D.mcl;
		D.mq->ExecuteCommandLists(1, &l);
		D.mq->Signal(D.mlocal, ++D.mlocal_v); D.mal_v[a] = D.mlocal_v;
		*ready_v = D.mlocal_v;
		return true;
	}

	// Flip the current back buffer (its copy was submitted by m_copy). false = fatal.
	bool m_flip(int type, uint32_t seq, uint64_t job, uint64_t frame, double t_target, double *t_call_out)
	{
		sample_stats();
		const double t_call = qpc_ms();
		const HRESULT hr = S.msc->Present(0, 0);
		const double t_ret = qpc_ms();
		if (t_call_out) *t_call_out = t_call;
		if (FAILED(hr))
		{
			logf("xa: composition Present FAILED 0x%08x (M device removed reason 0x%08x) - presenter error", static_cast<unsigned>(hr), static_cast<unsigned>(D.M->GetDeviceRemovedReason()));
			X.error = 4; return false;
		}
		UINT pc = 0; S.msc->GetLastPresentCount(&pc);
		prec_t &r = P->rec[pc % kRec];
		r = prec_t {}; r.pcount = pc; r.type = static_cast<uint8_t>(type); r.job = job; r.t_call = t_call; r.t_target = t_target;
		if (type == T_R) r.t_ref = t_call;
		P->last_pc = pc;
		if (type == T_G) ++P->w.pres_g; else ++P->w.pres_r;
		P->w.pcpu.push_back(static_cast<float>(t_ret - t_call));
		if (type == T_R && t_target > 0.0) P->w.late.push_back(static_cast<float>(t_call - t_target));
		++X.presented;
		if (cfg_present_log && P->csv)
		{
			char bl[200]; snprintf(bl, sizeof(bl), "P,%u,%d,%llu,%llu,%.3f,%.3f,%.3f,%u\n", seq, type, static_cast<unsigned long long>(job), static_cast<unsigned long long>(frame), t_call, t_ret, t_target, pc);
			P->buf += bl;
		}
		finalize(pc);
		return true;
	}

	// One job: N copies (G then R), M copies queued at once, G flipped at once, R flipped at t(G ready) + D/2 + bias.
	// fresh: the pacer waited for this job's fence (counted as "late wakes" when not). D = EMA of the interval between the render
	// thread's publish times of consecutive jobs (0.4.1: wake times were biased low by late wakes; learning only from on-time
	// wakes - the first 0.4.1 - biased it HIGH under frame-time jitter: early frames arrive while the pacer sleeps for R, so only
	// the long intervals were sampled; m4r_moving10 D 20.0 ms at 52.1 fps).
	bool process(uint64_t j, float bias_ms, bool fresh)
	{
		const job_t jb = X.jobs[j % kJobs];
		if (jb.id != j || jb.slot < 0 || jb.slot >= kSlots) { ++P->w.errors; return true; }
		const double t_wake = qpc_ms();
		if (P->prev_job && j == P->prev_job + 1 && P->prev_pub > 0.0 && jb.t_pub > 0.0)
		{
			const double dt = jb.t_pub - P->prev_pub;
			if (dt >= 100.0) P->D = 0.0;
			else if (dt > 0.0) P->D = P->D <= 0.0 ? dt : P->D + 0.2 * (dt - P->D);
		}
		if (!fresh) ++P->d_skipped;
		P->prev_pub = jb.t_pub; P->prev_job = j; P->prev_fresh = fresh;
		if (cfg_test_stall_ms > 0 && !test_stall_done && j == static_cast<uint64_t>(cfg_test_stall_at))
		{
			// debug (XaTestStallMs): a pacer stuck for that long (e.g. in a driver call) - the render thread's watchdog must
			// detach the visual itself and never wait for this thread inside a ReShade event
			test_stall_done = true;
			logf("xa: TEST: the pacer blocks for %d ms at job %llu (XaTestStallMs)", cfg_test_stall_ms, static_cast<unsigned long long>(j));
			Sleep(static_cast<DWORD>(cfg_test_stall_ms));
			logf("xa: TEST: the pacer resumes after its %d ms block (stop requested: %d)", cfg_test_stall_ms, X.stop.load() ? 1 : 0);
			if (X.stop.load()) return true;
		}
		const int xs = jb.slot;
		// N: copy engine -> cross-adapter buffers
		const int a = P->nai++ % kNAlloc;
		if (!wait12(D.nlocal, D.nal_v[a], ev_wait, 100)) { X.error = 8; return false; }
		auto record = [&](int li, ID3D12Resource *srcTex, int xb) -> bool {
			if (FAILED(D.nal[a][li]->Reset()) || FAILED(D.ncl[li]->Reset(D.nal[a][li], nullptr))) return false;
			D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = S.nX[xb]; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = S.fpN;
			D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = srcTex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
			D.ncl[li]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			return SUCCEEDED(D.ncl[li]->Close());
		};
		if ((jb.has_g && !record(0, S.nG[xs], 2 * xs)) || !record(1, S.nR[xs], 2 * xs + 1)) { X.error = 5; return false; }
		D.nq->Wait(S.nfA, j);
		if (P->xjob[xs] != 0) D.nq->Wait(S.nfM, P->xjob[xs]);   // M has finished reading this X slot (its last processed job)
		P->xjob[xs] = j;
		ID3D12CommandList *l0 = D.ncl[0], *l1 = D.ncl[1];
		if (jb.has_g) { D.nq->ExecuteCommandLists(1, &l0); D.nq->Signal(S.nfX, 2 * j); }
		D.nq->ExecuteCommandLists(1, &l1);
		D.nq->Signal(S.nfX, 2 * j + 1);
		D.nq->Signal(D.nlocal, ++D.nlocal_v); D.nal_v[a] = D.nlocal_v;
		++P->w.jobs;
		// 0.4.0 m4b: G's path to the screen was ~2 ms slower than R's when M's copy was submitted only after the CPU saw N's
		// fence (the cross-adapter wait resolves late for a just-signalled value; R's value was signalled long before): G lost
		// 12 %, share 0.39. Now M's copies are queued at once behind their GPU waits (G into the current back buffer, flipped
		// at once; R into the next one, flipped at the target) and R's target is G's measured READY time + D/2 + bias.
		if (jb.has_g)
		{
			++P->w.jobs_g;
			uint64_t vg = 0, vr = 0;
			if (!m_copy(2 * xs, 2 * j, &vg)) return false;
			// m4d: when a frame arrives early (frame-time jitter) its G was flipped right behind the previous R, both landed in
			// one refresh and that R was never shown (R lost 3.5 %). G is now flipped no earlier than R + half a refresh (its
			// copy is already queued; with G call->ready ~3 ms the two images reach the screen about a refresh apart).
			if (P->last_r_call > 0.0)
			{
				const double rf = g_pargs.refresh_ms;
				double now = qpc_ms();
				const int mode = cfg_g_hold_ab > 0 ? static_cast<int>(std::fmod(std::floor(now / (1000.0 * cfg_g_hold_ab)), 2.0)) : cfg_g_hold;
				const double full = P->last_r_call + std::max(0.5 * rf, std::min(rf, P->D > 0.0 ? 0.5 * P->D - 0.5 : rf));
				const double half = P->last_r_call + 0.5 * rf;
				bool held = false;
				// XaGHold=1 (m4r: with half a refresh R was lost for 35-46 % of held G under a loaded CPU - a held G's copy is
				// mostly done by its flip, so it reaches the screen ~3 ms after R, in R's refresh half the time)
				if (mode == 1 && now < full && full - now < 20.0) { held = true; ++P->held_g_full; sleep_until(full); now = qpc_ms(); }
				if (now < half && half - now < 20.0) { held = true; sleep_until(half); now = qpc_ms(); }
				// 0.4.1 m4r_moving10: a G whose copy had ALREADY finished (its frame arrived early and waited behind R) reaches the
				// screen at its flip, not ~3 ms later: flipped at R + half a refresh it landed in R's refresh and R was never shown
				// (46 % of held G, R lost 13 % under a loaded CPU). Such a G waits for a full refresh after R - but never more than
				// D/2 - 0.5 ms, so the pacer still keeps up with the frame rate (D/2 + hold < D).
				if (mode == 0 && now < full && full - now < 20.0 && D.mlocal->GetCompletedValue() >= vg) { held = true; ++P->held_g_full; sleep_until(full); }
				if (held) ++P->held_g;
			}
			double tg_call = 0.0;
			if (!m_flip(T_G, jb.seq_g, j, jb.frame, 0.0, &tg_call)) return false;
			const uint32_t pc_g = P->last_pc;
			if (!m_copy(2 * xs + 1, 2 * j + 1, &vr)) return false;   // R into the next back buffer, flipped at the target
			P->w.wake_g.push_back(static_cast<float>(tg_call - t_wake));
			if (!wait12(D.mlocal, vg, ev_wait, 100)) { X.error = 2; return false; }
			const double t_ready = qpc_ms();
			P->w.g_ready.push_back(static_cast<float>(t_ready - tg_call));
			{
				prec_t &rg = P->rec[pc_g % kRec];
				if (rg.pcount == pc_g && rg.type == T_G) rg.t_ref = t_ready;
			}
			double off = 0.0;
			// m4d: the full difference cost R 3.5 %. Clamped to +-XaOffsetClampMs (default 2 ms as 0.4.0; the first 0.4.1 used 1 ms,
			// and m4r_moving10 sat at -1 ms with AND without dupmon) (without dupmon latG ~= latR: |off| <= 0.25 ms in
			// m4e; the larger values came from dupmon's slower G path, sampled only in bursts)
			if (P->nlatG >= 30 && P->nlatR >= 30) { off = 0.5 * (P->latG - P->latR); off = off < -cfg_off_clamp_ms ? -cfg_off_clamp_ms : (off > cfg_off_clamp_ms ? cfg_off_clamp_ms : off); }
			P->w.offs.push_back(static_cast<float>(off));
			const double half = (P->D > 0.0 ? P->D * 0.5 : 6.5) + static_cast<double>(bias_ms) + off;
			const double target = t_ready + (half < 0.0 ? 0.0 : half);
			sample_stats();
			sleep_until(target);
			sample_stats();
			if (X.stop.load()) return true;
			if (!m_flip(T_R, jb.seq_r, j, jb.frame, target, &P->last_r_call)) return false;
			// 0.4.1 m4g review: attach only now. G's flip was issued D/2 ago and its copy is done, so the first composition that
			// includes the visual shows this job's G or R. Attaching right after G's flip call (0.4.0) let DWM compose the
			// chain's stale front buffer (the last image of the previous engaged period, 2.5-7 s old) while G's cross-adapter
			// copy was still running: 2 of 2 resumes in m4g flashed it.
			attach(j);
		}
		else
		{
			// R-only job (cut / FSR reset): flipped at once; never the first image of an attach (the game's own present shows
			// the same R(k) while detached)
			uint64_t vr = 0;
			if (!m_copy(2 * xs + 1, 2 * j + 1, &vr)) return false;
			if (!m_flip(T_R, jb.seq_r, j, jb.frame, 0.0, &P->last_r_call)) return false;
		}
		D.mq->Signal(S.mfM, j);
		P->pending_release = j;
		P->t_last_job = qpc_ms();
		if (P->w.jobs >= static_cast<uint64_t>(cfg_log_window)) pacer_log_window(qpc_ms());
		return true;
	}

	void release_check()
	{
		const uint64_t pr = P->pending_release;
		if (pr != 0 && pr > X.released.load(std::memory_order_relaxed) && S.nfX->GetCompletedValue() >= 2 * pr + 1)
			X.released.store(pr, std::memory_order_release);
	}

	void pacer_loop(float bias_ms)
	{
		uint64_t last = 0;
		bool waited = true;   // went through a wait since the last job (its fence was not already complete when we looked)
		while (!X.stop.load())
		{
			// 0.4.1: idle detach - no job for max(150 ms, 6 D) while attached (render-thread hitch, a runtime that stopped calling
			// reshade_present, a suspend whose detach request was lost). The game's own present shows the same last R(k); the next
			// job with G attaches again after its R flip.
			if (X.attached.load() && P->t_last_job > 0.0)
			{
				const double idle = qpc_ms() - P->t_last_job, lim = P->D * 6.0 > 150.0 ? P->D * 6.0 : 150.0;
				if (idle > lim) { char b[80]; snprintf(b, sizeof(b), "no new job for %.0f ms", idle); detach(b); ++P->idle_detaches; }
			}
			// detach once the last job of an engaged period is presented
			const uint64_t dr = X.detach_req.load(std::memory_order_acquire);
			if (dr > P->detach_done && last >= dr)
			{
				if (X.pub.load(std::memory_order_acquire) == last) detach("the render thread stopped issuing jobs");   // else re-engaged meanwhile: stay attached
				P->detach_done = dr;
			}
			release_check();
			const uint64_t pub = X.pub.load(std::memory_order_acquire);
			const uint64_t done = S.nfA->GetCompletedValue();
			if (done == UINT64_MAX) { X.error = 1; break; }
			if (pub > last && done > last)
			{
				const uint64_t j = pub < done ? pub : done;
				if (j > last + 1)
				{
					// older jobs were never presented: their slots are free (nothing was copied from them)
					P->w.skipped += j - 1 - last; X.skipped += j - 1 - last;
				}
				if (!process(j, bias_ms, waited)) break;
				last = j;
				waited = false;
				continue;
			}
			if (P->registered != last + 1)
			{
				if (FAILED(S.nfA->SetEventOnCompletion(last + 1, ev_fence))) { X.error = 7; break; }
				P->registered = last + 1;
			}
			// 0.4.1: steady ~2 ms frame-statistics sampling also between jobs (0.4.0 sampled in bursts around the R target; its
			// DXGI view under-counted G). The high-resolution timer bounds the wait; the fence / control events still wake at once.
			// Idle (no job for 500 ms, detached: suspended / menus): no sampling, 100 ms waits.
			const bool idle_long = !X.attached.load() && (P->t_last_job <= 0.0 || qpc_ms() - P->t_last_job > 500.0);
			LARGE_INTEGER due; due.QuadPart = -20000;   // 2 ms
			const bool tset = !idle_long && timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
			HANDLE hs[3] = { ev_fence, ev_ctl, timer };
			WaitForMultipleObjects(tset ? 3 : 2, hs, FALSE, (tset || idle_long) ? 100 : 2);
			waited = true;
			sample_stats();
		}
	}


	// arg: the add-on module, pinned by the creating thread BEFORE CreateThread (0.4.1: no window in which this thread runs
	// unpinned add-on code); released on the way out through FreeLibraryAndExitThread.
	DWORD WINAPI pacer_main(void *arg)
	{
		HMODULE self = static_cast<HMODULE>(arg);
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
		using PFN_STD = HRESULT(WINAPI *)(HANDLE, PCWSTR);
		if (const auto std_ = reinterpret_cast<PFN_STD>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) std_(GetCurrentThread(), L"tf2-fg presenter 2 pacer");
		X.running = true;
		try
		{
			if (cfg_present_log)
			{
				P->csv = _wfsopen((g_dir / "tf2-fg-presents.csv").c_str(), L"ab", _SH_DENYWR);
				if (P->csv) { fputs("# P,seq,type(1 R 2 G),job,frame,t_call_ms,t_ret_ms,t_target_ms,present_count | S,present_count,sync_qpc,sync_refresh\n", P->csv); fflush(P->csv); }
			}
			P->w.reset(qpc_ms(), thread_cpu_100ns());
			logf("xa: pacer thread %lu running (HIGHEST, spin %d us, bias %.2f ms, window %d jobs, present log %d)", GetCurrentThreadId(), cfg_spin_us, g_pargs.bias_ms, cfg_log_window, cfg_present_log);
			pacer_loop(g_pargs.bias_ms);
		}
		catch (...) { X.error = 6; }
		try
		{
			if (X.error.load()) logf("xa: pacer ERROR: %s - presenter 2 off for this session", pacer_err_name(X.error.load()));
			if (P && P->w.jobs > 0) pacer_log_window(qpc_ms());
			detach(X.error.load() ? "pacer error" : "pacer stopped");
			if (P && P->csv) { if (!P->buf.empty()) fwrite(P->buf.data(), 1, P->buf.size(), P->csv); fclose(P->csv); P->csv = nullptr; }
		}
		catch (...) {}
		X.running = false;
		if (self) FreeLibraryAndExitThread(self, 0);
		return 0;
	}

	// ---------------------------------------------------------------- render-thread control
	bool pacer_alive() { return thread != nullptr && !abandoned; }

	bool start_pacer(float bias_ms)
	{
		if (thread || abandoned || stalling || !S.ok || !D.ok) return thread != nullptr;
		if (!ev_fence) ev_fence = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!ev_ctl) ev_ctl = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!ev_wait) ev_wait = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (!ev_fence || !ev_ctl || !ev_wait) { logf("xa: CreateEvent FAILED"); return false; }
		if (P) { P->~pacer_t(); new (P) pacer_t(); }   // reset in place (no ~60 KB temporary on the render thread's stack)
		else P = new (std::nothrow) pacer_t();
		if (!P) { logf("xa: pacer state allocation FAILED"); return false; }
		X.stop = false; X.error = 0;
		g_pargs.bias_ms = bias_ms;
		g_pargs.refresh_ms = topo.refresh_hz > 1.0 ? 1000.0 / topo.refresh_hz : 1000.0 / 60.0;
		HMODULE pin = nullptr;   // pinned here, released by the thread (FreeLibraryAndExitThread)
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&pacer_main), &pin) || !pin) { logf("xa: GetModuleHandleEx (pin) FAILED %lu", GetLastError()); return false; }
		DWORD tid = 0;
		thread = CreateThread(nullptr, 256 * 1024, pacer_main, pin, 0, &tid);
		if (!thread) { FreeLibrary(pin); logf("xa: CreateThread FAILED %lu", GetLastError()); return false; }
		busy_run = 0; t_last_pub = 0.0;
		return true;
	}

	// ---------------------------------------------------------------- builder thread (0.4.0 m4d, 0.4.1)
	// Devices (0.25-1.5 s) and the D3D12 / DXGI-composition / DComp part of the surfaces (0.06-0.2 s) are built on a worker
	// thread; the render thread creates the game-device part first (create_surfaces_d3d11, ~1 ms) and hands the builder only NT
	// handles and LUIDs. While the builder runs the render thread never touches D / S (except the D3D11 slots it created, which
	// the builder does not use), never waits for it inside a ReShade event (a teardown then is DEFERRED: build_cancel +
	// pend_level, run by poll_builder() once the builder has exited), and only destroy_device joins it with a bound.
	HANDLE builder = nullptr;
	std::atomic<int> build_state { 0 };   // 0 idle, 1 running, 2 finished ok, 3 finished with a failure, 4 cancelled
	std::atomic<bool> build_cancel { false };
	int pend_level = 0;                    // deferred teardown level (render thread)
	const char *pend_why = "";
	uint64_t builds_started = 0;
	uint64_t build_start_frame = 0;
	bool test_teardown_done = false;
	struct build_args
	{
		uint32_t w = 0, h = 0;
		HWND hwnd = nullptr; uint64_t sc_gen = 0, frame = 0;
		LUID lr = {}, ld = {};
		slot_handles hs;
		double t0 = 0.0, ms = 0.0;
	} ba;

	DWORD WINAPI builder_main(void *arg)
	{
		HMODULE self = static_cast<HMODULE>(arg);   // pinned by the creating thread
		using PFN_STD = HRESULT(WINAPI *)(HANDLE, PCWSTR);
		if (const auto std_ = reinterpret_cast<PFN_STD>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))) std_(GetCurrentThread(), L"tf2-fg presenter 2 builder");
		int st = 3;
		try
		{
			bool ok = D.ok || create_devices(ba.lr, ba.ld);
			if (ok && build_cancel.load()) st = 4;
			else if (ok) { ok = create_surfaces_x(ba.w, ba.h, ba.hwnd, ba.sc_gen, ba.frame, ba.hs, &build_cancel); st = ok ? 2 : (build_cancel.load() ? 4 : 3); }
		}
		catch (...) { st = 3; logf("xa: exception in the builder thread"); }
		close_handles(ba.hs);   // whatever was not opened (failure / cancel paths)
		ba.ms = qpc_ms() - ba.t0;
		build_state.store(st, std::memory_order_release);
		if (self) FreeLibraryAndExitThread(self, 0);
		return 0;
	}

	// Joins a finished (or, with ms > 0, a running) builder. false = still running after `ms` (with ms >= 5000: abandoned).
	bool builder_join(DWORD ms)
	{
		if (!builder) return true;
		const DWORD r = WaitForSingleObject(builder, ms);
		if (r != WAIT_OBJECT_0)
		{
			if (ms >= 5000)
			{
				abandoned = true;
				disabled_why = "the builder thread did not finish (abandoned; its objects are never released)";
				logf("xa: builder thread did NOT finish within %lu ms - abandoned: presenter 2 off for this session, its objects are leaked on purpose", ms);
			}
			return false;
		}
		CloseHandle(builder); builder = nullptr;
		return true;
	}

	// Render thread. The game-device part is created here (it needs no D3D12 device); the rest on the builder.
	bool start_builder(ID3D11Device *dev, ID3D11DeviceContext *imm, uint32_t w, uint32_t h, DXGI_FORMAT tl, DXGI_FORMAT srgb, DXGI_FORMAT unorm, HWND hwnd, uint64_t sc_gen, uint64_t frame)
	{
		if (builder || abandoned || stalling || !dev || !imm) return false;
		ba = build_args {};
		if (!create_surfaces_d3d11(dev, imm, w, h, tl, srgb, unorm, frame, ba.hs)) return false;
		ba.w = w; ba.h = h; ba.hwnd = hwnd; ba.sc_gen = sc_gen; ba.frame = frame; ba.lr = topo.render; ba.ld = topo.display;
		ba.t0 = qpc_ms();
		build_cancel = false;
		build_state.store(1, std::memory_order_release);
		HMODULE pin = nullptr;
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&builder_main), &pin) || !pin)
		{ build_state = 0; close_handles(ba.hs); release_surfaces(); logf("xa: GetModuleHandleEx (pin) FAILED %lu", GetLastError()); return false; }
		DWORD tid = 0;
		builder = CreateThread(nullptr, 256 * 1024, builder_main, pin, 0, &tid);
		if (!builder) { FreeLibrary(pin); build_state = 0; close_handles(ba.hs); release_surfaces(); logf("xa: CreateThread (builder) FAILED %lu", GetLastError()); return false; }
		++builds_started; build_start_frame = frame;
		if (builds_started <= 20) logf("xa: builder thread #%llu started (%ux%u; game-device part created on the render thread in %.1f ms)", static_cast<unsigned long long>(builds_started), w, h, S.create_d3d11_ms);
		return true;
	}

	// Stops and joins the pacer (it detaches the visual on its way out). false = it did not exit (abandoned).
	bool stop_pacer(const char *why)
	{
		if (!thread) return true;
		X.stop = true;
		if (ev_ctl) SetEvent(ev_ctl);
		const double t0 = qpc_ms();
		const DWORD r = WaitForSingleObject(thread, 2000);
		if (r != WAIT_OBJECT_0)
		{
			abandoned = true;
			disabled_why = "the pacer thread did not stop (abandoned; its objects are never released)";
			logf("xa: pacer did NOT exit within 2 s (%s) - abandoned: presenter 2 off for this session, its objects are leaked on purpose", why);
			emergency_detach("abandoned pacer");   // 0.4.1: the game's own frames must become visible again
			return false;
		}
		CloseHandle(thread); thread = nullptr;
		logf("xa: pacer stopped (%s) in %.1f ms: presented %llu, skipped %llu, attaches %llu, detaches %llu%s", why, qpc_ms() - t0, X.presented.load(), X.skipped.load(),
			X.attaches.load(), X.detaches.load(), X.error.load() ? " - after an ERROR" : "");
		return true;
	}

	// GPU idle on N and M (after the pacer stopped). false = timeout (resources are then leaked, never released).
	// 0.4.1: a REMOVED device counts as idle (nothing runs on it any more; releasing its objects is legal), so a TDR does not
	// strand the game device's slot objects; the event is leaked (never closed) when a wait timed out.
	bool queues_idle()
	{
		if (!D.ok) return true;
		HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!ev) return false;
		auto idle = [&](ID3D12Device *dev, ID3D12CommandQueue *q, ID3D12Fence *f, uint64_t &v, const char *name) -> bool {
			if (!q || !f) return true;
			const HRESULT rr = dev ? dev->GetDeviceRemovedReason() : S_OK;
			if (rr != S_OK) { logf("xa: %s device removed (0x%08x) - its queue counts as idle", name, static_cast<unsigned>(rr)); return true; }
			q->Signal(f, ++v);
			return wait12(f, v, ev, 1000);
		};
		const bool okN = idle(D.N, D.nq, D.nlocal, D.nlocal_v, "N");
		const bool okM = idle(D.M, D.mq, D.mlocal, D.mlocal_v, "M");
		if (okN && okM) CloseHandle(ev);   // else a SetEventOnCompletion may still be registered on it: leaked
		return okN && okM;
	}

	// Render thread: the result of a finished builder (joined here).
	void take_build()
	{
		const int st = build_state.exchange(0);
		logf("xa: builder thread finished in %.1f ms (%s) - the render thread did not wait for it", ba.ms,
			st == 2 ? "devices + surfaces ready" : (st == 4 ? "CANCELLED by a teardown" : "FAILED, see above"));
		if (st == 2) reset_jobs();
		else release_surfaces();   // the game-device slots of a failed / cancelled build (its D3D12 part is already gone)
	}

	// level 1 = surfaces (+ pacer), 2 = + devices. Render thread (or device/swap-chain destruction).
	// can_wait = false (every caller inside a ReShade present / swap-chain event): a running builder is cancelled and the
	// teardown DEFERRED to poll_builder(); never waited for (0.4.0: 5 s deadlock with ReShade's D3D11 lock).
	void teardown(int level, const char *why, bool can_wait = false)
	{
		if (abandoned) return;
		if (stalling && !can_wait && thread && WaitForSingleObject(thread, 0) != WAIT_OBJECT_0)
		{
			// the stalled pacer has not exited yet (its visual is already detached): poll_stall() tears down once it has
			if (level > pend_level) { pend_level = level; pend_why = why; }
			return;
		}
		if (builder)
		{
			build_cancel = true;
			if (!can_wait)
			{
				const bool first = pend_level == 0;
				if (level > pend_level) { pend_level = level; pend_why = why; }
				if (first) logf("xa: teardown (%s) DEFERRED: the builder thread is running - cancelled, joined and released on a later frame (never waited for inside a ReShade event)", why);
				return;
			}
			const double t0 = qpc_ms();
			if (!builder_join(5000)) return;
			logf("xa: teardown (%s): waited %.1f ms for the builder thread (outside any present event)", why, qpc_ms() - t0);
			build_state = 0;
			if (level < pend_level) level = pend_level;
			pend_level = 0;
		}
		build_state = 0;
		const bool had = thread != nullptr || S.ok || S.R[0] != nullptr || (level >= 2 && D.ok);
		if (!stop_pacer(why)) return;
		stalling = false;
		if (!queues_idle())
		{
			abandoned = true; disabled_why = "GPU queues did not go idle at teardown";
			logf("xa: teardown (%s): N/M queues not idle within 1 s - resources leaked on purpose, presenter 2 off for this session", why);
			emergency_detach("queues not idle");
			return;
		}
		release_surfaces();
		if (level >= 2) release_devices();
		engaged = false;
		if (had) logf("xa: teardown (%s): %s released", why, level >= 2 ? "surfaces + devices" : "surfaces");
	}

	// Render thread, every frame (on_present): joins a finished builder, takes its result, runs a deferred teardown.
	void poll_builder()
	{
		if (!builder) { if (pend_level && !stalling) { const int l = pend_level; pend_level = 0; teardown(l, pend_why, true); } return; }
		if (build_state.load(std::memory_order_acquire) == 1 || !builder_join(0)) return;
		take_build();
		if (pend_level)
		{
			const int l = pend_level; pend_level = 0;
			teardown(l, pend_why, true);   // the builder has exited: no wait
		}
	}

	// Render thread: the pacer stopped consuming jobs (0.4.1 watchdog). Show the game's own frames at once, ask the pacer to
	// stop, never wait for it here; poll_stall() finishes the teardown once it has exited.
	void stall(const char *why)
	{
		if (stalling || !thread || abandoned) return;
		stalling = true; stall_t0 = qpc_ms(); ++stall_count; ++stalls_total;
		logf("xa: PACER STALLED (%s) - emergency detach, stop requested; the render thread does not wait for it (stall #%d this session)", why, stall_count);
		emergency_detach("pacer stalled");
		X.stop = true;
		if (ev_ctl) SetEvent(ev_ctl);
	}
	void poll_stall()
	{
		if (!stalling) return;
		if (thread && WaitForSingleObject(thread, 0) != WAIT_OBJECT_0)
		{
			if (qpc_ms() - stall_t0 > 10000.0)
			{
				abandoned = true; stalling = false;
				disabled_why = "the pacer stalled and did not exit within 10 s (abandoned; its objects are never released)";
				logf("xa: stalled pacer did NOT exit within 10 s - abandoned: presenter 2 off for this session, its objects are leaked on purpose");
				emergency_detach("abandoned stalled pacer");
			}
			return;
		}
		logf("xa: stalled pacer exited %.0f ms after the stop request", qpc_ms() - stall_t0);
		stalling = false;
		const int l = pend_level > 1 ? pend_level : 1;
		pend_level = 0;
		teardown(l, "pacer stalled", true);   // exited: no wait; the surfaces are rebuilt in WARMUP / SUSPENDED
		if (stall_count >= 3 && !disabled_why && !abandoned)
		{
			disabled_why = "the pacer stalled 3 times";
			teardown(2, "pacer stalled 3 times", true);
			logf("presenter 2 OFF for this session: the pacer stalled 3 times");
		}
	}

	// Publishes job `id` (slot = id % 3) and signals fence A with it. Render thread, after the slot writes were recorded.
	void publish(uint64_t id, uint64_t frame, bool has_g, uint32_t seq_g, uint32_t seq_r)
	{
		job_t &jb = X.jobs[id % kJobs];
		jb.id = id; jb.frame = frame; jb.slot = static_cast<int>(id % kSlots); jb.has_g = has_g; jb.seq_g = seq_g; jb.seq_r = seq_r; jb.t_pub = qpc_ms();
		X.pub.store(id, std::memory_order_release);
		S.ctx4->Signal(S.fA, id);
	}
}
