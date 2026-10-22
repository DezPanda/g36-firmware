/* SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda */
/* Button input for the G36 menu (evdev). */
#ifndef G36_INPUT_H
#define G36_INPUT_H

typedef struct
{
   int code;    /* linux input key code */
   int value;   /* 1 = press, 0 = release, 2 = repeat */
} key_event_t;

int  input_open(void);
/* Waits up to timeout_ms for input; returns 1 and fills ev if an event is available. */
int  input_next(key_event_t *ev, int timeout_ms);
int  input_held(int code);
void input_close(void);

#endif
