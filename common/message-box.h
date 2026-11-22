#pragma once

typedef struct
{
    uint32_t width;
    uint32_t height;
    color_t *bg;
    char *text;
    void (*confirm)(void *arg); // Set for a box that asks A to confirm or B to cancel.
    void *confirmArg;
} MessageBox;

void menuCreateMsgBox(int width, int height, const char *text);
// A box with two choices: A runs confirm(arg) after closing the box, B closes it.
void menuCreateConfirmMsgBox(int width, int height, const char *text, void (*confirm)(void *arg), void *arg);
void menuCloseMsgBox();
bool menuIsMsgBoxOpen();
bool menuIsConfirmMsgBoxOpen(void);
// Closes a confirmation box and runs its confirm function.
void menuConfirmMsgBox(void);
void menuDrawMsgBox(void);
MessageBox menuGetCurrentMsgBox();
void menuMsgBoxSetNetloaderState(bool enabled, const char *text, bool enable_progress, float progress);
