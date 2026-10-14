/* SPDX-License-Identifier: Apache-2.0 */
/* MobilePlayHelper additions, Apache-2.0. */
#ifndef MPH_MAPPER_H
#define MPH_MAPPER_H
#include <stdbool.h>
#define MPH_BINDINGS 32
#define MPH_CONTACTS (MPH_BINDINGS + 2)
enum mph_action { MPH_DOWN, MPH_UP, MPH_MOVE };
struct mph_binding { int key; float x, y; };
struct mph_contact { bool down; float x, y; };
struct mph_mapper {
    float joy_x, joy_y, joy_radius, look_x, look_y, look_radius, sensitivity;
    struct mph_binding bindings[MPH_BINDINGS];
    unsigned count;
    int toggle_key;
    bool directions[4];
    float look_pending_x, look_pending_y;
    unsigned look_refresh_phase;
    unsigned look_resets, look_rejected, look_overflows;
    struct mph_contact contacts[MPH_CONTACTS];
    bool (*emit)(void *, unsigned, enum mph_action, float, float);
    void *userdata;
};
void mph_init(struct mph_mapper *m);
bool mph_release(struct mph_mapper *m);
void mph_direction(struct mph_mapper *m, unsigned direction, bool down, float aspect);
void mph_button(struct mph_mapper *m, int key, bool down);
void mph_look(struct mph_mapper *m, float dx, float dy, float aspect);
void mph_look_tick(struct mph_mapper *m, float aspect);
void mph_look_refresh(struct mph_mapper *m);
#endif
