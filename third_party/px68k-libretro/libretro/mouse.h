#ifndef _WINX68K_MOUSE_H
#define _WINX68K_MOUSE_H

#include <stdint.h>
#include "common.h"

void Mouse_Init(void);
void Mouse_Event(int wparam, float dx, float dy);
void Mouse_SetData(void);
void Mouse_StartCapture(int flag);
/* Movement not yet handed to the guest.  Mouse_SetData keeps the overflow
 * that does not fit into one SCC report, so relative moves are never lost. */
void Mouse_GetPending(int *dx, int *dy);
void Mouse_ClearPending(void);
void Mouse_Reset(void);

#endif /* _WINX68K_MOUSE_H */
