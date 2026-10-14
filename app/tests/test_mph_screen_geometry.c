/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real geometry code with pre-video dimensions. */
#include "screen.h"
#include <assert.h>
#include "test_geometry_impl.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    SDL_FRect r;
    for (int mode=SC_RENDER_FIT_LETTERBOX;mode<=SC_RENDER_FIT_UNSCALED;++mode) {
        compute_content_rect((struct sc_size){1000,600},(struct sc_size){0,0},false,mode,&r);
        assert(r.w==0 && r.h==0);
        compute_content_rect((struct sc_size){1000,600},(struct sc_size){0,1200},false,mode,&r);
        assert(r.w==0 && r.h==0);
        compute_content_rect((struct sc_size){1000,600},(struct sc_size){2000,0},false,mode,&r);
        assert(r.w==0 && r.h==0);
    }
    compute_content_rect((struct sc_size){1000,600},(struct sc_size){2000,1200},false,SC_RENDER_FIT_LETTERBOX,&r);
    assert(r.w==1000 && r.h==600);
    puts("Screen geometry: PASS (pre-video zero dimensions and normal aspect ratio)");
}
