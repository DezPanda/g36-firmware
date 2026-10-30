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
/* The buttons the Controls screen can move (A B X Y L R Select Start Menu): real[k] = the evdev code of the real button that acts as
 * the k-th of them (in that order) for the menu. NULL turns it off. input_set_raw(1) hands out the real buttons (for the
 * Controls screen itself). */
void input_set_map(const int *real);
void input_set_raw(int raw);
void input_close(void);

#endif
