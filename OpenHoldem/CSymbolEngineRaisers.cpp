//******************************************************************************
//
// This file is part of the OpenHoldem project
//   Download page:         http://code.google.com/p/openholdembot/
//   Forums:                http://www.maxinmontreal.com/forums/index.php
//   Licensed under GPL v3: http://www.gnu.org/licenses/gpl.html
//
//******************************************************************************
//
// Purpose:
//
//******************************************************************************
//
// nopponentstruelyraising counts all people who voluntarily bet more than needed,
// especially:
//  * all raisers
//  * the first voluntary better postflop
// but not
//  * the infamous "blind-raisers" (Winholdem)
//  * people posting antes 
//
// nopponentstruelyraising counts only the info that is visible at the table,
// i.e. one orbit (max). Formerly it was well-defined only at out turn,
// but we try to make it well-defined all the time, mainly because
// people don't understand the restrictions of "Raises" (OpenPPl,
// implemented with the use of nopponentsraising:
// http://www.maxinmontreal.com/forums/viewtopic.php?f=297&t=18141)
//
// Now nopponentstruelyraising should count:
//  * a full orbit when it is our turn
//  * a partial orbit from dealer to hero if we did not yet act
//  * a partial orbit behind us if we already acted 
//    (similar to RaisesSinceLastPlay, but might include a bettor)
//  * an orbit after the dealer if the userchair is unknown
//    (not really usable for OpenPPL which updates at our turn, 
//    but at least somewhat meaningful in the debug-tab).
//
//******************************************************************************

#include "stdafx.h"
#include "CSymbolEngineRaisers.h"

#include <assert.h>
#include "CBetroundCalculator.h"
#include "CScraper.h"
#include "CStringMatch.h"
#include "CSymbolEngineActiveDealtPlaying.h"
#include "CSymbolEngineAutoplayer.h"
#include "CSymbolEngineChipAmounts.h"
#include "CSymbolEngineDealerchair.h"
#include "CSymbolEngineDebug.h"
#include "CSymbolEngineHistory.h"
#include "CSymbolEngineTableLimits.h"
#include "CSymbolEngineUserchair.h"
#include "CPreferences.h"
#include "CTableState.h"
#include "NumericalFunctions.h"
#include "..\StringFunctionsDLL\string_functions.h"

CSymbolEngineRaisers *p_symbol_engine_raisers = NULL;

CSymbolEngineRaisers::CSymbolEngineRaisers() {
	// The values of some symbol-engines depend on other engines.
	// As the engines get later called in the order of initialization
	// we assure correct ordering by checking if they are initialized.
	assert(p_symbol_engine_active_dealt_playing != NULL);
  assert(p_symbol_engine_autoplayer != NULL);
	assert(p_symbol_engine_chip_amounts != NULL);
	assert(p_symbol_engine_dealerchair != NULL);
	assert(p_symbol_engine_tablelimits != NULL);
	assert(p_symbol_engine_userchair != NULL);
	// Also using p_symbol_engine_history one time,
	// but because we use "old" information here
	// there is no dependency on this cycle.
  //
  // Also using p_symbol_engine_debug
  // which doesn't depend on anything and which we want to place last
  // for performance reasons (very rarely used).
  UpdateOnHandreset();
}

CSymbolEngineRaisers::~CSymbolEngineRaisers() {
}

void CSymbolEngineRaisers::InitOnStartup() {
  UpdateOnConnection();
}

void CSymbolEngineRaisers::UpdateOnConnection() {
	UpdateOnHandreset();
}

void CSymbolEngineRaisers::UpdateOnHandreset() {
	for (int i=kBetroundPreflop; i<=kBetroundRiver; i++) {
		_raisbits[i] = 0;
    _lastraised[i] = kUndefined;
	}
	_raischair = kUndefined;
	_firstraiser_chair = kUndefined;
	_nopponentstruelyraising = 0;
}

void CSymbolEngineRaisers::UpdateOnNewRound() {
	_firstraiser_chair = kUndefined;
}

void CSymbolEngineRaisers::UpdateOnMyTurn() {
}

void CSymbolEngineRaisers::UpdateOnHeartbeat() {
  CalculateRaisers();
}

int CSymbolEngineRaisers::ChairInFrontOfFirstPossibleActor() {
  if (p_symbol_engine_userchair->userchair_confirmed()) {
    if (p_symbol_engine_autoplayer->ismyturn()) {
      // userchair known and my turn
      // * a full orbit when it is our turn
      return p_symbol_engine_userchair->userchair();
    } else if (p_symbol_engine_history->DidAct()) {
      // userchair known and not my turn, but did act
      // * a partial orbit behind us if we already acted 
      //   (similar to RaisesSinceLastPlay, but might include a bettor)
      return p_symbol_engine_userchair->userchair();
    } else {
      // userchair known and not my turn, did not yetact
      // * a partial orbit from dealer to hero if we did not yet act
      return p_symbol_engine_dealerchair->dealerchair();
    }
  } else {
    // user-chair unknown
    // * an orbit after the dealer if the userchair is unknown
    //   (not really usable for OpenPPL which updates at our turn, 
    //   but at least somewhat meaningful in the debug-tab).
    return p_symbol_engine_dealerchair->dealerchair();
  }
}

int CSymbolEngineRaisers::FirstPossibleActor() {
  return (ChairInFrontOfFirstPossibleActor() + 1);
}

int CSymbolEngineRaisers::LastPossibleActor() {
  int result = kUndefined;
  if (p_symbol_engine_userchair->userchair_confirmed()) {
    if (p_symbol_engine_autoplayer->ismyturn()) {
      // userchair known and my turn
      // * a full orbit when it is our turn
      result = p_symbol_engine_userchair->userchair();
    } else if (p_symbol_engine_history->DidAct()) {
      // userchair known and not my turn, but did act
      // * a partial orbit behind us if we already acted 
      //   (similar to RaisesSinceLastPlay, but might include a bettor)
      result = p_symbol_engine_userchair->userchair();
    } else {
      // userchair known and not my turn, did not yetact
      // * a partial orbit from dealer to hero if we did not yet act
      result = p_symbol_engine_userchair->userchair();
    }
  } else {
    // user-chair unknown
    // * an orbit after the dealer if the userchair is unknown
    //   (not really usable for OpenPPL which updates at our turn, 
    //   but at least somewhat meaningful in the debug-tab).
    result = p_symbol_engine_dealerchair->dealerchair();
  }
  int first_possible_actor = FirstPossibleActor();  
  if (result < first_possible_actor) {
    // Make sure tat our simple for-loops don't terminate too early
    // when searching for raisers
    int nchairs = p_tablemap->nchairs();
    result += nchairs;
  }
  return result;
}

double CSymbolEngineRaisers::MinimumStartingBetCurrentOrbit(bool searching_for_raisers) {
	// Not yet acted: 0 or 1 bb for the first orbit
	if (!p_symbol_engine_history->DidAct()) {
    if (p_betround_calculator->betround() == kBetroundPreflop) {
      if (searching_for_raisers) {
        // Preflop:
        // Start with big blind and forget about WinHoldem "blind raisers".
        return p_symbol_engine_tablelimits->bblind();
      } else {
        // When searching for callers start with 0 big blinds
        // to avoid recognizing the big-blind as a caller.
        // (true, we could change the starting position
        // of the search, but this would cause troubles heradsup).
        return 0.0;
      }
    } else {
      // Postflop
		  return 0.0;
    }
	}
  int last_known_actor = ChairInFrontOfFirstPossibleActor();
  int nchairs = p_tablemap->nchairs();
  AssertRange(last_known_actor, 0, (nchairs - 1));
	return p_table_state->Player(last_known_actor)->_bet.GetValue();
}

void CSymbolEngineRaisers::CalculateRaisers() {
	_nopponentstruelyraising = 0;
  _temp_raisbits_current_orbit = 0;
  if (p_symbol_engine_chip_amounts->call() <= 0.0) {
    // There are no bets and raises.
    // Skip the calculations to keep the raischair of the previous round.
    // http://www.maxinmontreal.com/forums/viewtopic.php?f=156&t=16806
    write_log(preferences.debug_symbolengine(),
      "[CSymbolEngineRaisers] No bet to call, therefore no raises\n");
    return;
  }
	int first_possible_raiser = FirstPossibleActor();
  p_symbol_engine_debug->SetValue(0, first_possible_raiser);
	int last_possible_raiser  = LastPossibleActor();
  p_symbol_engine_debug->SetValue(1, last_possible_raiser);
  assert(last_possible_raiser > first_possible_raiser);
  assert(p_symbol_engine_debug != NULL);
	double highest_bet = MinimumStartingBetCurrentOrbit(true);
  p_symbol_engine_debug->SetValue(2, highest_bet);
  write_log(preferences.debug_symbolengine(), "[CSymbolEngineRaisers] Searching for raisers from chair %i to %i with a bet higher than %.2f\n",
		first_possible_raiser, last_possible_raiser, highest_bet); 
	for (int i=first_possible_raiser; i<=last_possible_raiser; ++i) {
		int chair = i % p_tablemap->nchairs();
		double current_players_bet = p_table_state->Player(chair)->_bet.GetValue();
    write_log(preferences.debug_symbolengine(), 
      "[CSymbolEngineRaisers] chair %d bet %.2f\n",
      chair, current_players_bet);
		// Raisers are people
		// * with a higher bet than players before them
		// * who are still playing, not counting people who bet/fold in later orbits
    // * either betting/raising postflop or truely raising preflop
    //   (not counting the infamous "blind raisers")
    if (!p_table_state->Player(chair)->HasAnyCards()) {
      write_log(preferences.debug_symbolengine(), 
        "[CSymbolEngineRaisers] chair %d has no cards.\n", chair);
      continue;
    } else if (current_players_bet <= highest_bet) {
      write_log(preferences.debug_symbolengine(), 
        "[CSymbolEngineRaisers] chair %d is not raising\n", chair);
      continue;
    } else if ((p_betround_calculator->betround() == kBetroundPreflop)
				&& (current_players_bet <= p_symbol_engine_tablelimits->bblind())) {
      write_log(preferences.debug_symbolengine(), 
        "[CSymbolEngineRaisers] chair %d so-called \"blind raiser\". To be ignored.\n", chair);
      continue;
    } else if ((p_betround_calculator->betround() == kBetroundPreflop)
      && p_table_state->Player(chair)->PostingBothBlinds()) {
      write_log(preferences.debug_symbolengine(), 
        "[CSymbolEngineRaisers] chair %d is posting both blinds at once. To be ignored.\n", chair);
      continue;
    }
		highest_bet = current_players_bet;
    p_symbol_engine_debug->SetValue(3, highest_bet);
    write_log(preferences.debug_symbolengine(), "[CSymbolEngineRaisers] Opponent %i raising to %s\n",
			chair, Number2CString(highest_bet));
    // Updating some other symbols
    // Be easy with symbols that get calculated every turn,
    // they can easily be repared...
    ++_nopponentstruelyraising;
    _raischair = chair;
    p_symbol_engine_debug->SetValue(4, _raischair);
    assert(chair != USER_CHAIR);
    if (_firstraiser_chair == kUndefined) {
      _firstraiser_chair = chair;
      p_symbol_engine_debug->SetValue(5, _firstraiser_chair);
    }
    AssertRange(_raischair, kUndefined, kLastChair);
    _lastraised[BETROUND] = _raischair;
    // We have to be very careful
    // if we accumulate info based on dozens of unstable frames
    // when it is not our turn and the casino potentially
    // updates its display, causing garbabe input that sums up.
    // This affects raisbits, callbits, foldbits.
    // Special fail-safe-code for raisbits: 
    // We calcuklate stable raisbits and unstable temp_raisbits.
    // When it is our turn we update the stable data.
    // Completeness of historical info of previous betrounds
    // is guaranteed, as after an opponents raise
    // it will be our turn for the update once more again
    // (at least as long as we are playing, and that's all that matters).
    _temp_raisbits_current_orbit |= k_exponents[chair];
    if (p_symbol_engine_autoplayer->ismyturn()) {
      _raisbits[BETROUND] |= _temp_raisbits_current_orbit;
    }
	}
	write_log(preferences.debug_symbolengine(), "[CSymbolEngineRaisers] nopponentstruelyraising: %i\n", _nopponentstruelyraising);
	write_log(preferences.debug_symbolengine(), "[CSymbolEngineRaisers] raischair: %i\n", _raischair);
	write_log(preferences.debug_symbolengine(), "[CSymbolEngineRaisers]  firstraiser_chair (empty if no raiser) : %i\n", _firstraiser_chair);
}

int CSymbolEngineRaisers::raisbits(int betround) {
  AssertRange(betround, kBetroundPreflop, kBetroundRiver);
  if (betround == BETROUND) {
    // Compute the result based on known good data and temp data
    // (potentially unstable, but best what we have)
    return (_raisbits[betround] | _temp_raisbits_current_orbit);
  }
  if ((betround >= kBetroundPreflop)
      && (betround <= kBetroundRiver)) {
    // Return stable historical data
    return _raisbits[betround];
  } else {
    // index out of range, can happen e.g. for bad user-DLLs
    return kUndefined;
  }
}

int CSymbolEngineRaisers::LastRaised(const int round) {
  AssertRange(round, kBetroundPreflop, kBetroundRiver);
  return _lastraised[round];
}

bool CSymbolEngineRaisers::EvaluateSymbol(const char *name, double *result, bool log /* = false */) {
  FAST_EXIT_ON_OPENPPL_SYMBOLS(name);
	if (memcmp(name, "nopponents", 10)==0) {
		if (memcmp(name, "nopponentstruelyraising", 23)==0 && strlen(name)==23) {
			*result = nopponentstruelyraising();
		}	else {
			// Invalid symbol
			return false;
		}
		// Valid symbol
		return true;
	} else if (memcmp(name, "raischair", 9) == 0 && strlen(name) == 9) {
		*result = raischair();
	} else if (memcmp(name, "firstraiser_chair", 17) == 0) {
		*result = firstraiser_chair();
	}	else if (memcmp(name, "raisbits", 8)==0 && strlen(name)==9) {
		*result = raisbits(name[8]-'0');
	} else if (memcmp(name, "lastraised", 10)==0 && strlen(name)==11) { 
    *result = LastRaised(name[10]-'0');
  }	else {
		// Symbol of a different symbol-engine
		return false;
	}
	// Valid symbol
	return true;
}

CString CSymbolEngineRaisers::SymbolsProvided() {
  CString list = "nopponentstruelyraising raischair firstraiser_chair  ";
  list += RangeOfSymbols("raisbits%i", kBetroundPreflop, kBetroundRiver);
  list += RangeOfSymbols("lastraised%i", kBetroundPreflop, kBetroundRiver);
  return list;
}
