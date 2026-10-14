/* SPDX-License-Identifier: Apache-2.0 */
#include "mph_mapper.h"
#include <math.h>
#include <string.h>

void mph_init(struct mph_mapper *m) {
    memset(m, 0, sizeof(*m));
    m->joy_x = .16f; m->joy_y = .76f; m->joy_radius = .10f;
    m->look_x = .65f; m->look_y = .45f; m->look_radius = .20f;
    m->sensitivity = .001f;
    m->toggle_key = 58; /* SDL physical F1, fixed mapping toggle */
}

static bool touch(struct mph_mapper *m, unsigned id, enum mph_action a,
                  float x, float y) {
    struct mph_contact *c = &m->contacts[id];
    if (a == MPH_DOWN) {
        unsigned active = 0;
        for (unsigned i = 0; i < MPH_CONTACTS; ++i) active += m->contacts[i].down;
        if (active >= 10 || c->down) return false;
    } else if (!c->down) return true;
    x = fmaxf(0, fminf(1, x)); y = fmaxf(0, fminf(1, y));
    if (!m->emit(m->userdata, id, a, x, y)) return false;
    *c = (struct mph_contact) {a != MPH_UP, x, y};
    return true;
}

bool mph_release(struct mph_mapper *m) {
    m->look_refresh_phase=0;
    m->look_pending_x=0;m->look_pending_y=0;
    bool ok = true;
    for (unsigned i = 0; i < MPH_CONTACTS; ++i) {
        struct mph_contact *c = &m->contacts[i];
        if (c->down && !touch(m, i, MPH_UP, c->x, c->y)) ok = false;
    }
    memset(m->directions, 0, sizeof(m->directions));
    return ok;
}

void mph_direction(struct mph_mapper *m, unsigned direction, bool down, float aspect) {
    if (direction >= 4 || aspect <= 0) return;
    m->directions[direction] = down;
    float x = (int)m->directions[3] - (int)m->directions[1];
    float y = (int)m->directions[2] - (int)m->directions[0];
    float length = sqrtf(x*x + y*y);
    struct mph_contact *c = &m->contacts[0];
    if (!length) { touch(m, 0, MPH_UP, c->x, c->y); return; }
    if (!c->down && !touch(m, 0, MPH_DOWN, m->joy_x, m->joy_y)) return;
    touch(m, 0, MPH_MOVE, m->joy_x + x/length*m->joy_radius/aspect,
          m->joy_y + y/length*m->joy_radius);
}

void mph_button(struct mph_mapper *m, int key, bool down) {
    for (unsigned i = 0; i < m->count; ++i) {
        struct mph_binding *b = &m->bindings[i];
        if (b->key == key && m->contacts[i+2].down != down)
            touch(m, i+2, down ? MPH_DOWN : MPH_UP, b->x, b->y);
    }
}

/* Mouse events only accumulate deltas. The UI timer sends one touch event
 * per tick, so UP/DOWN/MOVE cannot collapse into a same-timestamp burst. */
void mph_look(struct mph_mapper *m, float dx, float dy, float aspect) {
    if ((!dx && !dy) || !isfinite(aspect) || aspect<=0 ||
        !isfinite(dx) || !isfinite(dy) || !isfinite(m->sensitivity) || m->sensitivity<=0) return;
    double x=m->look_pending_x+(double)dx*m->sensitivity/aspect;
    double y=m->look_pending_y+(double)dy*m->sensitivity;
    /* Bound recovery after an input flood, rather than rotating for seconds
     * after the mouse stops. Normal mouse samples are much smaller. */
    double limit=m->look_radius*8;
    double length=fmax(fabs(x)*aspect,fabs(y));
    if(length>limit && limit>0) {x*=limit/length;y*=limit/length;++m->look_overflows;}
    m->look_pending_x=(float)x;m->look_pending_y=(float)y;
}

void mph_look_refresh(struct mph_mapper *m) {
    m->look_pending_x=0;m->look_pending_y=0;
    m->look_refresh_phase=1;
    struct mph_contact *c=&m->contacts[1];
    if(c->down) {
        if(!touch(m,1,MPH_UP,c->x,c->y)) {++m->look_rejected;return;}
        ++m->look_resets;
    }
    m->look_refresh_phase=2;
    if(touch(m,1,MPH_DOWN,m->look_x,m->look_y)) m->look_refresh_phase=0;
    else ++m->look_rejected;
}

void mph_look_tick(struct mph_mapper *m, float aspect) {
    if(!isfinite(aspect) || aspect<=0 || !isfinite(m->look_radius) || m->look_radius<=0) return;
    struct mph_contact *c=&m->contacts[1];
    if(m->look_refresh_phase) {
        if(m->look_refresh_phase==1 && c->down) {
            if(touch(m,1,MPH_UP,c->x,c->y)) {
                m->look_refresh_phase=2;++m->look_resets;
            } else ++m->look_rejected;
        } else {
            if(touch(m,1,MPH_DOWN,m->look_x,m->look_y)) m->look_refresh_phase=0;
            else ++m->look_rejected;
        }
        return; /* Retry a failed manual refresh without losing contact state. */
    }
    float dx=m->look_pending_x,dy=m->look_pending_y;
    if(fabsf(dx)<.0000001f && fabsf(dy)<.0000001f) return;
    if(!c->down) {
        if(!touch(m,1,MPH_DOWN,m->look_x,m->look_y)) ++m->look_rejected;
        return; /* allow the game to observe the new finger before moving it */
    }
    float fraction=1;
    /* The configured point is the gesture START, not a hard travel limit.
     * Keep the finger down throughout the visible screen, consuming the whole
     * pending delta unless a physical edge is reached. */
    float left=0,right=1,top=0,bottom=1;
    if(dx>0) fraction=fminf(fraction,(right-c->x)/dx);
    else if(dx<0) fraction=fminf(fraction,(left-c->x)/dx);
    if(dy>0) fraction=fminf(fraction,(bottom-c->y)/dy);
    else if(dy<0) fraction=fminf(fraction,(top-c->y)/dy);
    if(fraction<=.00001f) {
        if(touch(m,1,MPH_UP,c->x,c->y)) ++m->look_resets;
        else ++m->look_rejected;
        return; /* next tick DOWN, following tick MOVE; keep remaining delta */
    }
    dx*=fraction;dy*=fraction;
    if(touch(m,1,MPH_MOVE,c->x+dx,c->y+dy)) {
        m->look_pending_x-=dx;m->look_pending_y-=dy;
    } else ++m->look_rejected; /* retry unsent distance on a later tick */
}
