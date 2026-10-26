#ifndef FRAMEGEN_TEST_CAMERA_H
#define FRAMEGEN_TEST_CAMERA_H
#include "../games/totk/android/gfx/framegen_math.h"
#include <string.h>

#define FG_TEST_W 320u
#define FG_TEST_H 180u
#define FG_TEST_NEAR 0.1
#define FG_TEST_FAR 1000.0

/* A real rigid camera with the same documented UBO layout as Metal. yaw is
   camera-to-world rotation about +Y, and camera looks down local -Z. */
static inline void fg_test_camera(float cam[84], double yaw, double x, double y, double z) {
    memset(cam, 0, 84 * sizeof *cam);
    double c = cos(yaw), s = sin(yaw);
    double pose[16] = {c,0,s,x, 0,1,0,y, -s,0,c,z, 0,0,0,1};
    double view[16] = {c,0,-s,-c*x+s*z, 0,1,0,-y, s,0,c,-s*x-c*z, 0,0,0,1};
    double n = FG_TEST_NEAR, f = FG_TEST_FAR;
    double projection[16] = {(double)FG_TEST_H/FG_TEST_W,0,0,0, 0,1,0,0,
        0,0,-(f+n)/(f-n),-2*f*n/(f-n), 0,0,-1,0};
    double vp[16]; fg_math_mul(projection, view, vp);
    for (int i = 0; i < 12; i++) { cam[i] = (float)view[i]; cam[44+i] = (float)pose[i]; }
    for (int i = 0; i < 16; i++) { cam[12+i] = (float)vp[i]; cam[28+i] = (float)projection[i]; }
    cam[56] = (float)n; cam[57] = (float)f; cam[62] = (float)FG_TEST_W/FG_TEST_H;
    cam[70] = 1.5707963267948966f; cam[72] = FG_TEST_W; cam[73] = FG_TEST_H;
}
static inline float fg_test_depth(double distance) {
    return (float)(FG_TEST_FAR/(FG_TEST_FAR-FG_TEST_NEAR) -
        FG_TEST_FAR*FG_TEST_NEAR/((FG_TEST_FAR-FG_TEST_NEAR)*distance));
}
static inline float fg_test_decode(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f : powf((encoded + 0.055f) / 1.055f, 2.4f);
}
#endif
