// TF2NR.GraphicsToggles (AI Enhancements) - the "AI Enhancements" page (resource/ui/menus/tf2nr_ai.menu), its single entry
// row on the Video page (resource/ui/menus/video.menu = vanilla + BtnAIEnhancements), the "DLSS 5 Adjustments" submenu
// (resource/ui/menus/tf2nr_dlss5.menu), plus a Mod Settings mirror.
// ConVar-bound rows (DialogListButton "ConVar" / SliderControl "conCommand", the same mechanism as vanilla audio.menu) are
// written by the engine; the Upscaler / Quality rows are script lists (Northstar custom_match_settings style) whose
// handlers write the tf2nr_* convars. Native code (the ReShade add-ons) only READS these convars. Changes apply at once
// (no "Apply"). Vanilla menu_video_settings.nut is NOT overridden.
// 2026-09-28: no typed tables here - 'table< var, string >' is a compile error ("Type var cannot be a key"),
// and a UI compile error destroys the whole UI VM. Parallel arrays + linear search instead.
// 2026-09-28 (v0.2): DLSS SR is live (tf2-dlss-sr.addon64 replaces TF2's TSAA resolve). SR needs TSAA and a pinned
// Adaptive Resolution scale; the add-on applies those itself (live values only, videoconfig untouched). Script
// cannot: mat_antialias_mode / dvs_* are not visible to script ("ConVar mat_antialias_mode is not valid").
// 2026-09-28 (v0.3): DLSS 5 Adjustments submenu. The add-on pushes tf2nr_nr_* into [RenoDX.DLSS5].
// 2026-09-29 (v0.5): tf2-fg 0.3 = AMD FSR 3 frame interpolation, convar tf2nr_fsr_fg.
// 2026-09-29 (v1.0.0, owner decisions): the Video page is vanilla again plus ONE row, "AI Enhancements", which opens the
// AI Enhancements page: Upscaler (Off / DLSS / FSR 3), Quality, DLSS 5, DLSS 5 Adjustments, Frame Generation.
//   Settings interface (fixed by the lead):
//   tf2nr_dlss_sr      0 Off, 1 Native AA (DLAA / FSR 3 Native AA), 2 Quality, 3 Balanced, 4 Performance, 5 Ultra Performance
//   tf2nr_upscaler     1 DLSS, 2 FSR 3 (anything else = DLSS); sr_next falls back to FSR 3 by itself if NGX/DLSS fails
//   tf2nr_upscale_last the last non-zero tf2nr_dlss_sr, stored when the Upscaler row goes Off, restored when it goes
//                      back to DLSS or FSR 3. While the upscaler is Off the Quality row edits this value.
//   tf2nr_dlss5_nr     DLSS 5 NR runs inside the DLSS evaluate: DLSS upscaler only (inactive with FSR 3); NR on +
//                      upscaler Off -> sr_next still runs DLSS at Native AA (DLAA)
//   tf2nr_fsr_fg       FG needs an upscaler running (depth/MVs come from sr_next); works with both backends
//   Rows are never greyed out: the help text says what each one needs.
// 2026-09-29 (1.0.0 review fixes): tf2nr_dlss5_nr defaults to 0 (mod.json) - with the package's DX11Source=native a
// default of 1 made tf2-dlss-sr run DLSS/FSR at Native AA on a fresh install while this page said "Upscaler: Off".
// Mod Settings no longer has raw Upscaler / Quality rows (they bypassed the tf2nr_upscale_last memory and tf2nr_upscaler
// 1/2 does not fit an index-valued enum); its button opens this page instead. Help text: Off + DLSS 5, FG fps limits.
// 2026-09-29 22:00 (1.0.0 second review): the tf2-dlss-sr that ships (0.3.2) has NO FSR 3 upscaler and ignores
// tf2nr_upscaler (ai:sr 21:38). TF2NR_FSR3_UPSCALER = false hides the "FSR 3" choice, shows a stored tf2nr_upscaler 2
// as DLSS (and writes 1 back), and keeps FSR 3 out of the help text. FG help: tf2-fg 0.4.1 only runs where the screen
// is driven by another GPU than the renderer (hybrid laptops) and not while Steam/Discord/RTSS/OBS overlays are hooked.
// "Upscaler Off + DLSS 5 On = DLAA" only holds with the DLSS 5 runtime (renodx-dlss5) installed.

global function TF2NR_AddModSettings
global function TF2NR_VideoMenu_Init
global function TF2NR_LogState

const string TF2NR_CVAR_NR = "tf2nr_dlss5_nr"
const string TF2NR_CVAR_SR = "tf2nr_dlss_sr"
const string TF2NR_CVAR_UPSCALER = "tf2nr_upscaler"
const string TF2NR_CVAR_LAST = "tf2nr_upscale_last"
const string TF2NR_CVAR_FG = "tf2nr_fsr_fg"
const string TF2NR_MENU_DLSS5 = "TF2NR_DLSS5Menu"
const string TF2NR_MENU_AI = "TF2NR_AIMenu"

// Set by the lead after the in-game push test: true = renodx follows set_config_value live (or the version-pinned
// writer is active); false = values are saved to ReShade.ini and renodx reads them at the next game start.
const bool TF2NR_ADJUST_LIVE = true

// true only when the tf2-dlss-sr build that ships WITH this mod has the FSR 3 upscaler backend (reads tf2nr_upscaler 2).
// 1.0.0 ships tf2-dlss-sr 0.3.2 (DLSS only): false = Upscaler list Off / DLSS, no FSR 3 in the help text.
const bool TF2NR_FSR3_UPSCALER = false

const string TF2NR_NOTE_LIVE = "\n\nApplies within a second. Model settings make Neural Rendering restart its history, so the image settles again briefly."
const string TF2NR_NOTE_RESTART = "\n\nSaved now; renodx applies it the next time the game starts."

// help text (the Video page description panel is 290 tall = about 9 lines; the AI page uses the scheme's 562)
// *_FSR3 variants are used only while TF2NR_FSR3_UPSCALER is true
const string TF2NR_DESC_ENTRY = "AI upscaling (NVIDIA DLSS), NVIDIA DLSS 5 Neural Rendering and AMD FSR 3 Frame Generation.\n\nEvery setting there applies at once; no Apply needed."
const string TF2NR_DESC_ENTRY_FSR3 = "AI upscaling (NVIDIA DLSS or AMD FSR 3), NVIDIA DLSS 5 Neural Rendering and AMD FSR 3 Frame Generation.\n\nEvery setting there applies at once; no Apply needed."
const string TF2NR_DESC_UPSCALER = "Renders fewer pixels and rebuilds a sharp full-resolution image, in place of TSAA anti-aliasing. While it is on, Adaptive Resolution is held at the upscaler's scale.\n\nDLSS: NVIDIA AI upscaling. Needs an NVIDIA RTX GPU; without one the game keeps its own anti-aliasing.\nOff: the game's own anti-aliasing (TSAA). While DLSS 5 is On and the DLSS 5 runtime is installed, DLSS keeps running at DLAA instead."
const string TF2NR_DESC_UPSCALER_FSR3 = "Renders fewer pixels and rebuilds a sharp full-resolution image, in place of TSAA anti-aliasing. While it is on, Adaptive Resolution is held at the upscaler's scale.\n\nDLSS: NVIDIA AI upscaling. Needs an NVIDIA RTX GPU; if DLSS cannot start, FSR 3 is used instead.\nFSR 3: AMD FidelityFX Super Resolution 3. Works on any GPU.\nOff: the game's own anti-aliasing (TSAA). While DLSS 5 is On and the DLSS 5 runtime is installed, DLSS keeps running at DLAA instead."
const string TF2NR_DESC_QUALITY_DLSS = "How many pixels the game renders before DLSS rebuilds the full image.\n\nDLAA: native resolution, best anti-aliasing, no speed-up.\nQuality / Balanced / Performance / Ultra Performance render at about 67, 58, 50 and 33 percent of the screen resolution for more frames per second.\n\nWhile the Upscaler is Off, this picks the quality used when you turn it back on."
const string TF2NR_DESC_QUALITY_FSR = "How many pixels the game renders before FSR 3 rebuilds the full image.\n\nNative AA: native resolution, FSR 3 anti-aliasing only, no speed-up.\nQuality / Balanced / Performance / Ultra Performance render at about 67, 58, 50 and 33 percent of the screen resolution for more frames per second.\n\nWhile the Upscaler is Off, this picks the quality used when you turn it back on."
const string TF2NR_DESC_NR = "NVIDIA DLSS 5 Neural Rendering (RenoDX, running inside DLSS on the HDR frame).\n\nOff returns the frame rate to normal. Takes effect within a second."
const string TF2NR_DESC_NR_AI = "NVIDIA DLSS 5 Neural Rendering: an AI model relights and re-details the image inside DLSS.\n\nNeeds an RTX 50 series GPU, the DLSS 5 runtime and Upscaler = DLSS. With the Upscaler Off and the DLSS 5 runtime installed, DLSS still runs at DLAA while this is on.\n\nCosts frame rate. Takes effect within a second."
const string TF2NR_DESC_NR_AI_FSR3 = "NVIDIA DLSS 5 Neural Rendering: an AI model relights and re-details the image inside DLSS.\n\nNeeds an RTX 50 series GPU, the DLSS 5 runtime and Upscaler = DLSS. With FSR 3 it stays inactive. With the Upscaler Off and the DLSS 5 runtime installed, DLSS still runs at DLAA while this is on.\n\nCosts frame rate. Takes effect within a second."
// tf2-fg 0.4.1 (Presenter=auto): runs only when the display belongs to another adapter than the renderer (hybrid
// laptops); pass-through when the display is on the render adapter, and while listed overlay hooks are loaded
const string TF2NR_DESC_FG = "AMD FSR 3 Frame Generation: a generated frame between every two rendered frames, for smoother motion.\n\nNeeds Upscaler = DLSS (any quality) and V-Sync off. Works only when the screen is driven by another GPU than the one rendering the game (laptops with hybrid graphics). It stays off on desktops, when the screen runs on the rendering GPU, and while the Steam, Discord, RTSS or OBS overlay is hooked into the game.\n\nWhere it runs, it pauses by itself in menus and while the real frame rate is under 40 fps or over half the screen's refresh rate (82.5 fps at 165 Hz; at most 83). Adds a little input lag."
const string TF2NR_DESC_FG_FSR3 = "AMD FSR 3 Frame Generation: a generated frame between every two rendered frames, for smoother motion.\n\nNeeds an upscaler on (DLSS or FSR 3) and V-Sync off. Works only when the screen is driven by another GPU than the one rendering the game (laptops with hybrid graphics). It stays off on desktops, when the screen runs on the rendering GPU, and while the Steam, Discord, RTSS or OBS overlay is hooked into the game.\n\nWhere it runs, it pauses by itself in menus and while the real frame rate is under 40 fps or over half the screen's refresh rate (82.5 fps at 165 Hz; at most 83). Adds a little input lag."
const string TF2NR_DESC_TUNING = "Sliders for DLSS 5 Neural Rendering: strength, tone, structure, skin detail, style, colour and the Look / stabilizer controls.\n\nThey only change the image while DLSS 5 Neural Rendering is On."
const string TF2NR_DESC_RESET = "Puts every DLSS 5 adjustment back to its default. The On/Off switch above is not changed."

struct
{
	var menu = null             // VideoMenu
	var buttonEntry = null      // Video page row "AI Enhancements"

	var aiMenu = null
	var upscalerButton = null
	var qualityDlssButton = null
	var qualityFsrButton = null
	var nrButton = null
	var tuningButton = null
	var fgButton = null
	bool aiReady = false
	bool aiRefreshing = false   // true while the script sets list selections (UIE_CHANGE may fire)

	var dlss5Menu = null
	// one entry per row of the DLSS 5 Adjustments menu (parallel arrays)
	array<var> rowButtons       // the focusable button (a slider's BtnDropButton, or the row itself)
	array<var> rowSliders       // the SliderControl, or null for list/button rows
	array<string> rowLabels
	array<string> rowDescs
	array<string> rowCvars      // "" for the reset button
	array<string> rowFormats    // format() pattern for the value shown after the label; "" = no value
	array<float> rowMin
	array<float> rowMax
} file

// UICallback "Before": runs before InitMenus(), which is where Mod Settings expects registrations and where
// AddMenu must happen (Northstar.Client registers ModSettings / ModListMenu the same way).
// Keep every ModSettings_* call directly in THIS function: ModSettings checks getstackinfos( 2 )["func"], i.e. the
// title, the categories and the settings must be added from the same calling function.
void function TF2NR_AddModSettings()
{
	AddMenu( TF2NR_MENU_DLSS5, $"resource/ui/menus/tf2nr_dlss5.menu", TF2NR_InitDLSS5Menu )
	AddMenu( TF2NR_MENU_AI, $"resource/ui/menus/tf2nr_ai.menu", TF2NR_InitAIMenu )

	ModSettings_AddModTitle( "AI Enhancements" )
	ModSettings_AddModCategory( "Upscaling and frame generation" )
	// Upscaler and Quality live only on the AI Enhancements page: its Off/restore logic keeps tf2nr_upscale_last in step
	// (a raw Mod Settings row bypassed it), and tf2nr_upscaler 1/2 does not fit Mod Settings' index-valued enums.
	ModSettings_AddButton( "Upscaler and Quality: open the AI Enhancements menu", TF2NR_OpenAIMenu )
	ModSettings_AddEnumSetting( TF2NR_CVAR_NR, "DLSS 5 Neural Rendering", [ "#SETTING_OFF", "#SETTING_ON" ] )
	ModSettings_AddEnumSetting( TF2NR_CVAR_FG, "Frame Generation (FSR 3)", [ "#SETTING_OFF", "#SETTING_ON" ] )

	// Note: ModSettings ignores forceClamp and its text box accepts any number; the add-on clamps every value again.
	ModSettings_AddModCategory( "DLSS 5 adjustments" )
	ModSettings_AddSliderSetting( "tf2nr_nr_intensity", "Strength", 0.0, 2.0, 0.1 )
	ModSettings_AddSliderSetting( "tf2nr_nr_local_tone", "Local tone", 0.0, 2.0, 0.1 )
	ModSettings_AddSliderSetting( "tf2nr_nr_structure", "Structure", 0.0, 2.0, 0.1 )
	ModSettings_AddSliderSetting( "tf2nr_nr_skin", "Character / skin detail", 0.0, 1.0, 0.05 )
	ModSettings_AddEnumSetting( "tf2nr_nr_style", "Style", [ "Default", "Natural", "Cinematic" ] )
	ModSettings_AddSliderSetting( "tf2nr_nr_look_strength", "Look strength", 0.0, 2.0, 0.1 )
	ModSettings_AddSliderSetting( "tf2nr_nr_look_colour", "Colour change", 0.0, 2.0, 0.1 )
	ModSettings_AddSliderSetting( "tf2nr_nr_look_hue", "Tint", 0.0, 2.0, 0.1 )
	ModSettings_AddSliderSetting( "tf2nr_nr_look_darken", "Darkening", 0.0, 2.0, 0.1 )
	ModSettings_AddSliderSetting( "tf2nr_nr_look_detail", "Fine detail", 0.0, 2.0, 0.1 )
	ModSettings_AddEnumSetting( "tf2nr_nr_stabilize", "Stabilize", [ "#SETTING_OFF", "Static", "Motion" ] )
	ModSettings_AddEnumSetting( "tf2nr_nr_menu_tuning", "Adjustments drive ReShade", [ "#SETTING_OFF", "#SETTING_ON" ] )
}

// Mod Settings button (called from its UIE_CLICK handler, i.e. in UI frame context)
void function TF2NR_OpenAIMenu()
{
	AdvanceMenu( GetMenu( TF2NR_MENU_AI ) )
}

// UICallback "After": runs after UICodeCallback_UIInit -> InitMenus(), so VideoMenu and its children exist.
void function TF2NR_VideoMenu_Init()
{
	var menu = GetMenu( "VideoMenu" )
	file.menu = menu

	// If another mod with a higher LoadPriority replaced video.menu, our row is missing: stay silent instead of
	// throwing (a UI script error here would break the whole UI VM). The page is still reachable from Mod Settings.
	if ( !Hud_HasChild( menu, "BtnAIEnhancements" ) )
	{
		printt( "[TF2NR] video.menu override not active - AI Enhancements is only reachable from Mod Settings" )
		return
	}

	file.buttonEntry = Hud_GetChild( menu, "BtnAIEnhancements" )
	SetButtonRuiText( file.buttonEntry, "AI Enhancements" )
	AddButtonEventHandler( file.buttonEntry, UIE_GET_FOCUS, TF2NR_Entry_Focused )
	// same wiring as vanilla BtnAdvancedHud -> AdvancedHudMenu
	Hud_AddEventHandler( file.buttonEntry, UIE_CLICK, AdvanceMenuEventHandler( GetMenu( TF2NR_MENU_AI ) ) )
	printt( "[TF2NR] AI Enhancements row added to the Video menu" )
}

void function TF2NR_Entry_Focused( var button )
{
	SetElementsTextByClassname( file.menu, "MenuItemDescriptionClass", TF2NR_FSR3_UPSCALER ? TF2NR_DESC_ENTRY_FSR3 : TF2NR_DESC_ENTRY )
}

// ---- AI Enhancements page ---------------------------------------------------------------------------------------

// AddMenu initFunc: called from InitMenus() for every registered menu.
void function TF2NR_InitAIMenu()
{
	var menu = GetMenu( TF2NR_MENU_AI )
	file.aiMenu = menu

	AddMenuEventHandler( menu, eUIEvent.MENU_OPEN, TF2NR_AIMenu_Open )
	AddMenuEventHandler( menu, eUIEvent.MENU_CLOSE, TF2NR_AIMenu_Close )
	AddEventHandlerToButtonClass( menu, "RuiFooterButtonClass", UIE_GET_FOCUS, TF2NR_AIFooter_Focused )
	AddMenuFooterOption( menu, BUTTON_A, "#A_BUTTON_SELECT" )
	AddMenuFooterOption( menu, BUTTON_B, "#B_BUTTON_BACK", "#BACK" )

	array<string> need = [ "SwchUpscaler", "SwchQualityDLSS", "SwchQualityFSR", "SwchDLSS5NR", "BtnDLSS5Tuning", "SwchFSRFG" ]
	foreach ( string elem in need )
	{
		if ( !Hud_HasChild( menu, elem ) )
		{
			printt( "[TF2NR] tf2nr_ai.menu has no " + elem + " - AI Enhancements page not wired" )
			return
		}
	}

	file.upscalerButton = Hud_GetChild( menu, "SwchUpscaler" )
	file.qualityDlssButton = Hud_GetChild( menu, "SwchQualityDLSS" )
	file.qualityFsrButton = Hud_GetChild( menu, "SwchQualityFSR" )
	file.nrButton = Hud_GetChild( menu, "SwchDLSS5NR" )
	file.tuningButton = Hud_GetChild( menu, "BtnDLSS5Tuning" )
	file.fgButton = Hud_GetChild( menu, "SwchFSRFG" )

	SetButtonRuiText( file.upscalerButton, "Upscaler" )
	SetButtonRuiText( file.qualityDlssButton, "Quality" )
	SetButtonRuiText( file.qualityFsrButton, "Quality" )
	SetButtonRuiText( file.nrButton, "DLSS 5" )
	SetButtonRuiText( file.tuningButton, "DLSS 5 Adjustments" )
	SetButtonRuiText( file.fgButton, "Frame Generation" )

	// script lists: added once (a dialog list cannot be cleared); value strings are the convar values
	Hud_DialogList_AddListItem( file.upscalerButton, "#SETTING_OFF", "0" )
	Hud_DialogList_AddListItem( file.upscalerButton, "DLSS", "1" )
	if ( TF2NR_FSR3_UPSCALER )
		Hud_DialogList_AddListItem( file.upscalerButton, "FSR 3", "2" )
	array<string> qualityNames = [ "Quality", "Balanced", "Performance", "Ultra Performance" ]
	Hud_DialogList_AddListItem( file.qualityDlssButton, "DLAA", "1" )
	Hud_DialogList_AddListItem( file.qualityFsrButton, "Native AA", "1" )
	for ( int i = 0; i < qualityNames.len(); i++ )
	{
		Hud_DialogList_AddListItem( file.qualityDlssButton, qualityNames[ i ], string( i + 2 ) )
		Hud_DialogList_AddListItem( file.qualityFsrButton, qualityNames[ i ], string( i + 2 ) )
	}

	AddButtonEventHandler( file.upscalerButton, UIE_CHANGE, TF2NR_AI_Upscaler_Changed )
	AddButtonEventHandler( file.qualityDlssButton, UIE_CHANGE, TF2NR_AI_Quality_Changed )
	AddButtonEventHandler( file.qualityFsrButton, UIE_CHANGE, TF2NR_AI_Quality_Changed )
	AddButtonEventHandler( file.nrButton, UIE_CHANGE, TF2NR_AI_ConVarRow_Changed )
	AddButtonEventHandler( file.fgButton, UIE_CHANGE, TF2NR_AI_ConVarRow_Changed )
	Hud_AddEventHandler( file.tuningButton, UIE_CLICK, TF2NR_AI_OpenDLSS5Menu )

	array<var> rows = [ file.upscalerButton, file.qualityDlssButton, file.qualityFsrButton, file.nrButton, file.tuningButton, file.fgButton ]
	foreach ( var row in rows )
		AddButtonEventHandler( row, UIE_GET_FOCUS, TF2NR_AI_Focused )

	file.aiReady = true
}

void function TF2NR_AIMenu_Open()
{
	UI_SetPresentationType( ePresentationType.NO_MODELS )
	if ( !file.aiReady )
		return
	TF2NR_AI_Refresh()
	TF2NR_AI_Log( "opened" )
}

void function TF2NR_AIMenu_Close()
{
	SavePlayerSettings()   // as vanilla audio/video menus do on close: persists the ARCHIVE_PLAYERPROFILE convars
}

void function TF2NR_AI_OpenDLSS5Menu( var button )
{
	AdvanceMenu( GetMenu( TF2NR_MENU_DLSS5 ) )
}

int function TF2NR_LastQuality()
{
	int q = GetConVarInt( TF2NR_CVAR_LAST )
	if ( q < 1 || q > 5 )
		q = 2
	return q
}

void function TF2NR_AI_Log( string what )
{
	printt( "[TF2NR] AI menu: " + what + " | tf2nr_upscaler " + GetConVarInt( TF2NR_CVAR_UPSCALER ) + ", tf2nr_dlss_sr " + GetConVarInt( TF2NR_CVAR_SR ) +
		", tf2nr_upscale_last " + GetConVarInt( TF2NR_CVAR_LAST ) + ", tf2nr_dlss5_nr " + GetConVarInt( TF2NR_CVAR_NR ) + ", tf2nr_fsr_fg " + GetConVarInt( TF2NR_CVAR_FG ) )
}

// Diagnostics (console): 'script_ui TF2NR_LogState( false )' prints the AI Enhancements convars to the log.
// pad = true also writes ~65 KB of filler lines: Northstar buffers its log file and a force-stopped game (the probe
// harness) loses the buffered tail, so a test run ends with this call to get its menu lines onto disk.
void function TF2NR_LogState( bool pad )
{
	TF2NR_AI_Log( "state" )
	if ( !pad )
		return
	string line = "[TF2NR] log flush padding ........................................................................................"
	for ( int i = 0; i < 500; i++ )
		printt( line )
}

// convars -> list selections + which Quality row (DLSS or FSR 3 names) is shown
void function TF2NR_AI_Refresh()
{
	if ( !file.aiReady )
		return
	file.aiRefreshing = true

	int sr = GetConVarInt( TF2NR_CVAR_SR )
	bool fsr = GetConVarInt( TF2NR_CVAR_UPSCALER ) == 2
	if ( fsr && !TF2NR_FSR3_UPSCALER )
	{
		// no FSR 3 in the shipped tf2-dlss-sr (it runs DLSS for any tf2nr_upscaler): show and store DLSS
		fsr = false
		SetConVarInt( TF2NR_CVAR_UPSCALER, 1 )
		printt( "[TF2NR] AI menu: tf2nr_upscaler 2 (FSR 3) is not available in this release - set to 1 (DLSS)" )
	}
	string sel = fsr ? "2" : "1"
	if ( sr == 0 )
		sel = "0"
	Hud_SetDialogListSelectionValue( file.upscalerButton, sel )

	int q = TF2NR_LastQuality()
	if ( sr >= 1 && sr <= 5 )
		q = sr
	Hud_SetDialogListSelectionValue( file.qualityDlssButton, string( q ) )
	Hud_SetDialogListSelectionValue( file.qualityFsrButton, string( q ) )

	var shown = fsr ? file.qualityFsrButton : file.qualityDlssButton
	var hidden = fsr ? file.qualityDlssButton : file.qualityFsrButton
	Hud_SetVisible( hidden, false )
	Hud_SetEnabled( hidden, false )
	Hud_SetVisible( shown, true )
	Hud_SetEnabled( shown, true )
	// No 'button.SetNavDown( x )' here: calling a method on a var is a UI COMPILE ERROR in a typed script ("May not index
	// into type var with type string", run ai_menu1 2026-09-29); only 'untyped' files (Northstar menu_mod_settings.nut,
	// vanilla _menus.nut) may. The global SetNavUpDown (_menus.nut, untyped) links the list in order and wraps.
	array<var> navOrder = [ file.upscalerButton, shown, file.nrButton, file.tuningButton, file.fgButton ]
	SetNavUpDown( navOrder )

	file.aiRefreshing = false
}

// Upscaler: Off -> tf2nr_dlss_sr 0 (the last quality is kept in tf2nr_upscale_last);
// DLSS / FSR 3 -> tf2nr_upscaler 1 / 2, and tf2nr_dlss_sr comes back from tf2nr_upscale_last if it was Off
void function TF2NR_AI_Upscaler_Changed( var button )
{
	if ( file.aiRefreshing || !file.aiReady )
		return
	string v = Hud_GetDialogListSelectionValue( button )
	int sr = GetConVarInt( TF2NR_CVAR_SR )
	if ( v == "0" )
	{
		if ( sr >= 1 && sr <= 5 )
			SetConVarInt( TF2NR_CVAR_LAST, sr )
		if ( sr != 0 )
			SetConVarInt( TF2NR_CVAR_SR, 0 )
	}
	else if ( v == "1" || v == "2" )
	{
		SetConVarInt( TF2NR_CVAR_UPSCALER, ( v == "2" && TF2NR_FSR3_UPSCALER ) ? 2 : 1 )
		if ( sr < 1 || sr > 5 )
			SetConVarInt( TF2NR_CVAR_SR, TF2NR_LastQuality() )
	}
	TF2NR_AI_Log( "Upscaler -> " + v )
	TF2NR_AI_Refresh()
}

// Quality: edits tf2nr_dlss_sr 1..5; while the upscaler is Off it edits tf2nr_upscale_last (used when it comes back on)
void function TF2NR_AI_Quality_Changed( var button )
{
	if ( file.aiRefreshing || !file.aiReady )
		return
	string v = Hud_GetDialogListSelectionValue( button )
	int q = 0
	for ( int i = 1; i <= 5; i++ )
	{
		if ( v == string( i ) )
			q = i
	}
	if ( q == 0 )
		return
	if ( GetConVarInt( TF2NR_CVAR_SR ) == 0 )
		SetConVarInt( TF2NR_CVAR_LAST, q )
	else
		SetConVarInt( TF2NR_CVAR_SR, q )
	TF2NR_AI_Log( "Quality -> " + v )
	TF2NR_AI_Refresh()
}

// DLSS 5 / Frame Generation are ConVar-bound (the engine writes the convar); log only (the logged convar values may
// still be the old ones if the engine writes the convar after this event)
void function TF2NR_AI_ConVarRow_Changed( var button )
{
	if ( file.aiRefreshing || !file.aiReady )
		return
	string what = button == file.nrButton ? "DLSS 5" : "Frame Generation"
	TF2NR_AI_Log( what + " changed" )
}

void function TF2NR_AI_Focused( var button )
{
	string desc = ""
	if ( button == file.upscalerButton )
		desc = TF2NR_FSR3_UPSCALER ? TF2NR_DESC_UPSCALER_FSR3 : TF2NR_DESC_UPSCALER
	else if ( button == file.qualityDlssButton )
		desc = TF2NR_DESC_QUALITY_DLSS
	else if ( button == file.qualityFsrButton )
		desc = TF2NR_DESC_QUALITY_FSR
	else if ( button == file.nrButton )
		desc = TF2NR_FSR3_UPSCALER ? TF2NR_DESC_NR_AI_FSR3 : TF2NR_DESC_NR_AI
	else if ( button == file.tuningButton )
		desc = TF2NR_DESC_TUNING
	else if ( button == file.fgButton )
		desc = TF2NR_FSR3_UPSCALER ? TF2NR_DESC_FG_FSR3 : TF2NR_DESC_FG
	SetElementsTextByClassname( file.aiMenu, "MenuItemDescriptionClass", desc )
}

void function TF2NR_AIFooter_Focused( var button )
{
	SetElementsTextByClassname( file.aiMenu, "MenuItemDescriptionClass", "" )
}

// ---- DLSS 5 Adjustments submenu ---------------------------------------------------------------------------------

// AddMenu initFunc: called from InitMenus() for every registered menu.
void function TF2NR_InitDLSS5Menu()
{
	var menu = GetMenu( TF2NR_MENU_DLSS5 )
	file.dlss5Menu = menu

	AddMenuEventHandler( menu, eUIEvent.MENU_OPEN, TF2NR_DLSS5Menu_Open )
	AddMenuEventHandler( menu, eUIEvent.MENU_CLOSE, TF2NR_DLSS5Menu_Close )

	TF2NR_AddRow( menu, "SwchDLSS5NR", false, "DLSS 5 Neural Rendering", TF2NR_CVAR_NR, "", 0.0, 1.0, TF2NR_DESC_NR )
	TF2NR_AddRow( menu, "BtnDLSS5Reset", false, "Reset to defaults", "", "", 0.0, 0.0, TF2NR_DESC_RESET )

	TF2NR_AddRow( menu, "SldNRIntensity", true, "Strength", "tf2nr_nr_intensity", "%.1f", 0.0, 2.0,
		"How strongly the neural model re-renders the image (NRIntensity). 2 is the most the NVIDIA runtime accepts." )
	TF2NR_AddRow( menu, "SldNRLocalTone", true, "Local tone", "tf2nr_nr_local_tone", "%.1f", 0.0, 2.0,
		"Tone strength of the neural pass, region by region (NRLocalTone): how far it relights areas of the image." )
	TF2NR_AddRow( menu, "SldNRStructure", true, "Structure", "tf2nr_nr_structure", "%.1f", 0.0, 2.0,
		"Fine structure and surface detail the model adds (NRLocalStructure)." )
	TF2NR_AddRow( menu, "SldNRSkin", true, "Character / skin detail", "tf2nr_nr_skin", "%.2f", 0.0, 1.0,
		"Structure on faces, skin and characters (NRSkinStructure). 0 is neutral, 1 is the most detail this RenoDX build allows." )
	TF2NR_AddRow( menu, "SwchNRStyle", false, "Style", "tf2nr_nr_style", "", 0.0, 2.0,
		"Default lets the NVIDIA runtime choose; Natural and Cinematic force that look (NRStyle)." )

	TF2NR_AddRow( menu, "SldNRLookStrength", true, "Look strength", "tf2nr_nr_look_strength", "%.1f", 0.0, 2.0,
		"Scales the whole change Neural Rendering made, after the model (NRLookStrength): 0 removes it, 1 keeps it, 2 doubles it. High values can flicker." )
	TF2NR_AddRow( menu, "SldNRLookColour", true, "Colour change", "tf2nr_nr_look_colour", "%.1f", 0.0, 2.0,
		"How far the model moves colours toward or away from grey (NRLookColour): 0 keeps the game's colours, 1 keeps the model's change. Lower it if skin looks pink." )
	TF2NR_AddRow( menu, "SldNRLookHue", true, "Tint", "tf2nr_nr_look_hue", "%.1f", 0.0, 2.0,
		"How far the model turns hues (NRLookHue): 0 keeps the game's hues, 1 keeps the model's tint." )
	TF2NR_AddRow( menu, "SldNRLookDarken", true, "Darkening", "tf2nr_nr_look_darken", "%.1f", 0.0, 2.0,
		"How much of the model's darkening is kept (NRLookDarken). The game's colour grade tints darkened skin magenta, so lower it if faces look dark or maroon." )
	TF2NR_AddRow( menu, "SldNRLookDetail", true, "Fine detail", "tf2nr_nr_look_detail", "%.1f", 0.0, 2.0,
		"Scales the model's small-scale change (NRLookDetail): 1 keeps it as made. High values can flicker." )
	TF2NR_AddRow( menu, "SwchNRStabilize", false, "Stabilize", "tf2nr_nr_stabilize", "", 0.0, 2.0,
		"Smooths the model's broad change over time (NRLookStabilize). Motion follows the game's motion vectors; Static suits still views. Either adds a little lag to lighting changes." )

	if ( Hud_HasChild( menu, "BtnDLSS5Reset" ) )
		AddButtonEventHandler( Hud_GetChild( menu, "BtnDLSS5Reset" ), UIE_CLICK, TF2NR_ResetDialog )

	AddEventHandlerToButtonClass( menu, "RuiFooterButtonClass", UIE_GET_FOCUS, TF2NR_Footer_Focused )
	AddMenuFooterOption( menu, BUTTON_A, "#A_BUTTON_SELECT" )
	AddMenuFooterOption( menu, BUTTON_B, "#B_BUTTON_BACK", "#BACK" )
}

void function TF2NR_AddRow( var menu, string elem, bool isSlider, string label, string cvar, string valueFormat, float minValue, float maxValue, string desc )
{
	if ( !Hud_HasChild( menu, elem ) )
	{
		printt( "[TF2NR] tf2nr_dlss5.menu has no " + elem + " - row skipped" )
		return
	}
	var element = Hud_GetChild( menu, elem )
	var button = element
	var slider = null
	if ( isSlider )
	{
		slider = element
		button = Hud_GetChild( element, "BtnDropButton" )
		AddButtonEventHandler( slider, UIE_CHANGE, TF2NR_Slider_Changed )
	}
	SetButtonRuiText( button, label )
	AddButtonEventHandler( button, UIE_GET_FOCUS, TF2NR_Row_Focused )

	file.rowButtons.append( button )
	file.rowSliders.append( slider )
	file.rowLabels.append( label )
	file.rowDescs.append( desc )
	file.rowCvars.append( cvar )
	file.rowFormats.append( valueFormat )
	file.rowMin.append( minValue )
	file.rowMax.append( maxValue )
}

void function TF2NR_DLSS5Menu_Open()
{
	UI_SetPresentationType( ePresentationType.NO_MODELS )
	for ( int i = 0; i < file.rowButtons.len(); i++ )
	{
		if ( file.rowFormats[ i ] != "" )
			TF2NR_ShowValue( i, GetConVarFloat( file.rowCvars[ i ] ) )
	}
}

void function TF2NR_DLSS5Menu_Close()
{
	SavePlayerSettings()   // as vanilla audio/video menus do on close: persists the ARCHIVE_PLAYERPROFILE convars
}

void function TF2NR_ShowValue( int i, float value )
{
	SetButtonRuiText( file.rowButtons[ i ], file.rowLabels[ i ] + "   " + format( file.rowFormats[ i ], value ) )
}

void function TF2NR_Slider_Changed( var slider )
{
	for ( int i = 0; i < file.rowSliders.len(); i++ )
	{
		if ( file.rowSliders[ i ] == slider )
		{
			TF2NR_ShowValue( i, Hud_SliderControl_GetCurrentValue( slider ) )
			return
		}
	}
}

void function TF2NR_Row_Focused( var button )
{
	string desc = ""
	for ( int i = 0; i < file.rowButtons.len(); i++ )
	{
		if ( file.rowButtons[ i ] == button )
		{
			desc = file.rowDescs[ i ]
			if ( file.rowCvars[ i ] != "" && file.rowCvars[ i ] != TF2NR_CVAR_NR )
				desc = desc + ( TF2NR_ADJUST_LIVE ? TF2NR_NOTE_LIVE : TF2NR_NOTE_RESTART )
			break
		}
	}
	SetElementsTextByClassname( file.dlss5Menu, "MenuItemDescriptionClass", desc )
}

void function TF2NR_Footer_Focused( var button )
{
	SetElementsTextByClassname( file.dlss5Menu, "MenuItemDescriptionClass", "" )
}

void function TF2NR_ResetDialog( var button )
{
	DialogData dialogData
	dialogData.header = "Reset DLSS 5 adjustments?"
	dialogData.message = TF2NR_DESC_RESET
	AddDialogButton( dialogData, "#RESTORE", TF2NR_ResetToDefaults )
	AddDialogButton( dialogData, "#CANCEL" )
	OpenDialog( dialogData )
}

void function TF2NR_ResetToDefaults()
{
	for ( int i = 0; i < file.rowCvars.len(); i++ )
	{
		string cvar = file.rowCvars[ i ]
		if ( cvar == "" || cvar == TF2NR_CVAR_NR )
			continue
		SetConVarToDefault( cvar )
		var slider = file.rowSliders[ i ]
		if ( slider != null )
		{
			// SliderControl has no SetValue: pin min = max = value, then restore the range (Northstar ns_slider.nut trick)
			float value = GetConVarFloat( cvar )
			Hud_SliderControl_SetMin( slider, value )
			Hud_SliderControl_SetMax( slider, value )
			Hud_SliderControl_SetMin( slider, file.rowMin[ i ] )
			Hud_SliderControl_SetMax( slider, file.rowMax[ i ] )
			TF2NR_ShowValue( i, value )
		}
	}
	SetConVarToDefault( "tf2nr_nr_menu_tuning" )
}
