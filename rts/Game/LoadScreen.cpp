/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <SDL.h>
#include <functional>

#include "Rendering/GL/myGL.h"
#include "LoadScreen.h"
#include "Game.h"
#include "GlobalUnsynced.h"
#include "Game/Players/Player.h"
#include "Game/Players/PlayerHandler.h"
#include "Game/UI/MouseHandler.h"
#include "ExternalAI/SkirmishAIHandler.h"
#include "Lua/LuaIntro.h"
#include "Lua/LuaMenu.h"
#include "Map/MapInfo.h"
#include "Rendering/Fonts/glFont.h"
#include "Rendering/GlobalRendering.h"
#include "Rendering/Textures/NamedTextures.h"
#include "Sim/Misc/TeamHandler.h"
#include "Sim/Path/IPathManager.h"
#include "System/Config/ConfigHandler.h"
#include "System/Exceptions.h"
#include "System/Sync/FPUCheck.h"
#include "System/Log/ILog.h"
#include "Net/Protocol/NetProtocol.h"
#include "System/Matrix44f.h"
#include "System/SafeUtil.h"
#include "System/FileSystem/FileHandler.h"
#include "System/Platform/Watchdog.h"
#include "System/Platform/Threading.h"
#include "System/Sound/ISound.h"
#include "System/Sound/ISoundChannels.h"

#if !defined(HEADLESS) && !defined(NO_SOUND)
#include "System/Sound/OpenAL/EFX.h"
#include "System/Sound/OpenAL/EFXPresets.h"
#endif

#include <vector>

CONFIG(int, LoadingMT).defaultValue(0).safemodeValue(0);


CLoadScreen* CLoadScreen::singleton = nullptr;

CLoadScreen::CLoadScreen(const std::string& _mapName, const std::string& _modName, ILoadSaveHandler* _saveFile) :
	mapName(_mapName),
	modName(_modName),
	saveFile(_saveFile),
	localFont(font),
	mtLoading(true),
	lastDrawTime(0)
{
}

CLoadScreen::~CLoadScreen()
{
	// Kill() must have been called first, such that the loading
	// thread can not access singleton while its dtor is running
	assert(!gameLoadThread.joinable());

	if (clientNet != nullptr)
		clientNet->KeepUpdating(false);
	if (netHeartbeatThread.joinable())
		netHeartbeatThread.join();

	if (!gu->globalQuit) {
		activeController = game;

		if (luaMenu != nullptr)
			luaMenu->ActivateGame();
	}

	if (activeController == this)
		activeController = nullptr;

	if (luaIntro != nullptr)
		luaIntro->Shutdown();

	CLuaIntro::FreeHandler();

	if (!gu->globalQuit) {
		// send our playername to the server to indicate we finished loading
		const CPlayer* p = playerHandler->Player(gu->myPlayerNum);
		clientNet->Send(CBaseNetProtocol::Get().SendPlayerName(gu->myPlayerNum, p->name));
#ifdef SYNCCHECK
		clientNet->Send(CBaseNetProtocol::Get().SendPathCheckSum(gu->myPlayerNum, pathManager->GetPathCheckSum()));
#endif
		mouse->ShowMouse();

#if !defined(HEADLESS) && !defined(NO_SOUND)
		// sound is initialized at this point,
		// but EFX support is *not* guaranteed
		if (efx != nullptr) {
			efx->sfxProperties = *(mapInfo->efxprops);
			efx->CommitEffects();
		}
#endif
	}
}


bool CLoadScreen::Init()
{
	activeController = this;

	// hide the cursor until we are ingame
	SDL_ShowCursor(SDL_DISABLE);

	// When calling this function, mod archives have to be loaded
	// and gu->myPlayerNum has to be set.
	skirmishAIHandler.LoadPreGame();

#ifdef HEADLESS
	mtLoading = false;
#else
	const int mtCfg = configHandler->GetInt("LoadingMT");
	// user override
	mtLoading = (mtCfg > 0);
	// runtime detect. disable for intel/mesa drivers, they crash at multithreaded OpenGL (date: Nov. 2011)
	mtLoading |= (mtCfg < 0) && !globalRendering->haveMesa && !globalRendering->haveIntel;
#endif

	// Create a thread during the loading that pings the host/server, so it knows that this client is still alive/loading
	clientNet->KeepUpdating(true);

	netHeartbeatThread = std::move(spring::thread(Threading::CreateNewThread(std::bind(&CNetProtocol::UpdateLoop, clientNet))));
	game = new CGame(mapName, modName, saveFile);

	if (mtLoading) {
		try {
			// create the game-loading thread
			CglFont::threadSafety = true;
			gameLoadThread = std::move(COffscreenGLThread(std::bind(&CGame::LoadGame, game, mapName)));
			// Initialize localFont
			localFont = CglFont::LoadFont(font->GetFilePath(), false);
		} catch (const opengl_error& gle) {
			LOG_L(L_WARNING, "[LoadScreen::%s] offscreen GL context creation failed (error: \"%s\")", __func__, gle.what());

			mtLoading = false;
			localFont = font;
			CglFont::threadSafety = false;
		}
	}

	// LuaIntro must be loaded in the same thread that calls CLoadScreen::Draw
	CLuaIntro::LoadFreeHandler();

	if (mtLoading)
		return true;


	assert(!mtLoading);
	LOG("[LoadScreen::%s] single-threaded", __func__);
	game->LoadGame(mapName);
	return false;
}

void CLoadScreen::Kill()
{
	if (!gameLoadThread.joinable())
		return;

	// at this point, the thread running CGame::LoadGame
	// has finished and deregistered itself from WatchDog
	gameLoadThread.join();

	CglFont::threadSafety = false;
	if (localFont != font)
		spring::SafeDelete(localFont);
}


/******************************************************************************/

void CLoadScreen::CreateInstance(const std::string& mapName, const std::string& modName, ILoadSaveHandler* saveFile)
{
	assert(singleton == nullptr);
	singleton = new CLoadScreen(mapName, modName, saveFile);

	if (singleton->Init())
		return;

	DeleteInstance();
}

void CLoadScreen::DeleteInstance()
{
	if (singleton == nullptr)
		return;

	singleton->Kill();
	spring::SafeDelete(singleton);
}


/******************************************************************************/

void CLoadScreen::ResizeEvent()
{
	if (luaIntro != nullptr)
		luaIntro->ViewResize();
}


int CLoadScreen::KeyPressed(int k, bool isRepeat)
{
	//FIXME add mouse events
	if (luaIntro != nullptr)
		luaIntro->KeyPress(k, isRepeat);

	return 0;
}

int CLoadScreen::KeyReleased(int k)
{
	if (luaIntro != nullptr)
		luaIntro->KeyRelease(k);

	return 0;
}


bool CLoadScreen::Update()
{
	{
		// keep checking this while we are the active controller
		std::lock_guard<spring::recursive_mutex> lck(mutex);
		good_fpu_control_registers(curLoadMessage.c_str());
	}

	if (game->IsFinishedLoading()) {
		CLoadScreen::DeleteInstance();
		return true;
	}

	// without this call the window manager would think the window is unresponsive and thus ask for hard kill
	if (!mtLoading)
		SDL_PollEvent(nullptr);

	CNamedTextures::Update();
	return true;
}


bool CLoadScreen::Draw()
{
	// limit FPS via sleep to not lock a singlethreaded CPU from loading the game
	if (mtLoading) {
		const spring_time now = spring_gettime();
		const unsigned diffTime = spring_tomsecs(now - lastDrawTime);

		constexpr unsigned wantedFPS = 50;
		constexpr unsigned minFrameTime = 1000 / wantedFPS;

		if (diffTime < minFrameTime)
			spring_sleep(spring_msecs(minFrameTime - diffTime));

		lastDrawTime = now;
	}

	// curLoadMessage might be concurrently updated
	std::lock_guard<spring::recursive_mutex> lck(mutex);

	// let LuaMenu keep the lobby connection alive
	if (luaMenu != nullptr)
		luaMenu->Update();

	if (luaIntro != nullptr) {
		luaIntro->Update();
		luaIntro->DrawGenesis();
		ClearScreen();
		luaIntro->DrawLoadScreen();
	}

	if (!mtLoading)
		globalRendering->SwapBuffers(true, false);

	return true;
}


/******************************************************************************/
/******************************************************************************/

void CLoadScreen::SetLoadMessage(const std::string& text, bool replace_lastline)
{
	Watchdog::ClearTimer(WDT_LOAD);

	std::lock_guard<spring::recursive_mutex> lck(mutex);

	if (!replace_lastline) {
		if (oldLoadMessages.empty()) {
			oldLoadMessages = curLoadMessage;
		} else {
			oldLoadMessages += "\n" + curLoadMessage;
		}
	}
	curLoadMessage = text;

	LOG("%s", text.c_str());
	LOG_CLEANUP();

	if (luaIntro != nullptr)
		luaIntro->LoadProgress(text, replace_lastline);

	// be paranoid about FPU state for the loading thread since some
	// external library might reset it (main thread state is checked
	// in ::Update)
	good_fpu_control_registers(curLoadMessage.c_str());

	if (!mtLoading) {
		Update();
		Draw();
	}
}

