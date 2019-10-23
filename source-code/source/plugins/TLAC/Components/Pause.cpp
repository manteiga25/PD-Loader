﻿#include "Pause.h"
#include "../Constants.h"
#include "GameState.h"
#include "Drawing.h"
#include "GL/glut.h"
#include "detours.h"
#include <windows.h>
#include <vector>
#include <chrono>

namespace TLAC::Components
{
	bool Pause::pause = false;
	bool Pause::isPaused = false;
	bool Pause::giveUp = false;
	bool Pause::showUI = true;
	int Pause::triangleAet = 0;
	int Pause::squareAet = 0;
	int Pause::crossAet = 0;
	int Pause::circleAet = 0;
	int Pause::curMenuPos = 0;
	int Pause::mainMenuPos = 0;
	Pause::menusets Pause::curMenuSet = MENUSET_MAIN;
	std::chrono::time_point<std::chrono::high_resolution_clock> Pause::menuItemMoveTime;
	std::vector<uint8_t> Pause::origAetMovOp;
	uint8_t* Pause::aetMovPatchAddress = (uint8_t*)0x1401703b3;
	std::vector<uint8_t> Pause::origFramespeedOp;
	uint8_t* Pause::framespeedPatchAddress = (uint8_t*)0x140192D50;
	std::vector<bool> Pause::streamPlayStates;
	bool(*divaGiveUpFunc)(void*) = (bool(*)(void* cls))GIVEUP_FUNC_ADDRESS;
	PlayerData* Pause::playerData;
	InputState* Pause::inputState;
	TouchSliderState* Pause::sliderState;
	JvsButtons Pause::filteredButtons;
	std::vector<Pause::menuSet> Pause::menu = {
		{
			"PAUSED",
			{
				{ "RESUME", unpause, false },
				//{ "RESTART", restart, false },
				{ "SE VOLUME", sevolmenu, false },
				{ "GIVE UP", giveup, false },
			}
		},
		{
			"SE VOLUME",
			{
				{ "+", sevolplus, true },
				{ "XX", mainmenu, false },
				{ "-", sevolminus, true },
			}
		},
	};

	Pause::Pause()
	{
	}

	Pause::~Pause()
	{
	}

	const char* Pause::GetDisplayName()
	{
		return "pause";
	}

	void Pause::saveOldPatchOps()
	{
		origAetMovOp.resize(8);
		memcpy(origAetMovOp.data(), aetMovPatchAddress, 8);

		origFramespeedOp.resize(4);
		memcpy(origFramespeedOp.data(), framespeedPatchAddress, 4);
	}

	void Pause::Initialize(ComponentsManager*)
	{
		inputState = (InputState*)(*(uint64_t*)INPUT_STATE_PTR_ADDRESS);
		playerData = (PlayerData*)PLAYER_DATA_ADDRESS;
		sliderState = (TouchSliderState*)SLIDER_CTRL_TASK_ADDRESS;

		saveOldPatchOps();

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)divaGiveUpFunc, hookedGiveUpFunc);
		DetourTransactionCommit();
	}

	void Pause::Update()
	{

	}

	void Pause::UpdatePostInput()
	{
		if (pause)
		{
			// enter pause mode on state transition
			if (!isPaused)
			{
				((void(*)())DSC_PAUSE_FUNC_ADDRESS)();

				saveOldPatchOps();
				InjectCode(aetMovPatchAddress, { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 });
				InjectCode(framespeedPatchAddress, { 0x0f, 0x57, 0xc0, 0xc3 }); // XORPS XMM0,XMM0; RET

				uint64_t audioMixerAddr = *(uint64_t*)(AUDIO_MAIN_CLASS_ADDRESS + 0x70);
				uint64_t audioStreamsAddress = *(uint64_t*)(audioMixerAddr + 0x18);
				int nAudioStreams = *(uint64_t*)(audioMixerAddr + 0x20);
				for (int i = 0; i < nAudioStreams; i++)
				{
					uint32_t* playstate = (uint32_t*)(audioStreamsAddress + i * 0x50 + 0x18);
					if (i < streamPlayStates.size())
						streamPlayStates[i] = *playstate;
					else
						streamPlayStates.push_back(*playstate);

					*playstate = 0;
				}

				// block all buttons from being passed to game
				filteredButtons = allButtons;

				setMenuPos(MENUSET_MAIN, 0);
				showUI = true;
				isPaused = true;
			}

			// always exit pause if key is tapped or no longer in game somehow
			if (isPauseKeyTapped() || !isInGame())
			{
				pause = false;
			}
			else
			{
				if (inputState->Tapped.Buttons & JVS_SQUARE)
				{
					showUI = !showUI;
					if (showUI)
					{
						menuItemMoveTime = std::chrono::high_resolution_clock::now(); // restart animations
					}
					else
					{
						// clear these from the screen too
						destroyAetLayer(triangleAet);
						destroyAetLayer(squareAet);
						destroyAetLayer(crossAet);
						destroyAetLayer(circleAet);
					}
				}

				// only process menu events when UI is visible
				if (showUI)
				{
					if (inputState->Tapped.Buttons & JVS_L)
						setMenuPos(curMenuSet, curMenuPos - 1);

					if (inputState->Tapped.Buttons & JVS_R)
						setMenuPos(curMenuSet, curMenuPos + 1);

					if (inputState->Tapped.Buttons & JVS_CROSS)
					{
						if (curMenuSet == MENUSET_MAIN)
						{
							pause = false;
						}
						else
						{
							setMenuPos(MENUSET_MAIN, mainMenuPos);
						}
					}

					if (inputState->Tapped.Buttons & JVS_CIRCLE)
						menu[curMenuSet].items[curMenuPos].callback();

					if (curMenuSet == MENUSET_SEVOL)
					{
						const char volformat[] = "%d";
						size_t size = snprintf(nullptr, 0, volformat, playerData->act_vol) + 1;
						char* buf = new char[size];
						snprintf(buf, size, volformat, playerData->act_vol);
						menu[MENUSET_SEVOL].items[1].name = buf;
						delete[] buf;
					}
				}
			}

			// no slider while paused
			// this is simpler than buttons because slider doesn't trigger on press, it triggers on movement
			// (therefore per-button blocking isn't needed)
			sliderState->ResetSensors();
		}
		else
		{
			// exit pause mode on state transition
			if (isPaused)
			{
				((void(*)())DSC_UNPAUSE_FUNC_ADDRESS)();

				InjectCode(aetMovPatchAddress, origAetMovOp);
				InjectCode(framespeedPatchAddress, origFramespeedOp);

				uint64_t audioMixerAddr = *(uint64_t*)(AUDIO_MAIN_CLASS_ADDRESS + 0x70);
				uint64_t audioStreamsAddress = *(uint64_t*)(audioMixerAddr + 0x18);;
				int nAudioStreams = *(uint64_t*)(audioMixerAddr + 0x20);
				for (int i = 0; i < nAudioStreams; i++)
				{
					uint32_t* playstate = (uint32_t*)(audioStreamsAddress + i * 0x50 + 0x18);
					if (i < streamPlayStates.size())
						*playstate = streamPlayStates[i];
				}

				destroyAetLayer(triangleAet);
				destroyAetLayer(squareAet);
				destroyAetLayer(crossAet);
				destroyAetLayer(circleAet);

				isPaused = false;
			}

			// buttons that have been tapped no longer need to be filtered (doing this with down broke retriggering)
			filteredButtons = (JvsButtons)(filteredButtons & ~inputState->Tapped.Buttons);

			// only enter pause if in game
			if (isInGame())
			{
				if (isPauseKeyTapped())
				{
					pause = true;
				}
			}
		}

		// swallow filtered button inputs
		inputState->Tapped.Buttons = (JvsButtons)(inputState->Tapped.Buttons & ~filteredButtons);
		inputState->DoubleTapped.Buttons = (JvsButtons)(inputState->DoubleTapped.Buttons & ~filteredButtons);
		inputState->Down.Buttons = (JvsButtons)(inputState->Down.Buttons & ~filteredButtons);
		inputState->Released.Buttons = (JvsButtons)(inputState->Released.Buttons & ~filteredButtons);
		inputState->IntervalTapped.Buttons = (JvsButtons)(inputState->IntervalTapped.Buttons & ~filteredButtons);
	}

	void Pause::UpdateDraw2D()
	{
		if (isPaused && showUI)
		{
			// setup draw objects
			FontInfo fontInfo(0x11);
			DrawParams dtParams(&fontInfo);
			dtParams.layer = bgLayer;

			// get aspect ratio
			float aspect = *(float*)UI_ASPECT_RATIO;


			// bg rect
			float bgWidth = aspect * 720 + 2; // add a couple of pixels to protect against rounding errors
			float bgLeft = -(bgWidth - 1280) / 2; // 0,0 is in the corner of a 720p view.. half of the extra over 1280 wide is the horizontal offset to centre the bg
			RectangleBounds rect;
			rect = { bgLeft, 0, bgWidth, 720 };
			dtParams.colour = 0xa0000000;
			dtParams.fillColour = 0xa0000000;
			fillRectangle(&dtParams, &rect);


			// pause icon
			/*
			const int pauseWidth = 80;
			const int pauseHeight = 110;
			const int pauseGap = 20;
			const int pausePosX = 32;
			const int pausePosY = 32;
			const int pausePartWidth = (pauseWidth - pauseGap) / 2;

			const int pauseX1 = pausePosX;
			const int pauseX2 = pausePosX + pausePartWidth + pauseGap;
			const int pauseY1 = pausePosY;

			dtParams.colour = 0x80ffffff;
			dtParams.fillColour = 0x80ffffff;
			rect = { pauseX1, pauseY1, pausePartWidth, pauseHeight };
			fillRectangle(&dtParams, &rect);
			rect = { pauseX2, pauseY1, pausePartWidth, pauseHeight };
			fillRectangle(&dtParams, &rect);
			*/


			dtParams.layer = contentLayer;

			// selection cursor
			int menuOrigin = menuY - menuItemHeight * (menu[curMenuSet].items.size() / 2.0);
			if (curMenuSet != MENUSET_MAIN)
			{
				menuOrigin += menuItemHeight * 0.4;
			}
			int selectBoxPos = menuOrigin - (menuItemHeight - menuTextSize) / 2 + menuItemHeight * curMenuPos;
			const float selectBoxWidth = 200;
			const float selectBoxHeight = menuItemHeight;
			const float selectBoxThickness = 2;

			const float selectBoxX = menuX - selectBoxWidth / 2;
			const float selectBoxY = selectBoxPos;

			rect = { selectBoxX, selectBoxY, selectBoxWidth, selectBoxHeight };
			dtParams.colour = 0xc0ffff00;
			dtParams.fillColour = 0xc0ffff00;
			drawRectangle(&dtParams, &rect, selectBoxThickness);


			// menu
			fontInfo.setSize(menuTextSize, menuTextSize);
			
			dtParams.textCurrentLoc = { menuX, (float)menuOrigin };
			dtParams.lineOriginLoc = dtParams.textCurrentLoc;

			if (curMenuSet != MENUSET_MAIN)
			{
				dtParams.textCurrentLoc.y -= menuItemHeight * 1.333f;
				dtParams.lineOriginLoc.y = dtParams.textCurrentLoc.y;
				dtParams.colour = 0xffffffff;
				drawText(&dtParams, (drawTextFlags)(DRAWTEXT_ALIGN_CENTRE | DRAWTEXT_STROKE), menu[curMenuSet].name);
				dtParams.textCurrentLoc.y += menuItemHeight * 1.333f;
				dtParams.lineOriginLoc.y = dtParams.textCurrentLoc.y;
			}

			for (int i = 0; i < menu[curMenuSet].items.size(); i++)
			{
				if (i == curMenuPos)
				{
					uint8_t alpha = (cosf(getMenuAnimPos() * 6.283185f) * 0.15 + 0.85) * 255;
					dtParams.colour = 0x00ffff00 | (alpha << 24);
				}
				else
				{
					dtParams.colour = 0xffffffff;
				}

				drawText(&dtParams, (drawTextFlags)(DRAWTEXT_ALIGN_CENTRE), menu[curMenuSet].items[i].name);
				dtParams.textCurrentLoc.y += menuItemHeight;
				dtParams.lineOriginLoc = dtParams.textCurrentLoc;
			}
						

			// key legend
			destroyAetLayer(triangleAet);
			destroyAetLayer(squareAet);
			destroyAetLayer(crossAet);
			destroyAetLayer(circleAet);

			float textLeft;
			if (aspect > 16.0f / 9.0f)
				textLeft = 32;
			else
				textLeft = (1280 - bgWidth) / 2 + 32; // 0,0 is in the corner of a 720p view.. half of the difference to 1280 wide is the horizontal offset to the window corner

			fontInfo.setSize(18, 18);
			dtParams.textCurrentLoc = { textLeft, 720 - 40 };
			dtParams.lineOriginLoc = dtParams.textCurrentLoc;
			const float spriteSize = 18;
			const float halfSpriteSize = spriteSize / 2;
			Point spriteLoc = { 0, dtParams.textCurrentLoc.y + halfSpriteSize }; // the aets are centered on their location, so fudge this a little
			const Point spriteScale = { spriteSize / 64, spriteSize / 64 }; // just approximated

			dtParams.colour = 0xffffffff;
			drawTextW(&dtParams, (drawTextFlags)(DRAWTEXT_ENABLE_XADVANCE), L"L/R:Move　");
			
			spriteLoc.x = dtParams.textCurrentLoc.x + halfSpriteSize;
			squareAet = createAetLayer(3, 0x19, CREATEAET_20000, "button_shikaku", &spriteLoc, 0, nullptr, nullptr, 0, 0, &spriteScale, nullptr);
			dtParams.textCurrentLoc.x += spriteSize;
			drawTextW(&dtParams, (drawTextFlags)(DRAWTEXT_ENABLE_XADVANCE), L":Hide Menu　");

			spriteLoc.x = dtParams.textCurrentLoc.x + halfSpriteSize;
			crossAet = createAetLayer(3, 0x19, CREATEAET_20000, "button_batsu", &spriteLoc, 0, nullptr, nullptr, 0, 0, &spriteScale, nullptr);
			dtParams.textCurrentLoc.x += spriteSize;
			if (curMenuSet == MENUSET_MAIN)
				drawTextW(&dtParams, (drawTextFlags)(DRAWTEXT_ENABLE_XADVANCE), L":Close　");
			else
				drawTextW(&dtParams, (drawTextFlags)(DRAWTEXT_ENABLE_XADVANCE), L":Back　");
			
			spriteLoc.x = dtParams.textCurrentLoc.x + halfSpriteSize;
			circleAet = createAetLayer(3, 0x19, CREATEAET_20000, "button_maru", &spriteLoc, 0, nullptr, nullptr, 0, 0, &spriteScale, nullptr);
			dtParams.textCurrentLoc.x += spriteSize;
			drawTextW(&dtParams, (drawTextFlags)(DRAWTEXT_ENABLE_XADVANCE), L":Select");
		}
	}

	void Pause::OnFocusLost()
	{
		if (isInGame())
			pause = true;
	}

	bool Pause::isPauseKeyTapped()
	{
		return inputState->Tapped.Buttons & JVS_START;
	}

	bool Pause::isInGame()
	{
		return *(GameState*)CURRENT_GAME_STATE_ADDRESS == GS_GAME && *(SubGameState*)CURRENT_GAME_SUB_STATE_ADDRESS == SUB_GAME_MAIN;
	}

	bool Pause::hookedGiveUpFunc(void* cls)
	{
		if (giveUp)
		{
			giveUp = false;
			pause = false;
			return true;
		}
		else
		{
			if (divaGiveUpFunc(cls))
			{
				pause = false;
				return true;
			}
		}
		return false;
	}

	void Pause::setSEVolume(int amount)
	{
		playerData->act_vol += amount;
		if (playerData->act_vol < 0) playerData->act_vol = 0;
		if (playerData->act_vol > 100) playerData->act_vol = 100;
		playerData->act_slide_vol = playerData->act_vol;
	}

	void Pause::setMenuPos(menusets set, int pos)
	{
		if (set >= 0 && set < menu.size())
			curMenuSet = set;
		else
			curMenuSet = MENUSET_MAIN;

		if (pos < 0)
			pos = menu[curMenuSet].items.size() - 1;
		else if (pos >= menu[curMenuSet].items.size())
			pos = 0;

		curMenuPos = pos;

		if (curMenuSet == MENUSET_MAIN)
			mainMenuPos = curMenuPos;

		menuItemMoveTime = std::chrono::high_resolution_clock::now(); // restart animations
	}

	float Pause::getMenuAnimPos()
	{
		const int duration = 1500000000; // 1.5s
		return (float)((std::chrono::high_resolution_clock::now() - menuItemMoveTime) % std::chrono::nanoseconds(duration)).count() / (float)duration;
	}

	void Pause::InjectCode(void* address, const std::vector<uint8_t> data)
	{
		const size_t byteCount = data.size() * sizeof(uint8_t);

		DWORD oldProtect;
		VirtualProtect(address, byteCount, PAGE_EXECUTE_READWRITE, &oldProtect);
		memcpy(address, data.data(), byteCount);
		VirtualProtect(address, byteCount, oldProtect, nullptr);
	}
}
