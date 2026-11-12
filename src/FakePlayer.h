#ifndef CC_FAKEPLAYER_H
#define CC_FAKEPLAYER_H
#include "Core.h"
CC_BEGIN_HEADER

/* Client-side fake player: /client fakeplayer
   Spawns an entity that shows up like a real player (including in the tab list),
   can use a Blockbench .bbmodel as its model, and can mimic the local player.
*/
struct ChatCommand;
extern struct ChatCommand FakePlayerCommand;

/* Called when the commands component is initialised/freed */
void FakePlayer_Init(void);
void FakePlayer_Free(void);

CC_END_HEADER
#endif
