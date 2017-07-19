//******************************************************************************
//
// This file is part of the OpenHoldem project
//    Source code:           https://github.com/OpenHoldem/openholdembot/
//    Forums:                http://www.maxinmontreal.com/forums/index.php
//    Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose:
//
//******************************************************************************

#include "stdafx.h"
#include "CHeartbeatThread.h"

#include <process.h>
#include "CAutoconnector.h"
#include "CAutoplayer.h"
#include "CAutoplayerFunctions.h"
#include "CBetroundCalculator.h"
#include "CHeartbeatDelay.h"
#include "CEngineContainer.h"
#include "CGameState.h"
#include "CIteratorThread.h"
#include "CLazyScraper.h"
#include "COpenHoldemHopperCommunication.h"
#include "COpenHoldemStarter.h"
#include "COpenHoldemStatusbar.h"
#include "COpenHoldemTitle.h"
#include "CPreferences.h"
#include "CScraper.h"
#include "CSymbolEngineAutoplayer.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineUserchair.h"
#include "..\CTablemap\CTablemap.h"
#include "CTableMapLoader.h"
#include "CTablepointChecker.h"
#include "CTableTitle.h"
#include "CTablePositioner.h"
#include "CValidator.h"
#include "DialogScraperOutput.h"
#include "MainFrm.h"
#include "MemoryLogging.h"
#include "NumericalFunctions.h"
#include "OpenHoldem.h"

CHeartbeatThread	 *p_heartbeat_thread = NULL;
CRITICAL_SECTION	 CHeartbeatThread::cs_update_in_progress;
long int			     CHeartbeatThread::_heartbeat_counter = 0;
CHeartbeatThread   *CHeartbeatThread::pParent = NULL;
CHeartbeatDelay    CHeartbeatThread::_heartbeat_delay;
COpenHoldemStarter CHeartbeatThread::_openholdem_starter;

CHeartbeatThread::CHeartbeatThread() {
	InitializeCriticalSectionAndSpinCount(&cs_update_in_progress, 4000);
  _heartbeat_counter = 0;
  // Create events
	_m_stop_thread = CreateEvent(0, TRUE, FALSE, 0);
	_m_wait_thread = CreateEvent(0, TRUE, FALSE, 0);
}

CHeartbeatThread::~CHeartbeatThread() {
	// Trigger thread to stop
	::SetEvent(_m_stop_thread);

	// Wait until thread finished
	::WaitForSingleObject(_m_wait_thread, k_max_time_to_wait_for_thread_to_shutdown);

	// Close handles
	::CloseHandle(_m_stop_thread);
	::CloseHandle(_m_wait_thread);

	DeleteCriticalSection(&cs_update_in_progress);
	p_heartbeat_thread = NULL;
}

void CHeartbeatThread::StartThread() {
	// Start thread
	write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Starting heartbeat thread\n");
    assert(this != NULL);
	AfxBeginThread(HeartbeatThreadFunction, this);
}

UINT CHeartbeatThread::HeartbeatThreadFunction(LPVOID pParam) {
  CTablepointChecker tablepoint_checker;
	pParent = static_cast<CHeartbeatThread*>(pParam);
  assert(pParent != NULL);
	// Seed the RNG
	srand((unsigned)GetTickCount());

	while (true) {
		_heartbeat_counter++;
		write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Starting next cycle\n");
		// Check event for stop thread
		if(::WaitForSingleObject(pParent->_m_stop_thread, 0) == WAIT_OBJECT_0) {
			// Set event
      write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Ending heartbeat thread\n");
			::SetEvent(pParent->_m_wait_thread);
			AfxEndThread(0);
		}
    LogMemoryUsage("Begin of heartbeat thread cycle");
    assert(p_tablemap_loader != NULL);
		p_tablemap_loader->ReloadAllTablemapsIfChanged();
    assert(p_autoconnector != NULL);
    write_log(preferences.debug_alltherest(), "[CHeartbeatThread] location Johnny_B\n");
    if (p_autoconnector->IsConnectedToGoneWindow()) {
      p_autoconnector->Disconnect("table disappeared");
    }
    if (!p_autoconnector->IsConnectedToAnything()) {
      // Not connected
      AutoConnect();
    }
    // No "else" here
    // We want one fast scrape immediately after connection
    // without any heartbeat-sleeping.
    write_log(preferences.debug_alltherest(), "[CHeartbeatThread] location Johnny_C\n");
		if (p_autoconnector->IsConnectedToExistingWindow()) {
      if (tablepoint_checker.TablepointsMismatchedTheLastNHeartbeats()) {
        p_autoconnector->Disconnect("table theme changed (tablepoints)");
      } else {
        ScrapeEvaluateAct();
      } 		
		}
    assert(p_watchdog != NULL);
    p_watchdog->HandleCrashedAndFrozenProcesses();
    if (preferences.use_auto_starter()) {
      _openholdem_starter.StartNewInstanceIfNeeded();
    }
    if (preferences.use_auto_shutdown()) {
      _openholdem_starter.CloseThisInstanceIfNoLongerNeeded();
    }
    _heartbeat_delay.FlexibleSleep();
		write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Heartbeat cycle ended\n");
	}
}

void CHeartbeatThread::ScrapeEvaluateAct() {
	p_table_positioner->AlwaysKeepPositionIfEnabled();
	// This critical section lets other threads know that the internal state is being updated
	EnterCriticalSection(&pParent->cs_update_in_progress);

	////////////////////////////////////////////////////////////////////////////////////////////
	// Scrape window
  p_table_title->UpdateTitle();
  write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Calling DoScrape.\n");
  p_lazyscraper->DoScrape();
  // We must not check if the scrape of the table changed, because:
  //   * some symbol-engines must be evaluated no matter what
  //   * we might need to act (sitout, ...) on empty/non-changing tables
  //   * auto-player needs stable frames too
	p_engine_container->EvaluateAll();
	// Reply-frames no longer here in the heartbeat.
  // we have a "ReplayFrameController for that.
  LeaveCriticalSection(&pParent->cs_update_in_progress);
	p_openholdem_title->UpdateTitle();
	////////////////////////////////////////////////////////////////////////////////////////////
	// Update scraper output dialog if it is present
	if (m_ScraperOutputDlg) {
		m_ScraperOutputDlg->UpdateDisplay();
	}
  ////////////////////////////////////////////////////////////////////////////////////////////
	// Save state
	write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Calling CaptureState.\n");
	p_game_state->CaptureState();
  
	////////////////////////////////////////////////////////////////////////////////////////////
	// OH-Validator
	write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Calling ValidateGameState.\n");
	p_validator->ValidateGameState();

	////////////////////////////////////////////////////////////////////////////////////////////
	// Autoplayer
	write_log(preferences.debug_heartbeat(), "[HeartBeatThread] autoplayer_engaged(): %s\n", 
		Bool2CString(p_autoplayer->autoplayer_engaged()));
	write_log(preferences.debug_heartbeat(), "[HeartBeatThread] user_chair_confirmed(): %s\n", 
		Bool2CString(p_symbol_engine_userchair->userchair_confirmed()));
	// If autoplayer is engaged, we know our chair, and the DLL hasn't told us to wait, then go do it!
	if (p_autoplayer->autoplayer_engaged()) {
		write_log(preferences.debug_heartbeat(), "[HeartBeatThread] Calling DoAutoplayer.\n");
		p_autoplayer->DoAutoplayer();
	}
}

void CHeartbeatThread::AutoConnect() {
  write_log(preferences.debug_alltherest(), "[CHeartbeatThread] location Johnny_D\n");
	assert(!p_autoconnector->IsConnectedToAnything());
	if (preferences.autoconnector_when_to_connect() == k_AutoConnector_Connect_Permanent) {
		if (p_autoconnector->SecondsSinceLastFailedAttemptToConnect() > 1 /* seconds */) {
			write_log(preferences.debug_autoconnector(), "[CHeartbeatThread] going to call Connect()\n");
			p_autoconnector->Connect(NULL);
		}	else {
			write_log(preferences.debug_autoconnector(), "[CHeartbeatThread] Reconnection blocked. Other instance failed previously.\n");
		}
	}
}