#include "logic/core/global_logic.h"

/* Default global logic: intentionally does nothing. */
static void global_default(GlobalContext *gctx)
{
    (void)gctx;
}

REGISTER_GLOBAL_LOGIC(global_default);
