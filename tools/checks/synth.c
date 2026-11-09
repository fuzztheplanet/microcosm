#include "microcosm/mc_audio.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    mc_synth *a=mc_synth_make(0xB16BA75),*b=mc_synth_make(0xB16BA75);assert(a&&b);
    float x[2048],y[2048],peak=0;int total=(int)(mc_synth_length(a)*MC_SAMPLE_RATE);
    /* Compare one bar of the first warning with one bar of the close stare.
     * The threat must build audibly from this warning, without a louder master
     * ceiling. Whole-song checks below still cover every other movement. */
    const double bar=240.0/124.0*MC_SAMPLE_RATE;
    double warning=0,threat=0;
    for(int at=0;at<total;at+=1024) {
        int n=total-at<1024?total-at:1024;
        mc_synth_render(a,x,n);mc_synth_render(b,y,n/2);mc_synth_render(b,y+(n/2)*2,n-n/2);
        assert(!memcmp(x,y,n*2*sizeof(float)));
        for(int i=0;i<n*2;++i) {
            uint32_t bits;memcpy(&bits,&x[i],sizeof bits);assert((bits&0x7f800000)!=0x7f800000);
            float v=x[i]<0?-x[i]:x[i];assert(v<=1);if(v>peak)peak=v;
            int frame=at+i/2;
            if(frame>=111*bar && frame<112*bar)warning+=(double)x[i]*x[i];
            if(frame>=114*bar && frame<115*bar)threat+=(double)x[i]*x[i];
        }
    }
    assert(warning>1 && threat>warning*2.25); /* at least 1.5 times the warning's RMS */
    mc_synth_free(a);mc_synth_free(b);printf("Synth: %d frames, exact block-size independence, finite and bounded (peak %.6f)\n",total,peak);
}
