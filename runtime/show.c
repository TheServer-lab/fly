#include "flyrt.h"
#include <stdio.h>

// spec §10.3 interpolation-conversion table doubles as show()'s formatting
// rule for each tag. tex's payload is a FlyText* (runtime/text.c) as of
// milestone 4 -- no longer a raw C string pointer -- so this goes through
// fly_rt_text_data() rather than casting the payload directly.
void fly_rt_show(FlyValue v) {
    switch (v.tag) {
        case FLY_NUM: printf("%lld\n", (long long)v.payload); break;
        case FLY_DEC: printf("%g\n", fly_rt_as_dec(v)); break;
        case FLY_YN:  printf("%s\n", v.payload ? "Yes" : "No"); break;
        case FLY_EMP: printf("EMP\n"); break;
        case FLY_TEX: printf("%s\n", fly_rt_text_data(v)); break;
        case FLY_COLL:
        case FLY_BOARD: {
            FlyValue asTex = fly_rt_to_text(v); // §10.3's standard coll/board representation
            printf("%s\n", fly_rt_text_data(asTex));
            fly_rt_release(asTex);
            break;
        }
        default:      printf("<unknown fly value tag=%d>\n", v.tag); break;
    }
}
