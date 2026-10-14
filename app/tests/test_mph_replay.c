/* SPDX-License-Identifier: Apache-2.0 */
/* Replay captured mouse deltas through the mapper; no phone interaction. */
#include "mph_mapper.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
static unsigned actions[3];
static double px,py,travel_x,travel_y;
static bool output(void *unused,unsigned id,enum mph_action a,float x,float y) {
    (void)unused;assert(id==1);
    if(a==MPH_MOVE) {travel_x+=x-px;travel_y+=y-py;}
    px=x;py=y;++actions[a];return true;
}
int main(int argc,char **argv) {
    assert(argc==2);FILE *f=fopen(argv[1],"r");assert(f);
    struct mph_mapper m;mph_init(&m);m.emit=output;m.look_x=.686747f;m.look_y=.325866f;
    char line[1024];double peak=0,expected_x=0,expected_y=0;unsigned rows=0,stalls=0;
    while(fgets(line,sizeof(line),f)) {
        unsigned long long t;unsigned count;float dx,dy;
        if(sscanf(line,"%llu,%u,%f,%f,",&t,&count,&dx,&dy)!=4) continue;
        if(count) mph_look(&m,dx,dy,2700.f/1228);
        expected_x+=dx*.001/(2700./1228);expected_y+=dy*.001;
        unsigned before=actions[MPH_MOVE];mph_look_tick(&m,2700.f/1228);
        if(count && before==actions[MPH_MOVE]) ++stalls;
        peak=fmax(peak,fabs(m.look_pending_x)*2699);++rows;
    }
    fclose(f);
    for(unsigned i=0;i<2000 && (fabsf(m.look_pending_x)>1e-7 || fabsf(m.look_pending_y)>1e-7);++i) mph_look_tick(&m,2700.f/1228);
    assert(fabs(travel_x-expected_x)<1e-4 && fabs(travel_y-expected_y)<1e-4);
    printf("rows=%u reanchors=%u ticks_with_mouse_but_no_move=%u peak_pending_x_px=%.1f total_delta_preserved=1\n",rows,m.look_resets,stalls,peak);
    return 0;
}
