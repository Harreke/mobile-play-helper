/* SPDX-License-Identifier: Apache-2.0 */
#include "mph_mapper.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static bool active[MPH_CONTACTS];
static unsigned downs, ups, moves;
static bool reject;
static float look_x,look_y,moved_x,moved_y;
static unsigned emitted;
static unsigned reject_call;
static unsigned calls;
static bool emit(void *unused, unsigned id, enum mph_action a,float x,float y) {
    (void)unused;
    assert(x>=0 && x<=1 && y>=0 && y<=1);
    ++calls;
    if(reject || (reject_call && calls==reject_call)) return false;
    ++emitted;
    if(id==1) {
        if(a==MPH_MOVE) {moved_x+=x-look_x;moved_y+=y-look_y;}
        look_x=x;look_y=y;
    }
    if(a==MPH_DOWN) { assert(!active[id]); active[id]=true; ++downs; }
    else if(a==MPH_UP) { assert(active[id]); active[id]=false; ++ups; }
    else { assert(active[id]); ++moves; }
    return true;
}
static void drain(struct mph_mapper *m,float aspect) {
    for(unsigned i=0;i<2000 && (fabsf(m->look_pending_x)>1e-7f || fabsf(m->look_pending_y)>1e-7f);++i) {
        unsigned before=emitted;mph_look_tick(m,aspect);
        assert(emitted-before<=1); /* cannot burst UP/DOWN/MOVE in one tick */
        if(m->contacts[1].down) {
            assert(m->contacts[1].x>=0 && m->contacts[1].x<=1);
            assert(m->contacts[1].y>=0 && m->contacts[1].y<=1);
        }
    }
    assert(fabsf(m->look_pending_x)<=1e-7f && fabsf(m->look_pending_y)<=1e-7f);
}
int main(void) {
    struct mph_mapper m; mph_init(&m); m.emit=emit;
    m.bindings[0]=(struct mph_binding){10,.8f,.8f};m.count=1;
    mph_direction(&m,0,true,2); mph_direction(&m,3,true,2);
    float dx=(m.contacts[0].x-m.joy_x)*2,dy=m.contacts[0].y-m.joy_y;
    assert(fabsf(hypotf(dx,dy)-m.joy_radius)<.00001f); /* diagonal normalization */
    mph_look(&m,12,-5,2); drain(&m,2); mph_button(&m,10,true);
    assert(active[0] && active[1] && active[2]);
    unsigned d=downs; mph_button(&m,10,true); assert(downs==d); /* repeat */
    mph_direction(&m,0,false,2); assert(active[0]); /* D still held */
    mph_direction(&m,1,true,2); assert(!active[0]); /* A+D neutral */
    mph_direction(&m,1,false,2); assert(active[0]);
    mph_look(&m,1000,500,2); drain(&m,2); /* repeated re-anchoring */
    assert(active[0] && active[1] && active[2]);
    assert(downs>d); assert(mph_release(&m)); assert(downs==ups);
    for(unsigned i=0;i<MPH_BINDINGS;++i) m.bindings[i]=(struct mph_binding){(int)i+20,.5f,.5f};
    m.count=MPH_BINDINGS;
    for(unsigned i=0;i<MPH_BINDINGS;++i) mph_button(&m,i+20,true);
    unsigned n=0; for(unsigned i=0;i<MPH_CONTACTS;++i)n+=active[i]; assert(n==10);
    assert(mph_release(&m)); assert(downs==ups);
    reject=true; mph_button(&m,20,true); assert(!m.contacts[2].down);
    reject=false; mph_button(&m,20,true); reject=true;
    assert(!mph_release(&m)); assert(m.contacts[2].down); /* retry UP */
    reject=false; assert(mph_release(&m));assert(downs==ups);
    mph_init(&m);m.emit=emit;moved_x=0;moved_y=0;
    unsigned before=emitted;
    mph_look(&m,1000,-600,2);assert(emitted==before); /* raw mouse produces no burst */
    drain(&m,2);
    assert(fabsf(moved_x-.5f)<1e-5f && fabsf(moved_y+.6f)<1e-5f);
    assert(m.look_resets>0 && !m.look_overflows);
    assert(mph_release(&m));
    /* One injected failure per phase must preserve the remaining displacement. */
    for(unsigned failure=1;failure<=15;++failure) {
        mph_init(&m);m.emit=emit;moved_x=0;moved_y=0;calls=0;reject_call=failure;
        mph_look(&m,900,100,2);drain(&m,2);
        assert(fabsf(moved_x-.45f)<1e-5f && fabsf(moved_y-.1f)<1e-5f);
        reject_call=0;assert(mph_release(&m));
    }
    mph_init(&m);m.emit=emit;moved_x=0;moved_y=0;
    /* Long swipes and reversals across many boundaries retain total movement. */
    for(int i=0;i<300;++i) {mph_look(&m,i<200?40:-40,0,2);drain(&m,2);}
    assert(fabsf(moved_x-2.0f)<.0001f);
    mph_look(&m,1e30f,1e30f,2);assert(m.look_overflows==1);
    assert(isfinite(m.look_pending_x));
    assert(mph_release(&m));assert(!m.look_pending_x && !m.look_pending_y);
    before=emitted;mph_look_tick(&m,2);assert(emitted==before); /* no drift after F1 */
    /* Regression: every initial/replacement DOWN uses the configured center. */
    mph_init(&m);m.emit=emit;
    mph_look(&m,1000,0,2);
    for(unsigned i=0;i<100 && m.look_pending_x>1e-7f;++i) {
        unsigned old_downs=downs;mph_look_tick(&m,2);
        if(downs!=old_downs) assert(m.contacts[1].x==m.look_x && m.contacts[1].y==m.look_y);
    }
    assert(m.look_resets>0);assert(mph_release(&m));
    /* Manual refresh must work at rest, preserve other fingers and retry failures. */
    mph_init(&m);m.emit=emit;
    m.bindings[0]=(struct mph_binding){10,.8f,.8f};m.count=1;
    mph_direction(&m,0,true,2);mph_button(&m,10,true);
    before=emitted;mph_look_refresh(&m);
    assert(emitted==before+1 && active[1] && !m.look_refresh_phase);
    before=emitted;mph_look_refresh(&m);
    assert(emitted==before+2 && active[1] && active[0] && active[2]);
    mph_look(&m,10,5,2);reject=true;mph_look_refresh(&m);
    assert(!m.look_pending_x && !m.look_pending_y);
    reject=true;mph_look_tick(&m,2);
    assert(active[1] && m.look_refresh_phase==1);
    reject=false;before=emitted;mph_look_tick(&m,2);
    assert(emitted==before+1 && !active[1] && active[0] && active[2]);
    reject=true;mph_look_tick(&m,2);assert(m.look_refresh_phase==2 && !active[1]);
    reject=false;before=emitted;mph_look_tick(&m,2);
    assert(emitted==before+1 && active[1] && !m.look_refresh_phase);
    assert(look_x==m.look_x && look_y==m.look_y);
    before=emitted;mph_look_tick(&m,2);assert(emitted==before);
    mph_look_refresh(&m);assert(mph_release(&m));
    before=emitted;mph_look_tick(&m,2);assert(emitted==before);
    printf("MPH mapper: PASS (diagonal/opposites/multitouch/reanchor/repeat/limit/failure/release), %u moves\n",moves);
}
