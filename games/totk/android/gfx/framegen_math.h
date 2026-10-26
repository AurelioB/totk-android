/* TOTK camera-only half-step extrapolation and two-real-frame interpolation.
   No Vulkan or guest-memory dependency.
   The 84-float snapshot is the current camera from VS UBO1 (2368 bytes):
   view[0..11], row-major VP[12..27], projection[28..43], inverse view[44..55].
   Camera matrices use NVN clip z -1..1 and y up; sampled Vulkan depth is 0..1.
   Interpolation uses both preserved images; neither mode estimates object flow. */
#ifndef TOTK_FRAMEGEN_MATH_H
#define TOTK_FRAMEGEN_MATH_H

#include <math.h>
#include <float.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    FG_MATH_OK = 0, FG_MATH_NONFINITE, FG_MATH_EXTENT, FG_MATH_PROJECTION,
    FG_MATH_POSE, FG_MATH_SINGULAR, FG_MATH_CUT
} fg_math_result;

static inline const char *fg_math_reason(fg_math_result r) {
    switch (r) {
        case FG_MATH_OK: return "accepted";
        case FG_MATH_NONFINITE: return "nonfinite camera";
        case FG_MATH_EXTENT: return "camera/render extent mismatch";
        case FG_MATH_PROJECTION: return "unsupported/inconsistent projection";
        case FG_MATH_POSE: return "nonrigid/inconsistent camera pose";
        case FG_MATH_SINGULAR: return "singular view-projection";
        case FG_MATH_CUT: return "camera/projection cut";
    }
    return "invalid camera";
}

static inline void fg_math_mul(const double a[16], const double b[16], double out[16]) {
    /* out must not alias a or b. */
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        double s = 0;
        for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c];
        out[r * 4 + c] = s;
    }
}

static inline int fg_math_inverse(const double in[16], double out[16]) {
    double a[4][8], scale[4];
    for (int r = 0; r < 4; r++) {
        scale[r] = 0;
        for (int c = 0; c < 4; c++) {
            double v = in[r * 4 + c];
            if (!isfinite(v)) return 0;
            a[r][c] = v;
            if (fabs(v) > scale[r]) scale[r] = fabs(v);
            a[r][c + 4] = r == c;
        }
        if (scale[r] == 0) return 0;
    }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (fabs(a[r][c]) / scale[r] > fabs(a[p][c]) / scale[p]) p = r;
        if (fabs(a[p][c]) <= 1e-13 * scale[p]) return 0;
        if (p != c) {
            for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[p][k]; a[p][k] = t; }
            double t = scale[c]; scale[c] = scale[p]; scale[p] = t;
        }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) if (r != c) {
            double f = a[r][c];
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) {
        if (!isfinite(a[r][c + 4])) return 0;
        out[r * 4 + c] = a[r][c + 4];
    }
    return 1;
}

static inline fg_math_result fg_math_camera(const float cam[84], unsigned w, unsigned h) {
    for (int i = 0; i < 84; i++) if (!isfinite(cam[i])) return FG_MATH_NONFINITE;
    if (w < 320 || h < 180 || w > 16384 || h > 16384 ||
        fabs((double)cam[72] - w) > 1.0 || fabs((double)cam[73] - h) > 1.0)
        return FG_MATH_EXTENT;
    double near = cam[56], far = cam[57], aspect = cam[62], fov = cam[70];
    if (!(near >= 0.001 && near <= 10 && far > near && far <= 1e10 &&
          aspect >= 0.5 && aspect <= 4 && fov >= 0.15 && fov <= 2.8) ||
        fabs(aspect * h / w - 1) > 0.02) return FG_MATH_PROJECTION;
    double p[16], v[16] = {0}, iv[16] = {0}, vp[16], check[16], inverse[16];
    for (int i = 0; i < 16; i++) { p[i] = cam[28 + i]; vp[i] = cam[12 + i]; }
    for (int i = 0; i < 12; i++) { v[i] = cam[i]; iv[i] = cam[44 + i]; }
    v[15] = iv[15] = 1;
    static const int zero[] = {1, 3, 4, 7, 12, 13, 15};
    for (unsigned i = 0; i < sizeof zero / sizeof *zero; i++)
        if (fabs(p[zero[i]]) > 1e-5) return FG_MATH_PROJECTION;
    double ys = 1 / tan(fov * 0.5);
    if (fabs(fabs(p[14]) - 1) > 1e-5 || fabs(fabs(p[5]) / ys - 1) > 0.02 ||
        fabs(fabs(p[0]) * aspect / ys - 1) > 0.02 || fabs(p[2]) > 0.02 || fabs(p[6]) > 0.02)
        return FG_MATH_PROJECTION;
    /* Both forward-axis signs are representable, but only nonreversed -1..1 depth. */
    double zn = near / p[14], zf = far / p[14];
    if (fabs((p[10] * zn + p[11]) / near + 1) > 0.02 ||
        fabs((p[10] * zf + p[11]) / far - 1) > 0.02) return FG_MATH_PROJECTION;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        double dot = 0;
        for (int k = 0; k < 3; k++) dot += iv[r * 4 + k] * iv[c * 4 + k];
        if (fabs(dot - (r == c)) > 0.003) return FG_MATH_POSE;
    }
    double det = iv[0] * (iv[5] * iv[10] - iv[6] * iv[9]) -
        iv[1] * (iv[4] * iv[10] - iv[6] * iv[8]) + iv[2] * (iv[4] * iv[9] - iv[5] * iv[8]);
    if (fabs(det - 1) > 0.005) return FG_MATH_POSE;
    fg_math_mul(v, iv, check);
    double position_scale = fabs(iv[3]) + fabs(iv[7]) + fabs(iv[11]);
    for (int i = 0; i < 16; i++)
        if (fabs(check[i] - (i % 5 == 0)) > (i % 4 == 3 ? 0.02 + 2e-6 * position_scale : 0.003))
            return FG_MATH_POSE;
    fg_math_mul(p, v, check);
    for (int i = 0; i < 16; i++)
        if (fabs(check[i] - vp[i]) > 2e-4 * (1 + fabs(check[i]))) return FG_MATH_PROJECTION;
    if (!fg_math_inverse(vp, inverse)) return FG_MATH_SINGULAR;
    return FG_MATH_OK;
}
/* Smooth zoom is representable by the two-source midpoint warp. The 10% focal
   ratio bounds pure-zoom source->midpoint motion to .025 UV at either image
   edge, below the shader's .04 UV guard. Also cap angular zoom at 3 degrees per
   real tick; neither bound licenses an aspect/extent/sign or off-center cut. */
#define FG_INTERP_MAX_FOV_DELTA (3.0 * 3.14159265358979323846 / 180.0)
#define FG_INTERP_MAX_FOCAL_RATIO 1.10
#define FG_INTERP_MAX_HALF_FOV_TAN 0.02618592156918693 /* tan(1.5 degrees) */
typedef enum {
    FG_PROJECTION_STABLE, FG_PROJECTION_EXTENT, FG_PROJECTION_ASPECT,
    FG_PROJECTION_SIGN, FG_PROJECTION_FOV, FG_PROJECTION_COEFFICIENT
} fg_projection_reject;
static inline fg_projection_reject fg_math_projection_rejection(const float first[84],
                                                                 const float second[84],
                                                                 int interpolation) {
    if (interpolation) {
        if (first[72]!=second[72] || first[73]!=second[73]) return FG_PROJECTION_EXTENT;
        double first_aspect=fabs((double)first[33]/first[28]);
        double second_aspect=fabs((double)second[33]/second[28]);
        if (fabs((double)second[62]-first[62]) > 1e-4*fabs(first[62]) ||
            fabs(second_aspect-first_aspect) > 1e-4*first_aspect)
            return FG_PROJECTION_ASPECT;
        if ((double)first[28]*second[28]<=0 || (double)first[33]*second[33]<=0)
            return FG_PROJECTION_SIGN;
        if (fabs((double)second[70]-first[70]) > FG_INTERP_MAX_FOV_DELTA+4*FLT_EPSILON)
            return FG_PROJECTION_FOV;
        for (int i=0;i<=5;i+=5) {
            double a=fabs(first[28+i]), b=fabs(second[28+i]);
            if (fmax(a,b)>(FG_INTERP_MAX_FOCAL_RATIO+4*FLT_EPSILON)*fmin(a,b))
                return FG_PROJECTION_FOV;
            /* Bound the encoded projection too, not just its camera FOV field.
               tan(delta half-FOV)=(a-b)/(1+a*b), avoiding atan on the hot path. */
            if (i==5 && fabs(a-b)>(FG_INTERP_MAX_HALF_FOV_TAN+4*FLT_EPSILON)*(1+a*b))
                return FG_PROJECTION_FOV;
        }
    }
    for (int i=0;i<16;i++) {
        if (interpolation && (i==0 || i==5 || i==10 || i==11)) continue;
        if (fabs((double)second[28+i]-first[28+i]) > .002*(1+fabs(second[28+i])))
            return FG_PROJECTION_COEFFICIENT;
    }
    return FG_PROJECTION_STABLE;
}

/* Diagnostic-only camera-pair facts. Callers must avoid this helper unless
   diagnostics are enabled; the normal math path does not calculate acos. */
typedef enum {
    FG_CAMERA_REJECT_NONE, FG_CAMERA_REJECT_TRANSLATION, FG_CAMERA_REJECT_ROTATION,
    FG_CAMERA_REJECT_PROJECTION, FG_CAMERA_REJECT_VALIDATION
} fg_camera_reject;
typedef struct {
    fg_camera_reject rejection;
    double rotation_degrees, translation_distance;
} fg_camera_pair_stats;
static inline fg_camera_pair_stats fg_math_camera_pair_stats(const float first[84],
                                                              const float second[84],
                                                              unsigned w, unsigned h,
                                                              fg_math_result result,
                                                              int interpolation) {
    fg_camera_pair_stats stats={FG_CAMERA_REJECT_NONE,NAN,NAN};
    if (fg_math_camera(first,w,h)!=FG_MATH_OK ||
        fg_math_camera(second,w,h)!=FG_MATH_OK || result==FG_MATH_SINGULAR) {
        stats.rejection=FG_CAMERA_REJECT_VALIDATION;
        return stats;
    }
    double distance2=0, trace=0;
    for (int r=0;r<3;r++) {
        double d=(double)second[47+r*4]-first[47+r*4];
        distance2+=d*d;
        for (int k=0;k<3;k++)
            trace+=(double)second[44+r*4+k]*first[44+r*4+k];
    }
    stats.translation_distance=sqrt(distance2);
    double cosine=(trace-1)*0.5;
    stats.rotation_degrees=acos(fmin(1,fmax(-1,cosine)))* (180.0/3.14159265358979323846);
    if (result==FG_MATH_OK) return stats;
    if (distance2>64) stats.rejection=FG_CAMERA_REJECT_TRANSLATION;
    else if (cosine<0.9063077870366499) stats.rejection=FG_CAMERA_REJECT_ROTATION;
    else if (fg_math_projection_rejection(first,second,interpolation)!=FG_PROJECTION_STABLE)
        stats.rejection=FG_CAMERA_REJECT_PROJECTION;
    else stats.rejection=FG_CAMERA_REJECT_VALIDATION;
    return stats;
}
typedef struct {
    uint64_t translation, rotation, projection, validation, comparisons, samples;
    double rotation_sum, rotation_max, translation_sum, translation_max;
} fg_camera_pair_window;
static inline void fg_math_camera_pair_accumulate(fg_camera_pair_window *window,
                                                  fg_camera_pair_stats sample) {
    switch (sample.rejection) {
    case FG_CAMERA_REJECT_TRANSLATION: window->translation++; break;
    case FG_CAMERA_REJECT_ROTATION: window->rotation++; break;
    case FG_CAMERA_REJECT_PROJECTION: window->projection++; break;
    case FG_CAMERA_REJECT_VALIDATION: window->validation++; break;
    default: break;
    }
    window->comparisons++;
    if (!isfinite(sample.rotation_degrees) || !isfinite(sample.translation_distance)) return;
    window->samples++;
    window->rotation_sum+=sample.rotation_degrees;
    window->translation_sum+=sample.translation_distance;
    if (sample.rotation_degrees>window->rotation_max) window->rotation_max=sample.rotation_degrees;
    if (sample.translation_distance>window->translation_max) window->translation_max=sample.translation_distance;
}


/* ecbe's view-ray/world-Y=4 intersection exports A*(1-near/d). It is in
   the accepted camera's projection only for this nonreversed -Z perspective.
   Coefficients arrive independently rounded to float: permit four float eps
   of arithmetic roundoff, not a new projection or material depth scale. */
static inline int fg_math_plane_projection(const float camera[84]) {
    float a=camera[65], near=camera[56];
    if (!isfinite(a) || !isfinite(near) || a<=0 || near<=0 ||
        camera[36]!=0 || camera[37]!=0 || camera[40]!=0 || camera[41]!=0 ||
        camera[42]!=-1 || camera[43]!=0 || !isfinite(camera[38]) || !isfinite(camera[39])) return 0;
    double expected_a=(1.0-(double)camera[38])*0.5;
    double expected_b=-2.0*(double)a*near;
    return fabs((double)a-expected_a)<=4*FLT_EPSILON*fabs(expected_a) &&
        fabs((double)camera[39]-expected_b)<=4*FLT_EPSILON*fabs(expected_b);
}

/* The caller owns the immediately preceding accepted real-frame snapshot. Missing
   captures, resizes and skipped real frames must clear that history. out receives
   generatedVP * inverse(currentVP), row-major, and is unchanged on rejection. */
static inline fg_math_result fg_math_predict(const float curr[84], const float prev[84],
                                             unsigned w, unsigned h, float out[16]) {
    fg_math_result result = fg_math_camera(curr, w, h);
    if (result != FG_MATH_OK) return result;
    result = fg_math_camera(prev, w, h);
    if (result != FG_MATH_OK) return result;
    double distance2 = 0, delta[9], trace = 0;
    for (int r = 0; r < 3; r++) {
        double d = (double)curr[47 + r * 4] - prev[47 + r * 4]; distance2 += d * d;
        for (int c = 0; c < 3; c++) {
            delta[r * 3 + c] = 0;
            for (int k = 0; k < 3; k++) delta[r * 3 + c] += (double)curr[44 + r * 4 + k] * prev[44 + c * 4 + k];
        }
        trace += delta[r * 3 + r];
    }
    if (distance2 > 64 || (trace - 1) * 0.5 < 0.9063077870366499) return FG_MATH_CUT; /* 25 degrees, including roll */
    for (int i = 0; i < 16; i++)
        if (fabs((double)curr[28 + i] - prev[28 + i]) > 0.002 * (1 + fabs(curr[28 + i]))) return FG_MATH_CUT;
    /* Half of the shortest relative rigid rotation, via the unit quaternion.
       The cut bound keeps this far from the ill-conditioned 180-degree case. */
    double qw = sqrt(fmax(0, 1 + trace)) * 0.5;
    double hw = sqrt((1 + qw) * 0.5);
    double x = (delta[7] - delta[5]) / (8 * qw * hw);
    double y = (delta[2] - delta[6]) / (8 * qw * hw);
    double z = (delta[3] - delta[1]) / (8 * qw * hw);
    double half[9] = {1 - 2 * (y*y + z*z), 2 * (x*y - z*hw), 2 * (x*z + y*hw),
        2 * (x*y + z*hw), 1 - 2 * (x*x + z*z), 2 * (y*z - x*hw),
        2 * (x*z - y*hw), 2 * (y*z + x*hw), 1 - 2 * (x*x + y*y)};
    double rotation[9], displacement[3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            rotation[r * 3 + c] = 0;
            for (int k = 0; k < 3; k++) rotation[r * 3 + c] += half[r * 3 + k] * curr[44 + k * 4 + c];
        }
        displacement[r] = -0.5 * ((double)curr[47 + r * 4] - prev[47 + r * 4]);
    }
    /* Algebraically generatedVP * inverse(currentVP) = P * relativeView *
       inverse(P). Form the rigid relative view directly: subtract positions
       before projection, avoiding catastrophic cancellation of world offsets
       in the thousands and of independently rounded float VP translations. */
    double relative[16] = {0}, projection[16], inverse[16], generated[16], transform[16];
    relative[15] = 1;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++)
            relative[r * 4 + c] += rotation[k * 3 + r] * curr[44 + k * 4 + c];
        for (int k = 0; k < 3; k++) relative[r * 4 + 3] += rotation[k * 3 + r] * displacement[k];
    }
    for (int i = 0; i < 16; i++) projection[i] = curr[28 + i];
    if (!fg_math_inverse(projection, inverse)) return FG_MATH_SINGULAR;
    fg_math_mul(projection, relative, generated);
    fg_math_mul(generated, inverse, transform);
    for (int i = 0; i < 16; i++) if (!isfinite(transform[i]) || fabs(transform[i]) > 1e6) return FG_MATH_SINGULAR;
    for (int i = 0; i < 16; i++) out[i] = (float)transform[i];
    return FG_MATH_OK;
}

/* Map both adjacent real cameras to their midpoint, not a predicted future pose.
   Bounded smooth FOV changes interpolate projection coefficients; aspect,
   extent, orientation-sign and off-center/jitter cuts remain guarded.
   Outputs are row-major sourceClip -> midpointClip and unchanged on rejection.
   Compute relative rigid views before projection so large world positions cancel
   in double rather than in independently rounded float view-projection matrices. */
static inline fg_math_result fg_math_interpolate(const float first[84], const float second[84],
                                                 unsigned w, unsigned h,
                                                 float first_out[16], float second_out[16]) {
    fg_math_result result = fg_math_camera(first, w, h);
    if (result != FG_MATH_OK) return result;
    result = fg_math_camera(second, w, h);
    if (result != FG_MATH_OK) return result;
    double distance2 = 0, delta[9], trace = 0, displacement[3];
    for (int r = 0; r < 3; r++) {
        double d = (double)second[47 + r * 4] - first[47 + r * 4];
        distance2 += d * d;
        displacement[r] = d * 0.5;
        for (int c = 0; c < 3; c++) {
            delta[r * 3 + c] = 0;
            for (int k = 0; k < 3; k++)
                delta[r * 3 + c] += (double)second[44 + r * 4 + k] * first[44 + c * 4 + k];
        }
        trace += delta[r * 3 + r];
    }
    if (distance2 > 64 || (trace - 1) * 0.5 < 0.9063077870366499) return FG_MATH_CUT;
    /* Each source has its own inverse projection below. Independent near/far
       ranges and bounded symmetric zoom share one exact midpoint clip basis. */
    if (fg_math_projection_rejection(first,second,1)!=FG_PROJECTION_STABLE) return FG_MATH_CUT;
    double qw = sqrt(fmax(0, 1 + trace)) * 0.5;
    double hw = sqrt((1 + qw) * 0.5);
    double x = (delta[7] - delta[5]) / (8 * qw * hw);
    double y = (delta[2] - delta[6]) / (8 * qw * hw);
    double z = (delta[3] - delta[1]) / (8 * qw * hw);
    double half[9] = {1 - 2 * (y*y + z*z), 2 * (x*y - z*hw), 2 * (x*z + y*hw),
        2 * (x*y + z*hw), 1 - 2 * (x*x + z*z), 2 * (y*z - x*hw),
        2 * (x*z - y*hw), 2 * (y*z + x*hw), 1 - 2 * (x*x + y*y)};
    double rotation[9], midpoint_projection[16];
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        rotation[r * 3 + c] = 0;
        for (int k = 0; k < 3; k++)
            rotation[r * 3 + c] += half[r * 3 + k] * first[44 + k * 4 + c];
    }
    for (int i = 0; i < 16; i++)
        midpoint_projection[i] = ((double)first[28 + i] + second[28 + i]) * 0.5;
    double transform[2][16];
    for (int side = 0; side < 2; side++) {
        const float *source = side ? second : first;
        double relative[16] = {0}, projection[16], inverse[16], projected[16];
        relative[15] = 1;
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) for (int k = 0; k < 3; k++)
                relative[r * 4 + c] += rotation[k * 3 + r] * source[44 + k * 4 + c];
            for (int k = 0; k < 3; k++)
                relative[r * 4 + 3] += rotation[k * 3 + r] * (side ? displacement[k] : -displacement[k]);
        }
        for (int i = 0; i < 16; i++) projection[i] = source[28 + i];
        if (!fg_math_inverse(projection, inverse)) return FG_MATH_SINGULAR;
        fg_math_mul(midpoint_projection, relative, projected);
        fg_math_mul(projected, inverse, transform[side]);
        for (int i = 0; i < 16; i++)
            if (!isfinite(transform[side][i]) || fabs(transform[side][i]) > 1e6) return FG_MATH_SINGULAR;
    }
    for (int i = 0; i < 16; i++) {
        first_out[i] = (float)transform[0][i];
        second_out[i] = (float)transform[1][i];
    }
    return FG_MATH_OK;
}

/* Conservative sourceClip -> generatedClip displacement, in normalized UV.
   depth_lo/hi MUST enclose EVERY shader-valid depth in the retained source, not
   a sampled estimate. [0,1] is always valid; a complete raw-D32 GPU reduction can
   narrow it. The hard cap remains an independent shader acceptance condition.

   Proof: W is affine, so its minimum over the clip box is at a corner. With
   W>0, |P_i/W-q_i| <= max|P_i-q_i*W|/min(W). The numerator is affine in the
   other two coordinates and quadratic in q_i, so its extrema occur on the four
   parallel box edges, at their endpoints or the quadratic stationary point.
   We bound the ENTIRE box, including subsequently clipped/rejected points.
   This is deliberately not a bound inferred from rendered motion samples.
   FP32 shader arithmetic gets an outward error allowance; an uncertifiable W,
   singular/nonfinite transform, or invalid depth interval keeps the hard cap. */
#define FG_MOTION_CAP 0.04f
typedef struct { float uv[2]; unsigned radius[2]; } fg_motion_bound;
static inline fg_motion_bound fg_math_motion_bound(const float transform[16],
                                                    float depth_lo, float depth_hi,
                                                    unsigned width, unsigned height) {
    fg_motion_bound out={{FG_MOTION_CAP,FG_MOTION_CAP},{0,0}};
    double matrix[16], inverse[16], sum[4]={0};
    int valid=width && height && isfinite(depth_lo) && isfinite(depth_hi) &&
        depth_lo>=0 && depth_lo<=depth_hi && depth_hi<=1;
    for (int i=0;i<16;i++) {
        matrix[i]=transform[i];
        if (!isfinite(matrix[i])) valid=0;
        sum[i/4]+=fabs(matrix[i]);
    }
    if (valid && !fg_math_inverse(matrix,inverse)) valid=0;
    double z[2]={fmax(-1,2*(double)depth_lo-1-8*FLT_EPSILON),
                 fmin(1,2*(double)depth_hi-1+8*FLT_EPSILON)};
    double wmin=DBL_MAX, error_w=32*FLT_EPSILON*(1+sum[3]);
    if (valid) for (int corner=0;corner<8;corner++) {
        double q[4]={corner&1 ? 1 : -1,corner&2 ? 1 : -1,z[(corner>>2)&1],1}, w=0;
        for (int k=0;k<4;k++) w+=matrix[12+k]*q[k];
        if (w<wmin) wmin=w;
    }
    if (valid && wmin>1e-5+error_w) for (int axis=0;axis<2;axis++) {
        /* Exact planar identity has zero analytic displacement. The extra
           radius texel below still covers UV conversion/nearest footprints. */
        if (transform[12]==0 && transform[13]==0 && transform[14]==0 && transform[15]==1 &&
            transform[axis*4]==(axis==0) && transform[axis*4+1]==(axis==1) &&
            transform[axis*4+2]==0 && transform[axis*4+3]==0) {
            out.uv[axis]=0;
            continue;
        }
        double largest=0;
        for (int edge=0;edge<4;edge++) {
            double q[4]={0,0,z[(edge>>1)&1],1};
            q[1-axis]=edge&1 ? 1 : -1;
            double a=matrix[axis*4+axis], c=matrix[12+axis], b=0, d=0;
            for (int k=0;k<4;k++) if (k!=axis) {
                b+=matrix[axis*4+k]*q[k]; d+=matrix[12+k]*q[k];
            }
            double linear=a-d;
            double n0=fabs(-c-linear+b), n1=fabs(-c+linear+b);
            largest=fmax(largest,fmax(n0,n1));
            if (c!=0) {
                double t=linear/(2*c);
                if (t>-1 && t<1) largest=fmax(largest,fabs((-c*t+linear)*t+b));
            }
        }
        double error=128*FLT_EPSILON*(1+sum[axis]+sum[3]);
        double bound=(largest+error)/(2*(wmin-error_w))+8*FLT_EPSILON;
        if (isfinite(bound) && bound<FG_MOTION_CAP)
            out.uv[axis]=fminf(FG_MOTION_CAP,nextafterf((float)bound,INFINITY));
    }
    const unsigned extent[2]={width,height};
    for (int axis=0;axis<2;axis++) {
        /* One full native-depth texel covers nearest quantization and the
           continuity footprint. All four scene-filter taps remain separately
           checked, including when scene/depth have independent extents. */
        double radius=ceil((double)out.uv[axis]*extent[axis])+1;
        out.radius[axis]=!extent[axis] ? 0 : radius>=extent[axis] ? extent[axis] : (unsigned)radius;
    }
    return out;
}

/* CPU reference of the shader's forward mapping, for synthetic golden tests. */
static inline int fg_math_project(const float transform[16], float u, float v, float depth, float out_uv[2]) {
    if (!isfinite(u) || !isfinite(v) || !isfinite(depth) || depth < 0 || depth >= 1.0f - 1e-6f) return 0;
    double clip[4] = {2 * (double)u - 1, 1 - 2 * (double)v, 2 * (double)depth - 1, 1}, p[4] = {0};
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) p[r] += transform[r * 4 + k] * clip[k];
    for (int i = 0; i < 4; i++) if (!isfinite(p[i])) return 0;
    if (!(p[3] > 1e-5) || fabs(p[2]) > p[3]) return 0;
    double x = 0.5 + 0.5 * p[0] / p[3], y = 0.5 - 0.5 * p[1] / p[3];
    if (!isfinite(x) || !isfinite(y) || x < 0 || x > 1 || y < 0 || y > 1) return 0;
    out_uv[0] = (float)x; out_uv[1] = (float)y;
    return 1;
}

/* CPU references for the shader's conservative depth/HUD fallback gates. The
   shader also checks sample borders, motion <=0.04 UV and inverse-warp residual. */
#define FG_DEPTH_EDGE 0.02f
#define FG_HUD_EDGE 0.04f
static inline int fg_math_depth_continuous(const float *depth, size_t n) {
    if (!n) return 0;
    float lo = 1, hi = 0;
    for (size_t i = 0; i < n; i++) {
        if (!isfinite(depth[i]) || depth[i] < 0 || depth[i] >= 1.0f - 1e-6f) return 0;
        if (depth[i] < lo) lo = depth[i];
        if (depth[i] > hi) hi = depth[i];
    }
    return hi - lo <= FG_DEPTH_EDGE && (1.0f - lo) <= (1.0f - hi) * 1.08f;
}

/* Interpolation compares depths after both candidates reach the midpoint.
   The common near factor cancels in near/(1-depth). Disagreeing surfaces must
   not crossfade: the nearer valid candidate occludes the farther one. A single
   valid side fills a disocclusion; zero keeps the earlier unwarped scene.
   Bit 0 selects first, bit 1 second, both bits mean an equal linear-RGB blend. */
#define FG_INTERP_DEPTH_RATIO 1.04f
static inline unsigned fg_math_interpolation_sources(int first_valid, float first_depth,
                                                     int second_valid, float second_depth) {
    first_valid = first_valid && isfinite(first_depth) && first_depth >= 0 && first_depth < 1.0f - 1e-6f;
    second_valid = second_valid && isfinite(second_depth) && second_depth >= 0 && second_depth < 1.0f - 1e-6f;
    if (!first_valid) return second_valid ? 2u : 0u;
    if (!second_valid) return 1u;
    float a = 1.0f - first_depth, b = 1.0f - second_depth;
    if (fmaxf(a, b) <= fminf(a, b) * FG_INTERP_DEPTH_RATIO) return 3u;
    return first_depth < second_depth ? 1u : 2u;
}
static inline int fg_math_hud_difference(const float scene[3], const float final[3]) {
    for (int i = 0; i < 3; i++)
        if (!isfinite(scene[i]) || fabsf(scene[i] - final[i]) > FG_HUD_EDGE) return 1;
    return 0;
}

#endif
