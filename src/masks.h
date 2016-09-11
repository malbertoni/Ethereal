/*
  Ethereal is a UCI chess playing engine authored by Andrew Grant.
  <https://github.com/AndyGrant/Ethereal>     <andrew@grantnet.us>
  
  Ethereal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
  
  Ethereal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _MASKS_H
#define _MASKS_H

#include <stdint.h>

#include "types.h"

void initalizeMasks();

extern uint64_t IsolatedPawnMasks[SQUARE_NB];
extern uint64_t PassedPawnMasks[COLOUR_NB][SQUARE_NB];
extern uint64_t PawnAttackMasks[COLOUR_NB][SQUARE_NB];
extern uint64_t PawnAdvanceMasks[COLOUR_NB][SQUARE_NB];
extern uint64_t PawnConnectedMasks[COLOUR_NB][SQUARE_NB];
extern uint64_t OutpostSquareMasks[COLOUR_NB][SQUARE_NB];
extern uint64_t OutpostRanks[COLOUR_NB];

#endif
