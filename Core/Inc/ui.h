#ifndef UI_H
#define UI_H

#include "can_capture.h"

void Ui_Init(void);
void Ui_SubmitFrame(const CanFrameRecord *frame);
void Ui_ResetFrameHistory(void);
void Ui_TaskLoop(void);

#endif
