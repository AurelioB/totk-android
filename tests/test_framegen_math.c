/* Standalone CPU goldens: cc -std=c11 -O2 -Wall -Wextra -Werror
   tests/test_framegen_math.c -lm -o .scratch/interp/test-framegen-math */
#include "framegen_test_camera.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checks, failures;
#define CHECK(c, message) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL: %s (line %d)\n", message, __LINE__); } } while (0)
static void close_to(float actual, double expected, double tolerance, const char *message) {
    checks++;
    if (!isfinite(actual) || fabs(actual - expected) > tolerance) {
        failures++; fprintf(stderr, "FAIL: %s: %.9g expected %.12g\n", message, actual, expected);
    }
}
/* Rebuild only the projection-dependent fields after selecting a vertical
   focal scale. Keep the camera pose and clip convention from the fixture. */
static void test_set_focal(float camera[84], double focal, double near_plane, double far_plane) {
    double projection[16]={0},view[16],vp[16];
    projection[0]=(double)FG_TEST_H/FG_TEST_W*focal;
    projection[5]=focal;
    projection[10]=-(far_plane+near_plane)/(far_plane-near_plane);
    projection[11]=-2*far_plane*near_plane/(far_plane-near_plane);
    projection[14]=-1;
    for (unsigned i=0;i<12;i++) view[i]=camera[i];
    view[12]=view[13]=view[14]=0; view[15]=1;
    fg_math_mul(projection,view,vp);
    for (unsigned i=0;i<16;i++) { camera[28+i]=(float)projection[i]; camera[12+i]=(float)vp[i]; }
    camera[56]=(float)near_plane; camera[57]=(float)far_plane;
    camera[70]=(float)(2*atan(1/focal));
}
static void focal_interpolation(void) {
    const double p0=1.0, p1=1.04, pm=(p0+p1)*0.5;
    float camera[2][84],transform[2][16],uv[2];
    for (unsigned i=0;i<2;i++) fg_test_camera(camera[i],0,0,0,0);
    test_set_focal(camera[0],p0,0.1,1000);
    test_set_focal(camera[1],p1,0.1,1000);
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_OK,
          "smooth focal increase is interpolated");
    const double points[][2]={{0.2,0.25},{0.5,0.5},{0.78,0.7}};
    for (unsigned point=0;point<3;point++) for (unsigned side=0;side<2;side++) {
        /* Each source sample denotes this same world point, reconstructed
           independently from that source's projection and a two-unit depth. */
        const double focal=side?p1:p0, u0=points[point][0],v0=points[point][1];
        const double world_x=(2*u0-1)*2/(FG_TEST_H/(double)FG_TEST_W*p0);
        const double world_y=(1-2*v0)*2/p0;
        const double x=0.5+world_x*(FG_TEST_H/(double)FG_TEST_W*focal)/4;
        const double y=0.5-world_y*focal/4;
        const double expected_u=0.5+world_x*(FG_TEST_H/(double)FG_TEST_W*pm)/4;
        const double expected_v=0.5-world_y*pm/4;
        CHECK(fg_math_project(transform[side],(float)x,(float)y,fg_test_depth(2),uv),
              "FOV source point projects into midpoint");
        close_to(uv[0],expected_u,2e-6,"FOV midpoint horizontal projection is analytic");
        close_to(uv[1],expected_v,2e-6,"FOV midpoint vertical projection is analytic");
    }
    /* Reverse the zoom direction and vary clipping planes independently. */
    test_set_focal(camera[0],p1,1,25000); test_set_focal(camera[1],p0,0.1,5000);
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_OK,
          "smooth focal decrease with independent near/far is interpolated");
    for (unsigned side=0;side<2;side++) {
        const double focal=side?p0:p1, pm=(p0+p1)*0.5;
        const double world_x=(2*0.31-1)*2/(FG_TEST_H/(double)FG_TEST_W*pm);
        const double world_y=(1-2*0.64)*2/pm;
        const double x=0.5+world_x*(FG_TEST_H/(double)FG_TEST_W*focal)/4;
        const double y=0.5-world_y*focal/4;
        const double n=side?0.1:1,far=side?5000:25000;
        const float d=(float)(far/(far-n)-far*n/((far-n)*2));
        CHECK(fg_math_project(transform[side],(float)x,(float)y,d,uv),
              "reverse FOV source point projects with its own clip range");
        close_to(uv[0],0.31,2e-6,"reverse FOV horizontal analytic midpoint");
        close_to(uv[1],0.64,2e-6,"reverse FOV vertical analytic midpoint");
    }
    fg_test_camera(camera[0],-0.05,-0.12,0,0);
    fg_test_camera(camera[1],0.05,0.12,0,0);
    test_set_focal(camera[0],1.02,0.1,1000);
    test_set_focal(camera[1],0.98,0.1,1000);
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_OK,
          "smooth FOV combines with bounded translation and rotation");
    /* Inclusive contract boundaries: 3 degrees/tick and 1.10 scale ratio. */
    fg_test_camera(camera[0],0,0,0,0); fg_test_camera(camera[1],0,0,0,0);
    test_set_focal(camera[0],1,0.1,1000);
    test_set_focal(camera[1],tan(acos(-1.0)/4+acos(-1.0)/120),0.1,1000);
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_OK,
          "three-degree focal boundary is accepted");
    test_set_focal(camera[1],1.06,0.1,1000);
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_CUT,
          "FOV delta beyond three degrees is rejected");
    test_set_focal(camera[0],4,0.1,1000);
    test_set_focal(camera[1],4.4,0.1,1000);
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_OK,
          "1.10 focal-scale ratio boundary is accepted");
    test_set_focal(camera[1],4.40001,0.1,1000);
    for(unsigned s=0;s<2;s++) for(unsigned i=0;i<16;i++) transform[s][i]=321;
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_CUT,
          "focal ratio just beyond 1.10 is rejected");
    for(unsigned s=0;s<2;s++) for(unsigned i=0;i<16;i++)
        CHECK(transform[s][i]==321,"FOV rejection leaves both outputs unchanged");
    test_set_focal(camera[0],1,0.1,1000);
    test_set_focal(camera[1],1.01,0.1,1000);
    camera[1][62]*=1.001f;
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_CUT,
          "aspect field change is rejected");
    test_set_focal(camera[1],1.01,0.1,1000);
    camera[1][28]*=1.001f; camera[1][12]*=1.001f;
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_CUT,
          "effective p5/p0 aspect-ratio change is rejected");
    test_set_focal(camera[1],1.01,0.1,1000);
    camera[1][28]*=-1; camera[1][12]*=-1;
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_CUT,
          "projection lateral-scale sign flip remains rejected");
    test_set_focal(camera[1],1.01,0.1,1000); camera[1][72]+=1;
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_CUT,
          "FOV interpolation rejects camera size change");
    camera[1][72]=FG_TEST_W;
    test_set_focal(camera[1],1.01,0.1,1000);
    camera[1][30]+=0.01f; camera[1][14]+=0.01f;
    CHECK(fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                              transform[0],transform[1])==FG_MATH_CUT,
          "original off-center coefficient change remains rejected");
    test_set_focal(camera[1],1.01,0.1,1000);
    CHECK(fg_math_predict(camera[0],camera[1],FG_TEST_W,FG_TEST_H,transform[0])==FG_MATH_CUT,
          "prediction retains strict projection-change gate");
}
static void golden_uvs(void) {
    float current[84], previous[84], transform[16], uv[2];
    fg_test_camera(current, 0, 0, 0, 0); memcpy(previous, current, sizeof current);
    CHECK(fg_math_camera(current, FG_TEST_W, FG_TEST_H) == FG_MATH_OK, "valid Metal-layout camera");
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_OK, "identity pose prediction");
    const float points[][3] = {{0.5f,0.5f,0.4f}, {0.2f,0.8f,0.95f}, {0.75f,0.3f,0.99f}};
    for (unsigned i = 0; i < sizeof points / sizeof *points; i++) {
        CHECK(fg_math_project(transform, points[i][0], points[i][1], points[i][2], uv), "identity projects finite synthetic depth");
        close_to(uv[0], points[i][0], 1e-6, "identity golden u");
        close_to(uv[1], points[i][1], 1e-6, "identity golden v");
    }
    /* Current camera at origin, preceding at x=-0.16. Generated pose x=+0.08:
       a source point two units away shifts left by (height/width)*0.08/(2*2). */
    fg_test_camera(previous, 0, -0.16, 0, 0);
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_OK, "translation pose prediction");
    CHECK(fg_math_project(transform, 0.5f, 0.5f, fg_test_depth(2), uv), "translation projects source depth");
    close_to(uv[0], 0.48875, 2e-6, "translation golden u at z=2");
    close_to(uv[1], 0.5, 1e-6, "translation preserves v");
    CHECK(fg_math_project(transform, 0.5f, 0.5f, fg_test_depth(4), uv), "far translation projects");
    close_to(uv[0], 0.494375, 2e-6, "translation golden u at z=4");
    /* Vertical translation checks y-up camera to top-left texture conversion. */
    fg_test_camera(previous, 0, 0, -0.16, 0);
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_OK, "vertical pose prediction");
    CHECK(fg_math_project(transform, 0.5f, 0.5f, fg_test_depth(2), uv), "vertical translation projects");
    close_to(uv[1], 0.52, 2e-6, "vertical translation golden top-left v");
    /* Previous yaw -10 degrees -> predicted yaw +5 degrees, not entrywise VP
       extrapolation. A source point on the current optical axis moves right. */
    const double radians = 0.17453292519943295;
    fg_test_camera(previous, -radians, 0, 0, 0);
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_OK, "rotation pose prediction");
    CHECK(fg_math_project(transform, 0.5f, 0.5f, fg_test_depth(2), uv), "rotation projects source depth");
    close_to(uv[0], 0.5 + 0.5 * ((double)FG_TEST_H/FG_TEST_W) * tan(radians*0.5), 2e-6, "rigid rotation golden u");
    close_to(uv[1], 0.5, 1e-6, "rotation golden v");
    /* Large world offsets must cancel in double before GPU float constants. */
    fg_test_camera(current, 0.2, 4000, -1000, 3000);
    memcpy(previous, current, sizeof previous);
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_OK, "large-world camera inverse");
    CHECK(fg_math_project(transform, 0.25f, 0.75f, fg_test_depth(2), uv), "large-world identity projects");
    close_to(uv[0], 0.25, 5e-5, "large-world identity golden u");
    close_to(uv[1], 0.75, 5e-5, "large-world identity golden v");
}
static void rejection_and_cuts(void) {
    float current[84], previous[84], bad[84], transform[16];
    fg_test_camera(current, 0, 0, 0, 0); memcpy(previous, current, sizeof current);
    memcpy(bad, current, sizeof bad); bad[83] = NAN;
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_NONFINITE, "reject nonfinite camera fields");
    memcpy(bad, current, sizeof bad); bad[56] = 0;
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_PROJECTION, "reject zero near");
    memcpy(bad, current, sizeof bad); bad[57] = bad[56];
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_PROJECTION, "reject invalid far");
    memcpy(bad, current, sizeof bad); bad[62] = 4;
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_PROJECTION, "reject aspect mismatch");
    memcpy(bad, current, sizeof bad); bad[70] = 3.14f;
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_PROJECTION, "reject extreme FOV");
    memcpy(bad, current, sizeof bad); bad[72] += 16;
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_EXTENT, "reject render dimension mismatch");
    memcpy(bad, current, sizeof bad); bad[44] = 1.1f;
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_POSE, "reject nonrigid inverse view");
    memcpy(bad, current, sizeof bad); bad[12] += 0.1f;
    CHECK(fg_math_camera(bad, FG_TEST_W, FG_TEST_H) == FG_MATH_PROJECTION, "reject inconsistent VP");
    double singular[16] = {0}, inverse[16];
    CHECK(!fg_math_inverse(singular, inverse), "reject singular inverse");
    fg_test_camera(previous, 0, -8.1, 0, 0);
    for (int i = 0; i < 16; i++) transform[i] = 123;
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_CUT, "reject teleport cut");
    for (int i = 0; i < 16; i++) CHECK(transform[i] == 123, "cut leaves output unchanged");
    fg_test_camera(previous, -0.46, 0, 0, 0);
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_CUT, "reject >25 degree rotation cut");
    memcpy(previous, current, sizeof previous); previous[30] = 0.01f;
    previous[14] = 0.01f; /* matching P*V jitter; still individually valid */
    CHECK(fg_math_predict(current, previous, FG_TEST_W, FG_TEST_H, transform) == FG_MATH_CUT, "reject projection transition");
}
static void camera_pair_diagnostics(void) {
    float first[84], second[84], bad[84];
    fg_test_camera(first,0,0,0,0);
    memcpy(second,first,sizeof second);
    fg_camera_pair_stats sample=fg_math_camera_pair_stats(first,second,FG_TEST_W,FG_TEST_H,
        FG_MATH_OK,0);
    CHECK(sample.rejection==FG_CAMERA_REJECT_NONE,"accepted pair has no rejection");
    close_to((float)sample.rotation_degrees,0,1e-6,"stationary pair rotation");
    close_to((float)sample.translation_distance,0,1e-6,"stationary pair translation");
    fg_test_camera(second,0,-8.1,0,0);
    sample=fg_math_camera_pair_stats(first,second,FG_TEST_W,FG_TEST_H,FG_MATH_CUT,0);
    CHECK(sample.rejection==FG_CAMERA_REJECT_TRANSLATION,"translation cut classified");
    close_to((float)sample.translation_distance,8.1,1e-5,"translation measurement");
    fg_test_camera(second,-0.46,0,0,0);
    sample=fg_math_camera_pair_stats(first,second,FG_TEST_W,FG_TEST_H,FG_MATH_CUT,0);
    CHECK(sample.rejection==FG_CAMERA_REJECT_ROTATION,"rotation cut classified");
    CHECK(sample.rotation_degrees>26 && sample.rotation_degrees<27,"relative rotation measured in degrees");
    memcpy(second,first,sizeof second); second[30]=0.01f; second[14]=0.01f;
    sample=fg_math_camera_pair_stats(first,second,FG_TEST_W,FG_TEST_H,FG_MATH_CUT,0);
    CHECK(sample.rejection==FG_CAMERA_REJECT_PROJECTION,"projection cut classified");
    memcpy(bad,first,sizeof bad); bad[83]=NAN;
    sample=fg_math_camera_pair_stats(first,bad,FG_TEST_W,FG_TEST_H,FG_MATH_NONFINITE,0);
    CHECK(sample.rejection==FG_CAMERA_REJECT_VALIDATION,"camera validation classified");
    fg_camera_pair_window window={0};
    fg_math_camera_pair_accumulate(&window,sample);
    CHECK(window.validation==1 && window.comparisons==1 && window.samples==0,
          "invalid camera is counted without inventing a zero pose measurement");
    fg_test_camera(second,-0.1,1,0,0);
    sample=fg_math_camera_pair_stats(first,second,FG_TEST_W,FG_TEST_H,FG_MATH_OK,0);
    fg_math_camera_pair_accumulate(&window,sample);
    CHECK(window.validation==1 && window.comparisons==2 && window.samples==1,
          "pose mean denominator excludes invalid comparisons");
    close_to((float)(window.rotation_sum/window.samples),(float)sample.rotation_degrees,1e-6,
             "invalid comparison cannot bias measured rotation mean");
    close_to((float)(window.translation_sum/window.samples),1,1e-6,
             "invalid comparison cannot bias measured translation mean");
}

static void synthetic_fallbacks(void) {
    float flat[] = {0.5f,0.5f,0.5f,0.5f,0.5f};
    float discontinuity[] = {0.5f,0.5f,0.8f,0.5f,0.5f};
    float distant_edge[] = {0.99f,0.99f,0.991f,0.99f,0.99f};
    CHECK(fg_math_depth_continuous(flat, 5), "flat synthetic depth accepts warp");
    CHECK(!fg_math_depth_continuous(discontinuity, 5), "depth discontinuity holds real pixel");
    CHECK(!fg_math_depth_continuous(distant_edge, 5), "compressed far edge holds real pixel by relative distance");
    flat[2] = 1; CHECK(!fg_math_depth_continuous(flat, 5), "sky depth holds real pixel");
    flat[2] = NAN; CHECK(!fg_math_depth_continuous(flat, 5), "NaN depth holds real pixel");
    flat[2] = -0.1f; CHECK(!fg_math_depth_continuous(flat, 5), "negative depth holds real pixel");
    float transform[16] = {1,0,0,-3, 0,1,0,0, 0,0,1,0, 0,0,0,1}, uv[2] = {77,88};
    CHECK(!fg_math_project(transform, 0.5f, 0.5f, 0.5f, uv), "newly revealed out-of-bounds surface holds real pixel");
    CHECK(uv[0] == 77 && uv[1] == 88, "invalid projection leaves output unchanged");
    transform[3] = 0; transform[15] = -1;
    CHECK(!fg_math_project(transform, 0.5f, 0.5f, 0.5f, uv), "behind-camera projection holds real pixel");
    const float scene[3] = {0.2f,0.3f,0.4f}, final[3] = {0.2f,1,0.4f};
    CHECK(!fg_math_hud_difference(scene, scene), "identical encoded color is not HUD");
    CHECK(fg_math_hud_difference(scene, final), "HUD difference holds final real color");
    /* This tests HUD hold, not inverse-alpha or independently recomposited UI. */
}

static void interpolation_uvs(void) {
    float first[84], second[84], transform[2][16], uv[2];
    fg_test_camera(first, 0, 0, 0, 0); memcpy(second, first, sizeof first);
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_OK,
          "stationary interpolation accepted");
    for (int side = 0; side < 2; side++) {
        int projected = fg_math_project(transform[side], 0.3f, 0.7f, fg_test_depth(4), uv);
        CHECK(projected, "stationary midpoint projects");
        if (!projected) continue; /* Failed projection deliberately leaves the output untouched. */
        close_to(uv[0], 0.3, 1e-6, "stationary midpoint u");
        close_to(uv[1], 0.7, 1e-6, "stationary midpoint v");
    }
    fg_test_camera(first, 0, -0.12, 0, 0); fg_test_camera(second, 0, 0.12, 0, 0);
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_OK,
          "two-source translation accepted");
    for (int side = 0; side < 2; side++) {
        int projected = fg_math_project(transform[side], 0.5f, 0.5f, fg_test_depth(4), uv);
        CHECK(projected, "both translations project");
        if (!projected) continue;
        close_to(uv[0], 0.5 + (side ? 1 : -1) * 0.0084375, 2e-6, "opposite source-to-midpoint displacement");
        close_to(uv[1], 0.5, 1e-6, "midpoint translation preserves vertical coordinate");
    }
    const double radians = 0.17453292519943295;
    fg_test_camera(first, -radians, 0, 0, 0); fg_test_camera(second, radians, 0, 0, 0);
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_OK,
          "rigid shortest-arc midpoint accepted");
    for (int side = 0; side < 2; side++) {
        int projected = fg_math_project(transform[side], 0.5f, 0.5f, fg_test_depth(4), uv);
        CHECK(projected, "rotating source reaches midpoint");
        if (!projected) continue;
        close_to(uv[0], 0.5 + (side ? -1 : 1) * 0.5 * ((double)FG_TEST_H/FG_TEST_W) * tan(radians),
                 2e-6, "midpoint is halfway rotation, not matrix average");
    }
    fg_test_camera(first, 0.2, 4000, -1000, 3000); memcpy(second, first, sizeof first);
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_OK,
          "large-world midpoint accepted");
    for (int side = 0; side < 2; side++) {
        int projected = fg_math_project(transform[side], 0.25f, 0.75f, fg_test_depth(4), uv);
        CHECK(projected, "large-world midpoint projects");
        if (!projected) continue;
        close_to(uv[0], 0.25, 5e-5, "large-world midpoint u");
        close_to(uv[1], 0.75, 5e-5, "large-world midpoint v");
    }
    fg_test_camera(first, 0, 0, 0, 0); fg_test_camera(second, 0, 8.1, 0, 0);
    for (int side = 0; side < 2; side++) for (int i = 0; i < 16; i++) transform[side][i] = 123;
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_CUT,
          "midpoint rejects teleport");
    for (int side = 0; side < 2; side++) for (int i = 0; i < 16; i++)
        CHECK(transform[side][i] == 123, "midpoint rejection leaves both matrices unchanged");
    fg_test_camera(second, 0.46, 0, 0, 0);
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_CUT,
          "midpoint rejects camera rotation cut");
    memcpy(second, first, sizeof first); second[30] = second[14] = 0.01f;
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_CUT,
          "midpoint rejects projection cut");
    memcpy(second, first, sizeof first); second[83] = NAN;
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_NONFINITE,
          "midpoint validates future camera");
    memcpy(second, first, sizeof first); first[72] += 16;
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_EXTENT,
          "midpoint validates earlier camera extent");
}

static void interpolation_depth_ranges(void) {
    const double ranges[][2]={{1,25000},{0.5,10000},{0.1,5000}};
    for (unsigned pair=1;pair<3;pair++) {
        float camera[2][84],transform[2][16];
        for (unsigned side=0;side<2;side++) {
            fg_test_camera(camera[side],0,0,0,0);
            double n=ranges[pair-1+side][0],f=ranges[pair-1+side][1];
            camera[side][38]=camera[side][22]=(float)(-(f+n)/(f-n));
            camera[side][39]=camera[side][23]=(float)(-2*f*n/(f-n));
            camera[side][56]=(float)n; camera[side][57]=(float)f;
        }
        fg_math_result result=fg_math_interpolate(camera[0],camera[1],FG_TEST_W,FG_TEST_H,
                                                 transform[0],transform[1]);
        CHECK(result == FG_MATH_OK,"independent valid near/far ranges accept midpoint interpolation");
        if (result != FG_MATH_OK) continue;
        float projected[2][3]={{0}};
        for (unsigned side=0;side<2;side++) {
            double n=ranges[pair-1+side][0],f=ranges[pair-1+side][1];
            float d=(float)(f/(f-n)-f*n/((f-n)*2));
            int valid=fg_math_project(transform[side],0.3f,0.7f,d,projected[side]);
            CHECK(valid,"independent depth range projects the same physical surface");
            if (!valid) continue;
            double z=2*(double)d-1;
            projected[side][2]=(float)(0.5+0.5*
                (transform[side][8]*(-0.4)+transform[side][9]*(-0.4)+transform[side][10]*z+transform[side][11])/
                (transform[side][12]*(-0.4)+transform[side][13]*(-0.4)+transform[side][14]*z+transform[side][15]));
            close_to(projected[side][0],0.3,1e-6,"depth drift preserves midpoint u");
            close_to(projected[side][1],0.7,1e-6,"depth drift preserves midpoint v");
        }
        close_to(projected[0][2],projected[1][2],1e-6,
                 "both source depths enter the same midpoint depth basis");
    }
}

/* A static world rendered independently at x=-0.12, +0.12 and the exact x=0
   midpoint: a narrow z=-2 rectangle against a colored z=-4 plane. Preserve both
   real captures, then mirror the shader's nearest depth / bilinear color sampling
   and fixed-point inverse warp. This is a CPU golden, not a GPU execution test. */
typedef struct { float rgb[3], depth; } interp_pixel;
typedef interp_pixel interp_image[FG_TEST_H][FG_TEST_W];
static interp_image interp_capture[2];

static interp_pixel interp_world(double camera_x, double u, double v, int occluder) {
    double rx = (2*u-1) * FG_TEST_W/FG_TEST_H, ry = 1-2*v;
    int foreground = occluder && fabs(camera_x + 2*rx) <= 0.1 && fabs(2*ry) <= 0.4;
    double distance = foreground ? 2 : 4;
    interp_pixel pixel = {{0.8f,0.2f,0.1f}, fg_test_depth(distance)};
    if (!foreground) {
        pixel.rgb[0] = (float)(0.3 + 0.05 * (camera_x + distance*rx));
        pixel.rgb[1] = (float)(0.5 + 0.05 * distance*ry);
        pixel.rgb[2] = 0.25f;
    }
    return pixel;
}
static int interp_inside(float u, float v) {
    return u >= 0.5f/FG_TEST_W && u <= 1.0f-0.5f/FG_TEST_W &&
           v >= 0.5f/FG_TEST_H && v <= 1.0f-0.5f/FG_TEST_H;
}
static float interp_depth(const interp_image image, float u, float v) {
    return image[(unsigned)(v*FG_TEST_H)][(unsigned)(u*FG_TEST_W)].depth;
}
static unsigned interp_clamp(int coordinate, unsigned size) {
    return coordinate < 0 ? 0u : (unsigned)coordinate >= size ? size-1 : (unsigned)coordinate;
}
static void interp_color(const interp_image image, float u, float v, float rgb[3]) {
    float x = u*FG_TEST_W-0.5f, y = v*FG_TEST_H-0.5f;
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x-ix, fy = y-iy;
    unsigned x0 = interp_clamp(ix, FG_TEST_W), x1 = interp_clamp(ix+1, FG_TEST_W);
    unsigned y0 = interp_clamp(iy, FG_TEST_H), y1 = interp_clamp(iy+1, FG_TEST_H);
    for (int c = 0; c < 3; c++)
        rgb[c] = (image[y0][x0].rgb[c]*(1-fx) + image[y0][x1].rgb[c]*fx)*(1-fy) +
                 (image[y1][x0].rgb[c]*(1-fx) + image[y1][x1].rgb[c]*fx)*fy;
}
static int interp_project(const interp_image image, const float transform[16], const float source[2],
                          float projected[3], float *depth) {
    if (!interp_inside(source[0], source[1])) return 0;
    *depth = interp_depth(image, source[0], source[1]);
    if (!isfinite(*depth) || *depth < 0 || *depth >= 1.0f-1e-6f) return 0;
    double clip[4] = {2*(double)source[0]-1, 1-2*(double)source[1], 2*(double)*depth-1, 1};
    double p[4] = {0};
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) p[r] += transform[r*4+c]*clip[c];
    for (int i = 0; i < 4; i++) if (!isfinite(p[i])) return 0;
    if (p[3] <= 1e-5 || fabs(p[2]) > p[3]) return 0;
    projected[0] = (float)(0.5+0.5*p[0]/p[3]);
    projected[1] = (float)(0.5-0.5*p[1]/p[3]);
    projected[2] = (float)(0.5+0.5*p[2]/p[3]);
    return projected[2] >= 0 && projected[2] < 1.0f-1e-6f &&
           fabsf(projected[0]-source[0]) <= 0.04f && fabsf(projected[1]-source[1]) <= 0.04f;
}
static int interp_candidate(const interp_image image, const float transform[16], float u, float v,
                            float rgb[3], float *midpoint_depth) {
    if (!interp_inside(u, v)) return 0;
    float source[2] = {u,v}, projected[3], depth;
    for (int i = 0; i < 3; i++) {
        if (!interp_project(image, transform, source, projected, &depth)) return 0;
        source[0] += u-projected[0]; source[1] += v-projected[1];
    }
    if (!interp_project(image, transform, source, projected, &depth) ||
        fabsf(projected[0]-u) > 1.5f/FG_TEST_W || fabsf(projected[1]-v) > 1.5f/FG_TEST_H) return 0;
    const float offsets[4][2] = {{-1,0},{1,0},{0,-1},{0,1}};
    float neighborhood[5] = {depth,0,0,0,0};
    for (int i = 0; i < 4; i++) {
        float x = source[0]+offsets[i][0]/FG_TEST_W, y = source[1]+offsets[i][1]/FG_TEST_H;
        if (!interp_inside(x, y)) return 0;
        neighborhood[i+1] = interp_depth(image, x, y);
    }
    if (!fg_math_depth_continuous(neighborhood, 5)) return 0;
    interp_color(image, source[0], source[1], rgb);
    for (int c = 0; c < 3; c++) if (!isfinite(rgb[c])) return 0;
    *midpoint_depth = projected[2];
    return 1;
}
static unsigned interp_render(const float transform[2][16], float u, float v, float rgb[3]) {
    float first[3] = {0}, second[3] = {0}, first_z = 1, second_z = 1;
    int a = interp_candidate(interp_capture[0], transform[0], u, v, first, &first_z);
    int b = interp_candidate(interp_capture[1], transform[1], u, v, second, &second_z);
    unsigned selected = fg_math_interpolation_sources(a, first_z, b, second_z);
    if (!selected) interp_color(interp_capture[0], u, v, rgb);
    else for (int c = 0; c < 3; c++)
        rgb[c] = selected == 3 ? (first[c]+second[c])*0.5f : selected == 1 ? first[c] : second[c];
    return selected;
}
static void interp_capture_world(int occluder) {
    for (unsigned side = 0; side < 2; side++)
        for (unsigned y = 0; y < FG_TEST_H; y++) for (unsigned x = 0; x < FG_TEST_W; x++)
            interp_capture[side][y][x] = interp_world(side ? 0.12 : -0.12,
                                                    (x+0.5)/FG_TEST_W, (y+0.5)/FG_TEST_H, occluder);
}
static void interpolation_scene(void) {
    float first[84], second[84], transform[2][16], actual[3];
    fg_test_camera(first, 0, -0.12, 0, 0); fg_test_camera(second, 0, 0.12, 0, 0);
    CHECK(fg_math_interpolate(first, second, FG_TEST_W, FG_TEST_H, transform[0], transform[1]) == FG_MATH_OK,
          "static two-capture scene has midpoint transforms");
    interp_capture_world(0);
    unsigned valid_pixels = 0;
    int finite_image = 1;
    float max_error[3] = {0};
    for (unsigned y = 2; y < FG_TEST_H-2; y++) for (unsigned x = 1; x < FG_TEST_W-1; x++) {
        float u = (x+0.5f)/FG_TEST_W, v = (y+0.5f)/FG_TEST_H;
        valid_pixels += interp_render(transform, u, v, actual) != 0;
        interp_pixel expected = interp_world(0, u, v, 0);
        for (int c = 0; c < 3; c++) {
            if (!isfinite(actual[c])) finite_image = 0;
            max_error[c] = fmaxf(max_error[c], fabsf(actual[c]-expected.rgb[c]));
        }
    }
    CHECK(valid_pixels == (FG_TEST_W-2)*(FG_TEST_H-4), "static plane covers entire sampleable intermediate view");
    CHECK(finite_image, "static intermediate image is finite");
    for (int c = 0; c < 3; c++)
        close_to(max_error[c], 0, 2e-6, "static intermediate image matches exact camera across full interior");
    interp_capture_world(1);
    const float points[][2] = {{0.3f,0.4f},{0.7f,0.6f},{0.5f,0.5f}};
    for (unsigned i = 0; i < sizeof points/sizeof *points; i++) {
        CHECK(interp_render(transform, points[i][0], points[i][1], actual) == 3,
              "two valid static surfaces blend");
        interp_pixel expected = interp_world(0, points[i][0], points[i][1], 1);
        for (int c = 0; c < 3; c++)
            close_to(actual[c], expected.rgb[c], 2e-6, "static world matches exact intermediate-camera image");
    }
    for (unsigned side = 0; side < 2; side++) {
        float u = side ? 0.521875f : 0.478125f;
        CHECK(interp_render(transform, u, 0.5f, actual) == (side ? 2u : 1u),
              "disocclusion uses only side that actually saw the background");
        interp_pixel expected = interp_world(0, u, 0.5, 1);
        for (int c = 0; c < 3; c++)
            close_to(actual[c], expected.rgb[c], 2e-6, "one-sided reveal matches exact midpoint, not stale foreground");
    }
    CHECK(interp_render(transform, 0.3f, 0.5f/FG_TEST_H, actual) == 0,
          "both-invalid border conservatively keeps earlier scene");
    float earlier[3]; interp_color(interp_capture[0], 0.3f, 0.5f/FG_TEST_H, earlier);
    for (int c = 0; c < 3; c++) close_to(actual[c], earlier[c], 0, "both-invalid fallback is exact earlier scene");
    float near = fg_test_depth(2), far = fg_test_depth(4);
    CHECK(fg_math_interpolation_sources(1, near, 1, far) == 1, "nearer first surface rejects hidden second surface");
    CHECK(fg_math_interpolation_sources(1, far, 1, near) == 2, "nearer second surface rejects hidden first surface");
    CHECK(fg_math_interpolation_sources(1, far, 1, far) == 3, "agreeing midpoint depths blend");
    CHECK(fg_math_interpolation_sources(1, NAN, 1, far) == 2, "invalid first depth uses second side");
    CHECK(fg_math_interpolation_sources(1, near, 1, INFINITY) == 1, "invalid second depth uses first side");
    CHECK(!fg_math_interpolation_sources(1, 1, 1, -1), "both invalid midpoint depths keep earlier scene");
    /* UI stays in canvas coordinates. Black/white replay reconstructs the
       affine RGB blend for translucent and opaque UI without alpha guessing. */
    const float background[3] = {0.2f,0.4f,0.7f}, ui[3] = {0.9f,0.3f,0.1f};
    const float alpha[] = {0,0.35f,1};
    for (unsigned i = 0; i < sizeof alpha/sizeof *alpha; i++) for (int c = 0; c < 3; c++) {
        float black = ui[c]*alpha[i], white = black+1-alpha[i];
        close_to(background[c]*(white-black)+black, background[c]*(1-alpha[i])+ui[c]*alpha[i],
                 1e-6, "unwarped black/white UI replay preserves original linear blend");
    }
}
static void world_plane_depth(void) {
    float camera[84],bad[84];
    fg_test_camera(camera,0,0,10,0);
    double c=cos(0.35),s=sin(0.35);
    double pose[12]={1,0,0,0, 0,c,-s,10, 0,s,c,0};
    double view[16]={1,0,0,0, 0,c,s,-10*c, 0,-s,c,10*s, 0,0,0,1};
    double projection[16],vp[16];
    for (unsigned i=0;i<16;i++) projection[i]=camera[28+i];
    fg_math_mul(projection,view,vp);
    for (unsigned i=0;i<12;i++) { camera[i]=(float)view[i]; camera[44+i]=(float)pose[i]; }
    for (unsigned i=0;i<16;i++) camera[12+i]=(float)vp[i];
    camera[65]=(1.0f-camera[38])*0.5f;
    CHECK(fg_math_camera(camera,FG_TEST_W,FG_TEST_H)==FG_MATH_OK,"pitched world-plane camera is geometrically valid");
    CHECK(fg_math_plane_projection(camera),"pitched world plane is valid without requiring an axis-aligned view ray");
    for (unsigned i=0;i<5;i++) {
        double ray[3]={-0.4+0.2*i,-0.6-0.1*i,-1};
        double p=camera[48]*ray[0]+camera[49]*ray[1]+camera[50]*ray[2];
        double t=(4-camera[51])/p,point[3]={t*ray[0],t*ray[1],t*ray[2]};
        CHECK(t>0,"world-Y=4 plane intersection is in front of the eye");
        close_to((float)(camera[48]*point[0]+camera[49]*point[1]+camera[50]*point[2]+camera[51]),
                 4,1e-5,"inverse-view transform of the ray intersection lies on world Y=4");
        double clip_z=camera[38]*point[2]+camera[39],clip_w=-point[2];
        double actual=camera[65]*(1-camera[56]*p/(ray[2]*(camera[51]-4)));
        close_to((float)actual,(clip_z/clip_w+1)*0.5,2e-7,
                 "ecbe plane formula equals ordinary projection of the same world-plane point");
    }
    memcpy(bad,camera,sizeof bad); bad[65]*=1.01f;
    CHECK(!fg_math_plane_projection(bad),"different plane depth coefficient refuses");
    memcpy(bad,camera,sizeof bad); bad[39]*=1.01f;
    CHECK(!fg_math_plane_projection(bad),"different near-depth coefficient refuses");
    memcpy(bad,camera,sizeof bad); bad[36]=0.001f;
    CHECK(!fg_math_plane_projection(bad),"x-dependent clip depth refuses");
    memcpy(bad,camera,sizeof bad); bad[42]=1;
    CHECK(!fg_math_plane_projection(bad),"different forward-axis projection refuses");
    memcpy(bad,camera,sizeof bad); bad[65]=NAN;
    CHECK(!fg_math_plane_projection(bad),"nonfinite plane coefficient refuses");
}
/* Grid points exercise the analytic envelope; they are NOT its certificate.
   Certification is the polynomial-box/positive-W proof in framegen_math.h. */
static void motion_encloses(const float transform[16], float lo, float hi, const char *message) {
    fg_motion_bound bound=fg_math_motion_bound(transform,lo,hi,FG_TEST_W,FG_TEST_H);
    for (unsigned z=0;z<5;z++) for (unsigned y=0;y<=12;y++) for (unsigned x=0;x<=12;x++) {
        float u=x/12.0f,v=y/12.0f,d=lo+(hi-lo)*(z/4.0f),out[2];
        if (!fg_math_project(transform,u,v,d,out)) continue;
        for (unsigned axis=0;axis<2;axis++) {
            float delta=fabsf(out[axis]-(axis ? v : u));
            if (delta<=FG_MOTION_CAP) CHECK(delta<=bound.uv[axis]+2*FLT_EPSILON,message);
        }
    }
}
static void conservative_motion_bounds(void) {
    float identity[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    fg_motion_bound bound=fg_math_motion_bound(identity,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==0 && bound.uv[1]==0,"exact stationary transform has zero analytic displacement");
    CHECK(bound.radius[0]==1 && bound.radius[1]==1,"stationary mask retains one-texel footprint");
    float first[84],second[84],transform[2][16],stationary[16];
    fg_test_camera(first,0,0,0,0); memcpy(second,first,sizeof second);
    CHECK(fg_math_interpolate(first,second,FG_TEST_W,FG_TEST_H,transform[0],transform[1])==FG_MATH_OK,
          "stationary pair produces actual transforms for bound");
    memcpy(stationary,transform[1],sizeof stationary);
    bound=fg_math_motion_bound(stationary,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.radius[0]<=2 && bound.radius[1]<=2,"actual stationary pair shrinks fixed fourteen-by-nine radius");
    fg_test_camera(second,0,0.16,0,0);
    CHECK(fg_math_interpolate(first,second,FG_TEST_W,FG_TEST_H,transform[0],transform[1])==FG_MATH_OK,
          "translated next pair has new old-source transform");
    float depth=fg_test_depth(2);
    fg_motion_bound old_bound=fg_math_motion_bound(stationary,depth,depth,FG_TEST_W,FG_TEST_H);
    for (unsigned side=0;side<2;side++) {
        bound=fg_math_motion_bound(transform[side],depth,depth,FG_TEST_W,FG_TEST_H);
        CHECK(bound.uv[0]>=0.01125f && bound.uv[0]<0.012f,"certified z=2 full-depth domain narrows translation bound");
        CHECK(bound.radius[0]>old_bound.radius[0] && bound.radius[0]<14,"old source must be dilated for its new pair");
        CHECK(bound.radius[1]==1,"horizontal translation does not dilate vertical motion");
        motion_encloses(transform[side],depth,fg_test_depth(8),"translation envelope includes near and far endpoints");
        fg_motion_bound global=fg_math_motion_bound(transform[side],0,1,FG_TEST_W,FG_TEST_H);
        CHECK(global.uv[0]==FG_MOTION_CAP,"uncertified full near-depth domain retains hard cap");
        fg_motion_bound zero=fg_math_motion_bound(transform[side],-0.0f,0.0f,FG_TEST_W,FG_TEST_H);
        CHECK(zero.uv[0]==FG_MOTION_CAP,"signed-zero depth is a near-plane value, never a far-only certificate");
        float far=nextafterf(1.0f-1e-6f,0);
        motion_encloses(transform[side],far,far,"last shader-valid far depth remains enclosed");
    }
    fg_test_camera(first,-0.01,0,0,0); fg_test_camera(second,0.01,0,0,0);
    CHECK(fg_math_interpolate(first,second,FG_TEST_W,FG_TEST_H,transform[0],transform[1])==FG_MATH_OK,
          "rotated pair produces conservative envelopes");
    for (unsigned side=0;side<2;side++)
        motion_encloses(transform[side],0,1,"rotated envelope encloses full accepted clip box");
    /* All eight clip corners have zero X displacement here; its interior does
       not. A projected-position corner bound incorrectly misses this maximum. */
    float interior[16]={1,0,0,0.02f, 0,1,0,0, 0,0,1,0, 0.02f,0,0,1};
    bound=fg_math_motion_bound(interior,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]>0.01f && bound.uv[0]<0.012f,"quadratic interior extremum cannot be replaced by corner displacement samples");
    motion_encloses(interior,0,1,"interior extremum and coupled vertical displacement enclosed");
    float mixed[16]={1,0.003f,-0.002f,0.004f, -0.002f,1,0.001f,-0.003f,
                     0,0,1,0, 0.006f,-0.004f,0.002f,1};
    motion_encloses(mixed,0.2f,0.999f,"mixed projective cross terms enclosed on all box edges");
    float bad[16]; memcpy(bad,identity,sizeof bad); bad[12]=2;
    bound=fg_math_motion_bound(bad,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==FG_MOTION_CAP && bound.uv[1]==FG_MOTION_CAP,"box crossing horizon retains hard cap");
    memcpy(bad,identity,sizeof bad); bad[15]=1e-6f;
    bound=fg_math_motion_bound(bad,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==FG_MOTION_CAP && bound.uv[1]==FG_MOTION_CAP,"uncertified tiny positive W retains hard cap");
    memcpy(bad,identity,sizeof bad); bad[15]=-1;
    bound=fg_math_motion_bound(bad,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==FG_MOTION_CAP,"entire behind-camera box cannot shrink bound");
    memcpy(bad,identity,sizeof bad); bad[7]=INFINITY;
    bound=fg_math_motion_bound(bad,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==FG_MOTION_CAP && bound.uv[1]==FG_MOTION_CAP,"nonfinite transform retains hard cap");
    memcpy(bad,identity,sizeof bad); bad[10]=0;
    bound=fg_math_motion_bound(bad,0,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==FG_MOTION_CAP,"singular transform cannot shrink bound");
    bound=fg_math_motion_bound(identity,NAN,1,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==FG_MOTION_CAP,"nonfinite depth certificate cannot shrink bound");
    bound=fg_math_motion_bound(identity,0.9f,0.2f,FG_TEST_W,FG_TEST_H);
    CHECK(bound.uv[0]==FG_MOTION_CAP,"reversed depth interval cannot shrink bound");
    bound=fg_math_motion_bound(identity,0,1,0,FG_TEST_H);
    CHECK(bound.radius[0]==0 && bound.uv[0]==FG_MOTION_CAP,"missing extent cannot certify motion");
}
static void active_shrink_requires_primitive_bounds(void) {
    /* Admissible clipped triangle, not a claim about the game's actual mesh:
       view w=10 at all vertices; view rays x={100,-1,-1}; world-Y row
       (0.2,0,sqrt(.96)), eye Y=104. Only the far offscreen A vertex satisfies
       the world-XZ shrink ramp. Its texture gate can select k=5, others k=0.
       Clipping does not discard its perspective-correct varying contribution. */
    const double c=sqrt(0.96),eye_above_plane=100,w=10;
    const double world_x_a=c*(100*w)+0.2*w;
    const double world_x_b=-c*w+0.2*w;
    const double world_y_a=104+0.2*(100*w)-c*w;
    const double world_y_b=104-0.2*w-c*w;
    CHECK(world_x_a>750 && hypot(world_x_b,w)<500,"500 world-XZ threshold does not put every clipped contributor at view depth500");
    CHECK(world_y_a>5 && world_y_a<800 && world_y_b>0 && world_y_b<800,
        "observed amplitude800 permits both original vertex heights and a texture-selected shrink boundary");
    const double k_a=fmin(fmax(w,2.5),7.5)-2.5;
    CHECK(k_a==5 && w-k_a==5,"active ramp admits k5 at a positive but near offscreen vertex");
    const double lambda_a=1.0/101.0;
    const double raw_ray=lambda_a*100+(1-lambda_a)*-1;
    const double s_a=w/(w-k_a),normalizer=lambda_a*s_a+(1-lambda_a);
    const double nu_a=lambda_a*s_a/normalizer;
    const double biased_ray=nu_a*100+(1-nu_a)*-1;
    CHECK(fabs(raw_ray)<1e-12 && biased_ray>0.9,"unchanged view-position varying no longer follows the raster pixel ray");
    const double true_plane=eye_above_plane/c;
    const double biased_plane=eye_above_plane/(c-0.2*biased_ray);
    const double raster=1.0/(lambda_a/(w-k_a)+(1-lambda_a)/w);
    CHECK(true_plane>w && biased_plane>raster,"FS max chooses the erroneous plane channel, not the bounded raster channel");
    const double focal=640.0/(tan(25.0*3.14159265358979323846/180.0)*(16.0/9.0));
    const double pixel_error=focal*0.5*fabs(1/true_plane-1/biased_plane);
    CHECK(pixel_error>0.75,"one-unit camera-pair translation can exceed0.25px despite k<=5 and world-XZ shrink threshold");
}
int main(void) {
    golden_uvs(); rejection_and_cuts(); camera_pair_diagnostics(); synthetic_fallbacks(); interpolation_uvs();
    focal_interpolation(); interpolation_scene();
    interpolation_depth_ranges();
    world_plane_depth();
    conservative_motion_bounds();
    active_shrink_requires_primitive_bounds();
    printf("framegen camera/depth goldens: %u checks, %u failed\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
