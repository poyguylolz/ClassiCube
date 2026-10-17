#include "Game.h"
#include "Block.h"
#include "World.h"
#include "Lighting.h"
#include "MapRenderer.h"
#include "Graphics.h"
#include "Camera.h"
#include "Options.h"
#include "Funcs.h"
#include "ExtMath.h"
#include "Gui.h"
#include "Window.h"
#include "Event.h"
#include "Utils.h"
#include "Logger.h"
#include "Entity.h"
#include "Chat.h"
#include "Commands.h"
#include "Drawer2D.h"
#include "Model.h"
#include "Particle.h"
#include "Http.h"
#include "Inventory.h"
#include "Input.h"
#include "InputHandler.h"
#include "Server.h"
#include "TexturePack.h"
#include "Screens.h"
#include "SelectionBox.h"
#include "AxisLinesRenderer.h"
#include "EnvRenderer.h"
#include "HeldBlockRenderer.h"
#include "SelOutlineRenderer.h"
#include "Menus.h"
#include "Audio.h"
#include "Stream.h"
#include "Builder.h"
#include "Protocol.h"
#include "Picking.h"
#include "Animations.h"
#include "SystemFonts.h"
#include "Formats.h"
#include "EntityRenderers.h"

struct _GameData Game;
static cc_uint64 frameStart;
cc_bool Game_UseCPEBlocks, Game_Running;

struct RayTracer Game_SelectedPos;
int Game_ViewDistance     = DEFAULT_VIEWDIST;
int Game_UserViewDistance = DEFAULT_VIEWDIST;
int Game_MaxViewDistance  = DEFAULT_MAX_VIEWDIST;

int     Game_FpsLimit, Game_Vertices;
cc_bool Game_SimpleArmsAnim;
static float gfx_minFrameMs;
static cc_bool autoPause;

cc_bool Game_ClassicMode, Game_ClassicHacks;
cc_bool Game_AllowCustomBlocks;
cc_bool Game_AllowServerTextures;
cc_bool Game_Anaglyph3D;

cc_bool Game_ViewBobbing, Game_HideGui;
cc_bool Game_BreakableLiquids, Game_ScreenshotRequested;
struct GameVersion Game_Version;

static char usernameBuffer[STRING_SIZE];
static char mppassBuffer[STRING_SIZE];
cc_string Game_Username  = String_FromArray(usernameBuffer);
cc_string Game_Mppass    = String_FromArray(mppassBuffer);
#ifdef CC_BUILD_SPLITSCREEN
int Game_NumStates = 1;
#endif

const char* const FpsLimit_Names[FPS_LIMIT_COUNT] = {
	"LimitVSync", "Limit30FPS", "Limit60FPS", "Limit120FPS", "Limit144FPS", "LimitNone",
};

static struct IGameComponent* comps_head;
static struct IGameComponent* comps_tail;
void Game_AddComponent(struct IGameComponent* comp) {
	LinkedList_Append(comp, comps_head, comps_tail);
}


static struct ScheduledTask2* tasks_head;
static struct ScheduledTask2* tasks_tail;
struct CoreScheduledTasks Game_Tasks;

/* Backwards compatibility */
struct ScheduledTask2Ext {
	struct ScheduledTask2 task;
	ScheduledTaskCallback callback;
};

static cc_bool ScheduledTask_Callback(struct ScheduledTask2* task) {
	struct ScheduledTask2Ext* src = (struct ScheduledTask2Ext*)task;
	struct ScheduledTask dst;

	dst.accumulator = task->accumulator;
	dst.interval    = task->interval;
	dst.Callback    = src->callback;

	dst.Callback(&dst);

	task->accumulator = dst.accumulator;
	task->interval    = dst.interval;
	src->callback     = dst.Callback;
	return true;
}

int ScheduledTask_Add(double interval, ScheduledTaskCallback callback) {
	struct ScheduledTask2Ext* t = Mem_Alloc(1, sizeof(struct ScheduledTask2Ext), "task");
	t->task.interval    = interval;
	t->task.callback    = ScheduledTask_Callback;
	t->callback         = callback;

	ScheduledTask2_Add(&t->task);
	return 0;
}

void ScheduledTask2_Add(struct ScheduledTask2* task) {
	task->accumulator = 0.0f;
	LinkedList_Append(task, tasks_head, tasks_tail);
}


void Game_ToggleFullscreen(void) {
	int state = Window_GetWindowState();
	cc_result res;

	if (state == WINDOW_STATE_FULLSCREEN) {
		res = Window_ExitFullscreen();
		if (res) Logger_SysWarn(res, "leaving fullscreen");
	} else {
		res = Window_EnterFullscreen();
		if (res) Logger_SysWarn(res, "going fullscreen");
	}
}

static void CycleViewDistanceForwards(const short* viewDists, int count) {
	int i, dist;
	for (i = 0; i < count; i++) {
		dist = viewDists[i];

		if (dist > Game_UserViewDistance) {
			Game_UserSetViewDistance(dist); return;
		}
	}
	Game_UserSetViewDistance(viewDists[0]);
}

static void CycleViewDistanceBackwards(const short* viewDists, int count) {
	int i, dist;
	for (i = count - 1; i >= 0; i--) {
		dist = viewDists[i];

		if (dist < Game_UserViewDistance) {
			Game_UserSetViewDistance(dist); return;
		}
	}
	Game_UserSetViewDistance(viewDists[count - 1]);
}

static const short normalDists[]  = { 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };
static const short classicDists[] = { 8, 32, 128, 512 };
void Game_CycleViewDistance(void) {
	const short* dists = Gui.ClassicMenu ? classicDists : normalDists;
	int count = Gui.ClassicMenu ? Array_Elems(classicDists) : Array_Elems(normalDists);

	if (Input_IsShiftPressed()) {
		CycleViewDistanceBackwards(dists, count);
	} else {
		CycleViewDistanceForwards(dists,  count);
	}
}

cc_bool Game_ReduceVRAM(void) {
	if (Game_UserViewDistance <= 16) return false;
	Game_UserViewDistance /= 2;
	Game_UserViewDistance = max(16, Game_UserViewDistance);

	MapRenderer_Refresh();
	Game_SetViewDistance(Game_UserViewDistance);
	Chat_AddRaw("&cOut of VRAM! Halving view distance..");
	return true;
}


void Game_SetViewDistance(int distance) {
	distance = min(distance, Game_MaxViewDistance);
	if (distance == Game_ViewDistance) return;
	Game_ViewDistance = distance;

	Event_RaiseVoid(&GfxEvents.ViewDistanceChanged);
	Camera_UpdateProjection();
}

void Game_UserSetViewDistance(int distance) {
	Game_UserViewDistance = distance;
	Options_SetInt(OPT_VIEW_DISTANCE, distance);
	Game_SetViewDistance(distance);
}

void Game_Disconnect(const cc_string* title, const cc_string* reason) {
	Event_RaiseVoid(&NetEvents.Disconnected);
	Game_Reset();
	DisconnectScreen_Show(title, reason);
}

void Game_Reset(void) {
	struct IGameComponent* comp;
	World_NewMap();

	for (comp = comps_head; comp; comp = comp->next) {
		if (comp->Reset) comp->Reset();
	}
}

void Game_UpdateBlock(int x, int y, int z, BlockID block) {
	BlockID old = World_GetBlock(x, y, z);
	World_SetBlock(x, y, z, block);

	if (Weather_Heightmap) {
		EnvRenderer_OnBlockChanged(x, y, z, old, block);
	}
	Lighting.OnBlockChanged(x, y, z, old, block);
	MapRenderer_OnBlockChanged(x, y, z, block);
}

void Game_ChangeBlock(int x, int y, int z, BlockID block) {
	BlockID old = World_GetBlock(x, y, z);
	Game_UpdateBlock(x, y, z, block);
	Server.SendBlock(x, y, z, old, block);
}

cc_bool Game_CanPick(BlockID block) {
	if (Blocks.Draw[block] == DRAW_GAS)    return false;
	if (Blocks.Draw[block] == DRAW_SPRITE) return true;
	return Blocks.Collide[block] != COLLIDE_LIQUID || Game_BreakableLiquids;
}

cc_bool Game_UpdateTexture(GfxResourceID* texId, struct Stream* src, const cc_string* file,
							cc_uint8* skinType, int* heightDivisor) {
	struct Bitmap bmp;
	cc_bool success;
	cc_result res;
	
	res = Png_Decode(&bmp, src);
	if (res) { Logger_SysWarn2(res, "decoding", file); return false; }
	
	/* E.g. gui.png only need top half of the texture loaded */
	if (heightDivisor && bmp.height >= *heightDivisor)
		bmp.height /= *heightDivisor;

	success = Game_ValidateBitmap(file, &bmp);
	if (success) {
		if (skinType) { *skinType = Utils_CalcSkinType(&bmp); }
		Gfx_RecreateTexture(texId, &bmp, TEXTURE_FLAG_MANAGED, false);
	}

	Mem_Free(bmp.scan0);
	return success;
}

cc_bool Game_ValidateBitmap(const cc_string* file, struct Bitmap* bmp) {
	int maxWidth = Gfx.MaxTexWidth, maxHeight = Gfx.MaxTexHeight;
	float texSize, maxSize;

	if (!bmp->scan0) {
		Chat_Add1("&cError loading %s from the texture pack.", file);
		return false;
	}
	
	if (bmp->width > maxWidth || bmp->height > maxHeight) {
		Chat_Add1("&cUnable to use %s from the texture pack.", file);

		Chat_Add4("&c Its size is (%i,%i), your GPU supports (%i,%i) at most.",
				&bmp->width, &bmp->height, &maxWidth, &maxHeight);
		return false;
	}

	if (Gfx.MaxTexSize && (bmp->width * bmp->height > Gfx.MaxTexSize)) {
		Chat_Add1("&cUnable to use %s from the texture pack.", file);
		texSize = (bmp->width * bmp->height) / (1024.0f * 1024.0f);
		maxSize = Gfx.MaxTexSize             / (1024.0f * 1024.0f);

		Chat_Add2("&c Its size is %f3 MB, your GPU supports %f3 MB at most.",
				&texSize, &maxSize);
		return false;
	}

	return Game_ValidateBitmapPow2(file, bmp);
}

cc_bool Game_ValidateBitmapPow2(const cc_string* file, struct Bitmap* bmp) {
	if (!Math_IsPowOf2(bmp->width) || !Math_IsPowOf2(bmp->height)) {
		Chat_Add1("&cUnable to use %s from the texture pack.", file);

		Chat_Add2("&c Its size is (%i,%i), which is not a power of two size.",
			&bmp->width, &bmp->height);
		return false;
	}
	return true;
}

void Game_UpdateDimensions(void) {
	Game.Width  = max(Window_Main.Width,  1);
	Game.Height = max(Window_Main.Height, 1);
}

static void Game_OnResize(void* obj) {
	Game_UpdateDimensions();
	Gfx_OnWindowResize(Game.Width, Game.Height);
	Camera_UpdateProjection();
}

static void HandleOnNewMap(void* obj) {
	struct IGameComponent* comp;
	for (comp = comps_head; comp; comp = comp->next) {
		if (comp->OnNewMap) comp->OnNewMap();
	}
}

static void HandleOnNewMapLoaded(void* obj) {
	struct IGameComponent* comp;
	for (comp = comps_head; comp; comp = comp->next) {
		if (comp->OnNewMapLoaded) comp->OnNewMapLoaded();
	}
}

static void HandleInactiveChanged(void* obj) {
	if (Window_Main.Inactive) {
		Chat_AddOf(&Gfx_LowPerfMessage, MSG_TYPE_EXTRASTATUS_2);
		Gfx_SetVSync(false);
		Game_SetMinFrameTime(1000 / 1.0f);
		Gfx.ReducedPerfMode = true;
	} else {
		Chat_AddOf(&String_Empty,       MSG_TYPE_EXTRASTATUS_2);
		Game_SetFpsLimit(Game_FpsLimit);

		Gfx.ReducedPerfMode         = false;
		Gfx.ReducedPerfModeCooldown = 2;
	}

#ifdef CC_BUILD_WEB
	extern void emscripten_resume_main_loop(void);
	emscripten_resume_main_loop();
#endif
}

static void Game_WarnFunc(const cc_string* msg) {
	cc_string str = *msg, line;
	while (str.length) {
		String_UNSAFE_SplitBy(&str, '\n', &line);
		Chat_Add1("&c%s", &line);
	}
}

static void LoadOptions(void) {
	Game_ClassicMode  = Options_GetBool(OPT_CLASSIC_MODE,  false);
	Game_ClassicHacks = Options_GetBool(OPT_CLASSIC_HACKS, false);
	Game_Anaglyph3D   = Options_GetBool(OPT_ANAGLYPH3D,    false);
#if defined CC_BUILD_PS1 || defined CC_BUILD_SATURN
	/* View bobbing requires per-frame matrix multiplications - costly on FPU less systems */
	Game_ViewBobbing  = Options_GetBool(OPT_VIEW_BOBBING,  false);
#else
	Game_ViewBobbing  = Options_GetBool(OPT_VIEW_BOBBING,  true);
#endif
	
	Game_AllowCustomBlocks   = !Game_ClassicMode && Options_GetBool(OPT_CUSTOM_BLOCKS,      true);
	Game_SimpleArmsAnim      = !Game_ClassicMode && Options_GetBool(OPT_SIMPLE_ARMS_ANIM,   false);
	Game_BreakableLiquids    = !Game_ClassicMode && Options_GetBool(OPT_MODIFIABLE_LIQUIDS, false);
	Game_AllowServerTextures = !Game_ClassicMode && Options_GetBool(OPT_SERVER_TEXTURES,    true);

	Game_ViewDistance     = Options_GetInt(OPT_VIEW_DISTANCE, 8, 4096, DEFAULT_VIEWDIST);
	Game_UserViewDistance = Game_ViewDistance;
	/* TODO: Do we need to support option to skip SSL */
	/*cc_bool skipSsl = Options_GetBool("skip-ssl-check", false);
	if (skipSsl) {
		ServicePointManager.ServerCertificateValidationCallback = delegate { return true; };
		Options.Set("skip-ssl-check", false);
	}*/
	autoPause = Options_GetBool(OPT_AUTO_PAUSE, true);
}

static void Game_PendingClose(void* obj) { Game_Running = false; }
static void Game_Load(void) {
	struct IGameComponent* comp;
	Game_UpdateDimensions();
	Game_SetFpsLimit(Options_GetEnum(OPT_FPS_LIMIT, 0, FpsLimit_Names, FPS_LIMIT_COUNT));
	Gfx_Create();
	
	tasks_head = NULL;
	Logger_WarnFunc = Game_WarnFunc;
	LoadOptions();
	GameVersion_Load();
	Utils_EnsureDirectory("maps");

	Event_Register_(&WorldEvents.NewMap,           NULL, HandleOnNewMap);
	Event_Register_(&WorldEvents.MapLoaded,        NULL, HandleOnNewMapLoaded);
	Event_Register_(&WindowEvents.Resized,         NULL, Game_OnResize);
	Event_Register_(&WindowEvents.Closing,         NULL, Game_PendingClose);
	Event_Register_(&WindowEvents.InactiveChanged, NULL, HandleInactiveChanged);

	Game_AddComponent(&Gfx_Component);
	Game_AddComponent(&World_Component);
	Game_AddComponent(&Textures_Component);
	Game_AddComponent(&Input_Component);
	Game_AddComponent(&InputHandler_Component);
	Game_AddComponent(&Camera_Component);
	Game_AddComponent(&Blocks_Component);
	Game_AddComponent(&Drawer2D_Component);
	Game_AddComponent(&SystemFonts_Component);

	Game_AddComponent(&Chat_Component);
	Game_AddComponent(&Commands_Component);
	Game_AddComponent(&Particles_Component);
	Game_AddComponent(&TabList_Component);
	Game_AddComponent(&Models_Component);
	Game_AddComponent(&Http_Component);
	Game_AddComponent(&Lighting_Component);

	Game_AddComponent(&Animations_Component);
	Game_AddComponent(&Inventory_Component);
	Game_AddComponent(&Builder_Component);
	Game_AddComponent(&MapRenderer_Component);
	Game_AddComponent(&EnvRenderer_Component);
	Game_AddComponent(&Server_Component);
	Game_AddComponent(&Protocol_Component);
	Game_AddComponent(&Entities_Component);

	Game_AddComponent(&Gui_Component);
	Game_AddComponent(&Selections_Component);
	Game_AddComponent(&HeldBlockRenderer_Component);
	/* Gfx_SetDepthWrite(true) */
	Game_AddComponent(&SelOutlineRenderer_Component);
	Game_AddComponent(&Audio_Component);
	Game_AddComponent(&AxisLinesRenderer_Component);
	Game_AddComponent(&Formats_Component);
	Game_AddComponent(&EntityRenderers_Component);

	Plugins_LoadAll();
	for (comp = comps_head; comp; comp = comp->next) {
		if (comp->Init) comp->Init();
	}

	TexturePack_ExtractCurrent(true);
	if (TexturePack_DefaultMissing) {
		Window_ShowDialog("Missing file",
			"Both default.zip and classicube.zip are missing,\n try downloading resources first.\n\nClassiCube will still run, but without any textures.");
	}
	Gfx_WarnIfNecessary();

	if (Gfx.Limitations & GFX_LIMIT_VERTEX_ONLY_FOG)
		EnvRenderer_SetMode(EnvRenderer_Minimal | ENV_LEGACY);
	if (Gfx.Limitations & GFX_LIMIT_MINIMAL)
		EnvRenderer_SetMode(ENV_MINIMAL);

	Server.BeginConnect();
}

void Game_SetFpsLimit(int method) {
	float minFrameTime = 0;
	Game_FpsLimit = method;

	switch (method) {
	case FPS_LIMIT_144: minFrameTime = 1000/144.0f; break;
	case FPS_LIMIT_120: minFrameTime = 1000/120.0f; break;
	case FPS_LIMIT_60:  minFrameTime = 1000/60.0f;  break;
	case FPS_LIMIT_30:  minFrameTime = 1000/30.0f;  break;
	}
	Gfx_SetVSync(method == FPS_LIMIT_VSYNC);
	Game_SetMinFrameTime(minFrameTime);
}

#ifdef CC_BUILD_WEB
extern void Window_SetMinFrameTime(float timeMS);

void Game_SetMinFrameTime(float frameTimeMS) {
	if (frameTimeMS) Window_SetMinFrameTime(frameTimeMS);
}
#else
void Game_SetMinFrameTime(float frameTimeMS) {
	gfx_minFrameMs = frameTimeMS;
}
#endif

static void Render3DFrame(float delta, float t) {
	struct Matrix mvp;
	Vec3 pos;

	Camera.Active->GetView(&Gfx.View);
	/*Gfx_LoadMatrix(MATRIX_PROJ, &Gfx.Projection);
	Gfx_LoadMatrix(MATRIX_VIEW, &Gfx.View);
	Frustum_CalcPlanes(&Gfx.Projection, &Gfx.View);*/
	Gfx_LoadMVP(&Gfx.View, &Gfx.Projection, &mvp);
	Frustum_CalcPlanes(&mvp);

	if (EnvRenderer_ShouldRenderSkybox()) EnvRenderer_RenderSkybox();
	AxisLinesRenderer_Render();
	Entities_RenderModels(delta, t);
	EntityNames_Render();

	Particles_Render(t);
	EnvRenderer_RenderSky();
	EnvRenderer_RenderClouds();

	MapRenderer_Update(delta);
	MapRenderer_RenderNormal(delta);
	EnvRenderer_RenderMapSides();

	EntityShadows_Render();
	if (Game_SelectedPos.valid && !Game_HideGui) {
		SelOutlineRenderer_Render(&Game_SelectedPos, true);
	}

	/* Render water over translucent blocks when under the water outside the map for proper alpha blending */
	pos = Camera.CurrentPos;
	if (pos.y < Env.EdgeHeight && (pos.x < 0 || pos.z < 0 || pos.x > World.Width || pos.z > World.Length)) {
		MapRenderer_RenderTranslucent(delta);
		EnvRenderer_RenderMapEdges();
	} else {
		EnvRenderer_RenderMapEdges();
		MapRenderer_RenderTranslucent(delta);
	}

	/* Need to render again over top of translucent block, as the selection outline */
	/* is drawn without writing to the depth buffer */
	if (Game_SelectedPos.valid && !Game_HideGui && Blocks.Draw[Game_SelectedPos.block] == DRAW_TRANSLUCENT) {
		SelOutlineRenderer_Render(&Game_SelectedPos, false);
	}

	Selections_Render();
	EntityNames_RenderHovered();
	if (!Game_HideGui) HeldBlockRenderer_Render(delta);
}

static void Render3D_Anaglyph(float delta, float t) {
	struct Matrix proj = Gfx.Projection;
	struct Matrix view = Gfx.View;

	Gfx_Set3DLeft(&proj, &view);
	Render3DFrame(delta, t);

	Gfx_Set3DRight(&proj, &view);
	Render3DFrame(delta, t);

	Gfx_End3D(&proj, &view);
}

/* Renders a small picture-in-picture inset showing the world as seen from  */
/* whichever linked portal you're further from, angled to match your own    */
/* current look direction (position from the portal, angle from you - see   */
/* PhysicsComp_CheckPortal in EntityComponents.c for the rest of the portal */
/* of the portal feature). This is a second full render of the visible      */
/* world into a viewport-restricted corner of the screen - not a texture    */
/* applied to the portal block itself, so it won't line up with the block's */
/* on-screen position/shape. Toggled via /client portalview.                */
/*                                                                            */
/* Overrides Camera.CurrentPos rather than setting Gfx.View directly -       */
/* Render3DFrame's very first line is Camera.Active->GetView(&Gfx.View),     */
/* which unconditionally recomputes the view matrix from Camera.CurrentPos,  */
/* so setting Gfx.View beforehand gets immediately overwritten. Overriding   */
/* CurrentPos lets that existing computation do the right thing instead of   */
/* fighting it, and correctly carries through to everything else in that     */
/* render (frustum culling, entity sorting, etc.) being relative to the      */
/* portal's position rather than the player's.                              */
/* FOV, aspect ratio and view distance all match the player's own camera -   */
/* Camera.Fov and Game_ViewDistance are reused directly, and vw/vh are the   */
/* full window size divided by the same factor, so the aspect ratio is       */
/* identical too. Frustum culling of unseen chunks happens automatically:    */
/* Render3DFrame recalculates Frustum_CalcPlanes from whatever Gfx.View/     */
/* Gfx.Projection currently are on every single call, so this gets a         */
/* correctly-culled render for the portal's own view for free.              */
/* Projects a world position to screen pixel coordinates using the CURRENT   */
/* Gfx.View/Gfx.Projection (the real player camera's, at the point this is   */
/* called - before PortalPreview_Render below overrides them for its own     */
/* second render). Returns false if the point is behind the camera.          */
static cc_bool WorldToScreen(Vec3 world, int* sx, int* sy) {
	Vec3 viewPos;
	float clipX, clipY, clipW, ndcX, ndcY;

	Vec3_Transform(&viewPos, &world, &Gfx.View); /* View is affine - fine as-is */

	clipX = viewPos.x * Gfx.Projection.row1.x + viewPos.y * Gfx.Projection.row2.x + viewPos.z * Gfx.Projection.row3.x + Gfx.Projection.row4.x;
	clipY = viewPos.x * Gfx.Projection.row1.y + viewPos.y * Gfx.Projection.row2.y + viewPos.z * Gfx.Projection.row3.y + Gfx.Projection.row4.y;
	clipW = viewPos.x * Gfx.Projection.row1.w + viewPos.y * Gfx.Projection.row2.w + viewPos.z * Gfx.Projection.row3.w + Gfx.Projection.row4.w;
	if (clipW <= 0.01f) return false; /* behind the camera */

	ndcX = clipX / clipW;
	ndcY = clipY / clipW;
	*sx  = (int)((ndcX * 0.5f + 0.5f) * Game.Width);
	*sy  = (int)((1.0f - (ndcY * 0.5f + 0.5f)) * Game.Height); /* NDC +Y is up, screen +Y is down */
	return true;
}

static void PortalPreview_Render(float delta, float t) {
	extern IVec3   portal_pos[];
	extern float   portal_yaw[];
	extern int     portal_link[];
	extern int     portal_count;
	extern cc_bool portal_previewEnabled;
	struct Entity* e = &Entities.CurPlayer->Base;
	Vec3 camPos, playerPos, savedCamPos, blockCentre;
	int i, closest, dest, screenX, screenY;
	float bestDistSq, dx, dz, distSq, dist, fovy, aspect;
	float angleOffset, savedYaw;
	cc_bool savedThirdPerson;
	int vx, vy, vw, vh;
	struct Matrix projSaved, proj;

	if (!portal_previewEnabled) return;

	/* Find the linked portal you're nearest to (the one you're about to walk */
	/* into) - the inset shows what's waiting for you at its destination.     */
	playerPos = Camera.CurrentPos;
	closest = -1;
	bestDistSq = 0.0f;
	for (i = 0; i < portal_count; i++) {
		if (portal_link[i] < 0) continue;
		if (World_GetBlock(portal_pos[i].x, portal_pos[i].y, portal_pos[i].z) != BLOCK_PORTAL) continue;
		dx = playerPos.x - (portal_pos[i].x + 0.5f);
		dz = playerPos.z - (portal_pos[i].z + 0.5f);
		distSq = dx * dx + dz * dz;
		if (closest < 0 || distSq < bestDistSq) { closest = i; bestDistSq = distSq; }
	}
	if (closest < 0) return;
	dest = portal_link[closest];
	if (World_GetBlock(portal_pos[dest].x, portal_pos[dest].y, portal_pos[dest].z) != BLOCK_PORTAL) return;

	/* Project the NEAR portal (the one you're approaching) to screen space,  */
	/* using the real camera's current matrices - not the destination one,   */
	/* since it's the near portal's on-screen position that the inset should */
	/* sit at. Skipped entirely if it's behind you or off-screen; no check    */
	/* for a wall/block in front of it (that would need a raycast this       */
	/* doesn't do), so it can still show through solid geometry.             */
	blockCentre.x = portal_pos[closest].x + 0.5f;
	blockCentre.y = portal_pos[closest].y + 0.5f;
	blockCentre.z = portal_pos[closest].z + 0.5f;
	if (!WorldToScreen(blockCentre, &screenX, &screenY)) return;
	dist = Math_SqrtF(bestDistSq);
	if (screenX < 0 || screenX > Game.Width || screenY < 0 || screenY > Game.Height) return;

	camPos.x = portal_pos[dest].x + 0.5f;
	camPos.y = portal_pos[dest].y + 0.5f; /* eye height 0.5 */
	camPos.z = portal_pos[dest].z + 0.5f;

	/* Same "compass" formula as the actual teleport, in PhysicsComp_CheckPortal, */
	/* EntityComponents.c - kept identical so the preview always matches what   */
	/* actually happens when you walk through.                                  */
	angleOffset = portal_yaw[dest] - portal_yaw[closest];

	/* GetOrientation() (called inside Render3DFrame via Camera.Active->     */
	/* GetView) reads e->Yaw directly - there's no other way to feed it a    */
	/* custom angle, so the real entity's yaw is temporarily overridden for  */
	/* the duration of this render and restored after.                       */
	savedYaw = e->Yaw;
	e->Yaw   = savedYaw + angleOffset;

	/* Forces the local player's own body to render (normally suppressed in  */
	/* first person) and hides the first-person held-block viewmodel - see   */
	/* Entity.c:764 and HeldBlockRenderer.c:259, both gated on this exact    */
	/* flag. Camera.CurrentPos is already set to the portal's exact position */
	/* below rather than going through ThirdPersonCamera_GetPosition's zoom/ */
	/* pullback logic at all, so this comes with no distance offset for free.*/
	savedThirdPerson         = Camera.Active->isThirdPerson;
	Camera.Active->isThirdPerson = true;

	/* Size shrinks with distance (rough perspective cue), clamped so it     */
	/* never gets absurdly huge up close or vanishes far away. Centred on    */
	/* the projected screen point, then clamped back on-screen so it doesn't */
	/* get cut off at an edge.                                               */
	vw = (int)(Game.Height * 6.0f / (dist + 3.0f));
	if (vw < Game.Height / 10) vw = Game.Height / 10;
	if (vw > Game.Height / 3)  vw = Game.Height / 3;
	vh = vw;
	vx = screenX - vw / 2;
	vy = screenY - vh / 2;
	if (vx < 0) vx = 0;
	if (vy < 0) vy = 0;
	if (vx + vw > Game.Width)  vx = Game.Width  - vw;
	if (vy + vh > Game.Height) vy = Game.Height - vh;
	if (CC_GFX_BACKEND_IS_GL()) {
		/* Gfx_SetViewport doesn't flip Y for GL backends the way Gfx_SetScissor  */
		/* does internally (see _GLShared.h) - OpenGL's native origin is bottom-  */
		/* left, so convert our top-down vy into the equivalent bottom-up value.  */
		vy = Game.Height - vh - vy;
	}

	projSaved = Gfx.Projection;

	fovy   = Camera.Fov * MATH_DEG2RAD;
	aspect = (float)vw / (float)vh; /* always 1.0, vh is forced equal to vw */
	Gfx_CalcPerspectiveMatrix(&proj, fovy, aspect, (float)Game_ViewDistance);
	Gfx.Projection = proj;

	savedCamPos       = Camera.CurrentPos;
	Camera.CurrentPos = camPos;

	Gfx_SetViewport(vx, vy, vw, vh);
	Gfx_ClearBuffers(GFX_BUFFER_DEPTH);

	Render3DFrame(delta, t);

	Camera.CurrentPos = savedCamPos;
	Gfx.Projection     = projSaved;
	Camera.Active->isThirdPerson = savedThirdPerson;
	e->Yaw   = savedYaw;
	Gfx_SetViewport(0, 0, Game.Width, Game.Height);
}

/* Aimbot, toggled/tuned by /client aimbot in Commands.c. Aims at the player */
/* nearest to you (by distance). No line of    */
/* sight check. Uses its own degree/radian constant so /client pi can't      */
/* throw it off.                                                             */
cc_bool aimbot_enabled = false;
float   aimbot_range   = 50.0f;  /* max distance, blocks */
float   aimbot_smooth  = 0.0f;   /* visible mode: 0 = snap instantly, towards 1 = slower */
cc_bool aimbot_bots    = false;  /* also target bots (entities with no tab list entry) */
int     aimbot_silent  = 0;      /* 0 = visible aim, 1 = silent on click, 2 = silent always */
int     aimbot_fire    = 0;      /* silent-click mode auto-click: 0 off, 1 break, 2 place */
float   aimbot_fireDelay = 0.25f;/* seconds between auto-clicks */
float   aimbot_lagMs   = -1.0f;  /* lag compensation: -1 = use ping, 0 = off, >0 = fixed ms */

/* Orientation reported to the server instead of the real view, while set.   */
/* Read in Protocol.c (position + click packets). Your own screen never uses */
/* it, which is what makes the aim "silent".                                 */
cc_bool silent_active = false;
float   silent_yaw = 0.0f, silent_pitch = 0.0f;

extern int Ping_AveragePingMS(void);
#define AIM_RAD2DEG 57.2957795f

static float Aim_Wrap180(float a) {
	while (a >  180.0f) a -= 360.0f;
	while (a < -180.0f) a += 360.0f;
	return a;
}

/* Finds the nearest player. Outputs yaw (degrees) and signed   */
/* pitch (degrees, positive = down). Leads moving targets by your latency.     */
cc_bool Aimbot_FindTarget(float* outYaw, float* outPitch, int* outId) {
	struct Entity* me = &Entities.CurPlayer->Base;
	Vec3 eye;
	float yaw, pitch, dx, dy, dz, horiz, score, rangeSq;
	float lagSec, lead, bestScore = 0.0f, bestYaw = 0.0f, bestPitch = 0.0f;
	cc_bool found = false;
	int i, bestId = -1;

	eye      = Entity_GetEyePosition(me);
	rangeSq  = aimbot_range * aimbot_range;

	/* Lag compensation: lead targets by our latency. Their per-tick movement is */
	/* taken from their interpolation endpoints; physics ticks are a fixed 20/s.  */
	/* Horizontal only - vertical movement (jumps/falls) isn't predictable.       */
	lagSec = aimbot_lagMs < 0.0f ? (float)Ping_AveragePingMS() : aimbot_lagMs;
	lagSec /= 1000.0f;
	if (lagSec > 0.5f) lagSec = 0.5f;
	lead = lagSec * 20.0f;

	for (i = 0; i < MAX_NET_PLAYERS; i++) {
		struct Entity* t = Entities.List[i];
		if (!t || t == me) continue;
		if (!aimbot_bots && !TabList.NameOffsets[i]) continue; /* real players have a tab list entry, bots don't */

		dx = (t->Position.x + (t->next.pos.x - t->prev.pos.x) * lead) - eye.x;
		dy = (t->Position.y + t->Size.y * 0.6f) - eye.y; /* roughly chest height */
		dz = (t->Position.z + (t->next.pos.z - t->prev.pos.z) * lead) - eye.z;
		if (dx * dx + dy * dy + dz * dz > rangeSq) continue;

		/* inverse of Vec3_GetDirVector: x = cos(p)sin(y), y = -sin(p), z = -cos(p)cos(y) */
		horiz = Math_SqrtF(dx * dx + dz * dz);
		yaw   = Math_Atan2f(-dz, dx)    * AIM_RAD2DEG;
		pitch = Math_Atan2f(horiz, -dy) * AIM_RAD2DEG;
		if (pitch >  89.0f) pitch =  89.0f;
		if (pitch < -89.0f) pitch = -89.0f;

		score = dx * dx + dy * dy + dz * dz; /* nearest player by distance, so it re-targets as others get closer */
		if (!found || score < bestScore) {
			found = true; bestScore = score; bestYaw = yaw; bestPitch = pitch; bestId = i;
		}
	}
	if (!found) return false;
	*outYaw = bestYaw; *outPitch = bestPitch; *outId = bestId;
	return true;
}

static void Aimbot_Update(void) {
	struct Entity* me = &Entities.CurPlayer->Base;
	struct LocationUpdate update;
	float yaw, pitch, curYaw, curPitch, dyaw, dpitch;
	int id;

	silent_active = false; /* recomputed every frame (silent click mode sets it per click, in InputHandler.c) */
	if (!aimbot_enabled || Gui.InputGrab) return;
	if (aimbot_silent == 1) return;
	if (!Aimbot_FindTarget(&yaw, &pitch, &id)) return;

	if (aimbot_silent == 2) {
		silent_active = true;
		silent_yaw    = Math_ClampAngle(yaw);
		silent_pitch  = Math_ClampAngle(pitch);
		return;
	}

	curYaw   = me->next.yaw;
	curPitch = me->next.pitch;
	if (curPitch > 180.0f) curPitch -= 360.0f;
	dyaw   = Aim_Wrap180(yaw - curYaw);
	dpitch = pitch - curPitch;
	update.flags = LU_HAS_YAW | LU_HAS_PITCH; /* same call mouse-look uses, Camera.c */
	update.yaw   = Math_ClampAngle(curYaw   + dyaw   * (1.0f - aimbot_smooth));
	update.pitch = Math_ClampAngle(curPitch + dpitch * (1.0f - aimbot_smooth));
	me->VTABLE->SetLocation(me, &update);
}

static void PerformScheduledTasks(float time) {
	struct ScheduledTask2* task = tasks_head;
	struct ScheduledTask2* next;

	while (task) {
		task->accumulator += time;
		next = task->next; /* cache in case callback removes task */

		while (task->accumulator >= task->interval) {
			task->callback(task);
			task->accumulator -= task->interval;
		}
		task = next;
	}
}

void Game_TakeScreenshot(void) {
	cc_string filename; char fileBuffer[STRING_SIZE];
	cc_string path;     char pathBuffer[FILENAME_SIZE];
	struct cc_datetime now;
	cc_filepath raw_path;
	cc_result res;

#ifndef CC_BUILD_WEB
	struct Stream stream;
#endif
	Game_ScreenshotRequested = false;
	DateTime_CurrentLocal(&now);

	String_InitArray(filename, fileBuffer);
	String_Format3(&filename, "screenshot_%p4-%p2-%p2", &now.year, &now.month, &now.day);
	String_Format3(&filename, "-%p2-%p2-%p2.png", &now.hour, &now.minute, &now.second);

#ifdef CC_BUILD_WEB
	extern void interop_TakeScreenshot(const char* path);
	Platform_EncodePath(&raw_path, &filename);
	interop_TakeScreenshot(raw_path.buffer);
#else
	if (!Utils_EnsureDirectory("screenshots")) return;
	String_InitArray(path, pathBuffer);
	String_Format1(&path, "screenshots/%s", &filename);

	Platform_EncodePath(&raw_path, &path);
	res = Stream_CreatePath(&stream, &raw_path);
	if (res) { Logger_IOWarn2(res, "creating", &raw_path); return; }

	res = Gfx_TakeScreenshot(&stream);
	if (res) {
		Logger_IOWarn2(res, "saving to", &raw_path); stream.Close(&stream); return;
	}

	res = stream.Close(&stream);
	if (res) { Logger_IOWarn2(res, "closing", &raw_path); return; }
	Chat_Add1("&eTaken screenshot as: %s", &filename);

#ifdef CC_BUILD_MOBILE
	Platform_ShareScreenshot(&filename);
#endif
#endif
}


#ifdef CC_BUILD_WEB
static void LimitFPS(void) {
	/* Can't use Thread_Sleep on the web. (spinwaits instead of sleeping) */
	/* Instead the web browser manages the frame timing */
}
#else
static float gfx_targetTime, gfx_actualTime;

static CC_INLINE float ElapsedMilliseconds(cc_uint64 beg, cc_uint64 end) {
	cc_uint64 elapsed = Stopwatch_ElapsedMicroseconds(beg, end);
	if (elapsed > 5000000) elapsed = 5000000;
	
	/* Avoid uint64 / float division, as that typically gets implemented */
	/* using a library function rather than a direct CPU instruction */
	return (int)elapsed / 1000.0f;
}

/* Examines difference between expected and actual frame times, */
/*  then sleeps if actual frame time is too fast */
static void LimitFPS(void) {
	cc_uint64 frameEnd, sleepEnd;
	
	frameEnd = Stopwatch_Measure();
	gfx_actualTime += ElapsedMilliseconds(frameStart, frameEnd);
	gfx_targetTime += gfx_minFrameMs;

	/* going faster than FPS limit - sleep to slow down */
	if (gfx_actualTime < gfx_targetTime) {
		float cooldown = gfx_targetTime - gfx_actualTime;
		Thread_Sleep((int)(cooldown + 0.5f));

		/* also accumulate Thread_Sleep duration, as actual sleep */
		/*  duration can significantly deviate from requested time */
		/*  (e.g. requested 4ms, but actually slept for 8ms) */
		sleepEnd = Stopwatch_Measure();
		gfx_actualTime += ElapsedMilliseconds(frameEnd, sleepEnd);
	}

	/* reset accumulated time to avoid excessive FPS drift */
	if (gfx_targetTime >= 1000) { gfx_actualTime = 0; gfx_targetTime = 0; }
}
#endif

static CC_INLINE void Game_DrawFrame(float delta, float t) {
	int i;

	if (!Gui_GetBlocksWorld()) {
		Camera.Active->GetPickedBlock(&Game_SelectedPos); /* TODO: only pick when necessary */
		Camera_KeyLookUpdate(delta);
		InputHandler_Tick(delta);

		if (Game_Anaglyph3D) {
			Render3D_Anaglyph(delta, t);
		} else {
			Render3DFrame(delta, t);
		}
		PortalPreview_Render(delta, t);
	} else {
		RayTracer_SetInvalid(&Game_SelectedPos);
	}

	Gfx_Begin2D(Game.Width, Game.Height);
	Gui_RenderGui(delta);
	for (i = 0; i < Array_Elems(Game.Draw2DHooks); i++)
	{
		if (Game.Draw2DHooks[i]) Game.Draw2DHooks[i](delta);
	}

/* TODO find a better solution than this */
#ifdef CC_BUILD_3DS
	if (Game_Anaglyph3D) {
		extern void Gfx_SetTopRight(void);
		Gfx_SetTopRight();
		Gui_RenderGui(delta);
	}
	for (i = 0; i < Array_Elems(Game.Draw2DHooks); i++)
	{
		if (Game.Draw2DHooks[i]) Game.Draw2DHooks[i](delta);
	}
#endif
	Gfx_End2D();
}

#ifdef CC_BUILD_SPLITSCREEN
static void DrawSplitscreen(float delta, float t, int i, int x, int y, int w, int h) {
	Gfx_SetViewport(x, y, w, h);
	Gfx_SetScissor( x, y, w, h);
	Game.CurrentState = i;
	
	Entities.CurPlayer = &LocalPlayer_Instances[i];
	LocalPlayer_SetInterpPosition(Entities.CurPlayer, t);
	Camera.CurrentPos = Camera.Active->GetPosition(t);
	
	Game_DrawFrame(delta, t);
}

int Game_MapState(int deviceIndex) {
	if (Game_NumStates >= 4 && deviceIndex == 3) return 3;
	if (Game_NumStates >= 3 && deviceIndex == 2) return 2;
	if (Game_NumStates >= 2 && deviceIndex == 1) return 1;

	return 0;
}
#endif

void Game_RenderFrame(void) {
	double deltaD;
	float t, delta;

	cc_uint64 render  = Stopwatch_Measure();
	cc_uint64 elapsed = Stopwatch_ElapsedMicroseconds(frameStart, render);
	/* avoid large delta with suspended process */
	if (elapsed > 5000000) elapsed = 5000000;
	
	deltaD = (int)elapsed / (1000.0 * 1000.0);
	delta  = (float)deltaD;
	Window_ProcessEvents(delta);

	if (delta <= 0.0f) return;
	frameStart = render;

	/* TODO: Should other tasks get called back too? */
	/* Might not be such a good idea for the http_clearcache, */
	/* don't really want all skins getting lost */
	if (Gfx.LostContext) {
		if (Gfx_TryRestoreContext()) {
			Gfx_RecreateContext();
			/* all good, context is back */
		} else {
			Game.Time += delta; /* TODO: Not set in two places? */
			Server.Tick(&Game_Tasks.network);
			Thread_Sleep(16);
			return;
		}
	}

	Gfx_BeginFrame();
	Gfx_BindIb(Gfx.DefaultIb);
	Game.Time += deltaD;
	Game_Vertices = 0;
	Gamepad_Tick(delta);

#ifdef CC_BUILD_SPLITSCREEN
	/* TODO: find a better solution */
	for (int i = 0; i < Game_NumStates; i++)
	{
		Game.CurrentState  = i;
		Entities.CurPlayer = &LocalPlayer_Instances[i];
		Camera.Active->UpdateMouse(Entities.CurPlayer, delta);
	}
	Game.CurrentState  = 0;
	Entities.CurPlayer = &LocalPlayer_Instances[0];
#else
	Camera.Active->UpdateMouse(Entities.CurPlayer, delta);
#endif
	Aimbot_Update(); /* after mouse-look, so it gets the final say each frame */

	if (!Window_Main.Focused && !Gui.InputGrab && autoPause) 
		Gui_ShowPauseMenu();

	if (Bind_IsTriggered[BIND_ZOOM_SCROLL] && !Gui.InputGrab) {
		InputHandler_SetFOV(Camera.ZoomFov);
	}

	PerformScheduledTasks(delta);
	t = (float)(Game_Tasks.entities.accumulator / Game_Tasks.entities.interval);
	LocalPlayer_SetInterpPosition(Entities.CurPlayer, t);

	Camera.CurrentPos = Camera.Active->GetPosition(t);
	/* NOTE: EnvRenderer_UpdateFog also also sets clear color */
	EnvRenderer_UpdateFog();
	AudioBackend_Tick();

#if !defined CC_BUILD_SYMBIAN
	/* TODO: Not calling Gfx_EndFrame doesn't work with Direct3D9 */
	if (Window_Main.Inactive) return;
#endif
	Gfx_ClearBuffers(GFX_BUFFER_COLOR | GFX_BUFFER_DEPTH);
	
#ifdef CC_BUILD_SPLITSCREEN
	switch (Game_NumStates) {
		case 1:
			Game_DrawFrame(delta, t); break;
		case 2:
			DrawSplitscreen(delta, t, 0,  0,               0, Game.Width, Game.Height / 2);
			DrawSplitscreen(delta, t, 1,  0, Game.Height / 2, Game.Width, Game.Height / 2);
			break;
		case 3:
			DrawSplitscreen(delta, t, 0,              0,               0, Game.Width    , Game.Height / 2);
			DrawSplitscreen(delta, t, 1,              0, Game.Height / 2, Game.Width / 2, Game.Height / 2);
			DrawSplitscreen(delta, t, 2, Game.Width / 2, Game.Height / 2, Game.Width / 2, Game.Height / 2);
			break;
		case 4:
			DrawSplitscreen(delta, t, 0,              0,               0, Game.Width / 2, Game.Height / 2);
			DrawSplitscreen(delta, t, 1, Game.Width / 2,               0, Game.Width / 2, Game.Height / 2);
			DrawSplitscreen(delta, t, 2,              0, Game.Height / 2, Game.Width / 2, Game.Height / 2);
			DrawSplitscreen(delta, t, 3, Game.Width / 2, Game.Height / 2, Game.Width / 2, Game.Height / 2);
			break;
	}
#else
	Game_DrawFrame(delta, t);
#endif

	if (Game_ScreenshotRequested) Game_TakeScreenshot();
	Gfx_EndFrame();
	if (gfx_minFrameMs != 0.0f) LimitFPS();
}

void Game_Free(void) {
	struct IGameComponent* comp;
	/* Most components will call OnContextLost in their Free functions */
	/* Set to false so components will always free managed textures too */
	Gfx.ManagedTextures = false;
	Event_UnregisterAll();

	for (comp = comps_head; comp; comp = comp->next)
	{
		if (comp->Free) comp->Free();
	}

	Game_Running    = false;
	Logger_WarnFunc = Logger_DialogWarn;
	Gfx_Free();
	Options_SaveIfChanged();
	Window_DisableRawMouse();
}

#ifdef CC_BUILD_WEB
cc_bool Game_ShouldClose(void) {
	if (!Game_Running) return true;

	if (Server.IsSinglePlayer) {
		/* Close if map was saved within last 5 seconds */
		return World.LastSave + 5 >= Game.Time;
	}

	/* Try to intercept Ctrl+W or Cmd+W for multiplayer */
	if (Input_IsCtrlPressed() || Input_IsWinPressed()) return false;
	/* Also try to intercept mouse back button (Mouse4) */
	return !Input.Pressed[CCMOUSE_X1];
}
#endif

void Game_Setup(void) {
	cc_string title; char titleBuffer[STRING_SIZE];
	int width  = Options_GetInt(OPT_WINDOW_WIDTH,  0, DisplayInfo.Width,  0);
	int height = Options_GetInt(OPT_WINDOW_HEIGHT, 0, DisplayInfo.Height, 0);

	/* No custom resolution has been set */
	if (width == 0 || height == 0) {
		width = 854; height = 480;
		if (DisplayInfo.Width < 854) width = 640;
	}
	
	String_InitArray(title, titleBuffer);
	String_Format2(&title, "%c (%s)", GAME_APP_TITLE, &Game_Username);
	
	Window_Create3D(width, height);
	Window_SetTitle(&title);
	Window_Show();
	Game_Running = true;
	Game.CurrentState = 0;

	Game_Load();
	Event_RaiseVoid(&WindowEvents.Resized);
	frameStart = Stopwatch_Measure();
}

