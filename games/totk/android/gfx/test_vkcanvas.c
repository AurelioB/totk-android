/* Pure tracker tests; no GPU, game, allocation, or native-UI dependency. */
#include "vk_canvas.h"
#include <stdio.h>
#include <stdlib.h>
static int checks,failures;
#define CHECK(c,s) do { checks++; if(!(c)){failures++;printf("FAIL: %s\n",s);} } while(0)
int main(int argc,char **argv) {
    int native=argc>1?atoi(argv[1]):0,fg=argc>2?atoi(argv[2]):1,aspect=argc>3?atoi(argv[3]):0;
    vk_canvas_init(native,fg,aspect);
    vk_canvas_image canvas={.image=1,.incarnation=1,.storage=0x10000,.size=1280*720*4,.serial=1,.width=1280,.height=720,.format=122};
    vk_canvas_image window={.image=2,.incarnation=2,.storage=0x1000000,.size=1920*1080*4,.serial=2,.width=1920,.height=1080,.format=37};
    CHECK(vk_canvas_on==(native||fg||aspect),"feature gate admits framegen and the HUD aspect squeeze independently of native UI");
    vk_canvas_transfer(&canvas,&window,0);
    if(!vk_canvas_on) {
        CHECK(!vk_canvas_current()->canvas.image,"disabled tracker retains no canvas state");
        CHECK(!vk_canvas_first_ui(&canvas,"7f2113e2"),"disabled UI boundary is inert");
        CHECK(!vk_canvas_present(&window),"disabled present chain is inert");
    } else {
        CHECK(!vk_canvas_first_ui(&canvas,"78eb7b52"),"scene composite is not first UI");
        vk_canvas_image other=canvas; other.storage++;
        CHECK(!vk_canvas_first_ui(&other,"7f2113e2"),"equal size and image with wrong storage is not canvas");
        CHECK(vk_canvas_first_ui(&canvas,"7f2113e2"),"exact known canvas nominates first UI");
        CHECK(vk_canvas_first_ui(&canvas,"7f2113e2"),"preparation is idempotent for both consumers");
        CHECK(vk_canvas_in_ui(&canvas),"known canvas enters UI interval");
        CHECK(!vk_canvas_present(&window),"unemitted terminal transfer is not a verified chain");
        other=canvas; other.serial=100;
        vk_canvas_ui_commit(&other);
        CHECK(vk_canvas_current()->ui_committed,"descriptor serial aliases share exact storage incarnation");
        CHECK(!vk_canvas_first_ui(&canvas,"7f2113e2"),"committed first UI is not repeated");
        vk_canvas_transfer(&canvas,&window,1);
        other=window; other.image=3;
        CHECK(!vk_canvas_present(&other),"wrong presented window cannot verify transfer");
        CHECK(vk_canvas_present(&window),"committed transfer reaches exact present identity");
        CHECK(vk_canvas_current()->presented&&vk_canvas_current()->canvas.width==1280&&vk_canvas_current()->window.width==1920,
            "active canvas and padded window retain separate extents");
        vk_canvas_transfer(&canvas,&window,0);
        CHECK(!vk_canvas_present(&window),"a later unemitted transfer invalidates earlier same-frame authority");
        vk_canvas_frame_end();
        CHECK(!vk_canvas_current()->first_ui&&!vk_canvas_current()->transfer_committed,"real-frame end clears event facts");
        CHECK(vk_canvas_first_ui(&canvas,"7f2113e2"),"same backing is recognized next frame independently of DRS");
        other=canvas; other.incarnation++;
        CHECK(!vk_canvas_first_ui(&other,"7f2113e2"),"recycled image/storage with new lifetime requires rediscovery");
        other=canvas; other.level=1;
        CHECK(!vk_canvas_first_ui(&other,"7f2113e2"),"mip identity cannot alias base canvas boundary");
    }
    CHECK(vk_canvas_transfer_shader("dd3d1e0ebf73242845970b5655fac199a30f5acf747975c5e771caa49af8b12b-b7530f698e76"),"exact final-transfer shader is recognized");
    CHECK(!vk_canvas_transfer_shader("dd3d1e0e"),"a truncated shader prefix is not transfer authority");
    printf("%s: canvas native=%d framegen=%d aspect=%d %d checks, %d failed\n",failures?"FAILED":"ok",native,fg,aspect,checks,failures);
    return failures!=0;
}
