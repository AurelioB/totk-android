package com.aure.totk.setup;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.util.TypedValue;
import android.widget.TextView;

/** The menu look (approved mockups, .scratch/release/mockups/theme.css): dark mineral glass, warm parchment text, a
 * lime selection glow with diamond end-caps, thin rules with diamond joints and Zonai-teal accents. Original art only;
 * everything is drawn with Canvas. The one bundled font family is Noto Sans (assets/fonts/, OFL).
 *
 * Sizes are given in the mockups' 1920 x 1080 design pixels and scaled to the window by {@link #scale}, so a 1080p
 * handheld shows the screens exactly as approved and other sizes keep the proportions (letterboxed).
 *
 * The colours below are the approved ones. Another game's build (GameBuild) names its own two accents and a hue for
 * the green-tinted surfaces, rules and dim accents, which {@link #tint} moves there; warm colours stay as they are. */
final class Theme {
    private Theme() { }
    static final int INK = tint(0xff0a100e), DEEP = tint(0xff0f1714), SLATE = tint(0xff17221e), GLASS = tint(0xd1141f1b), GLASS2 = tint(0xc71c2924);
    static final int RULE = tint(0xff3a4a41), RULE_SOFT = tint(0x38788c78), NOTCH = tint(0xff5d6b55), TRACK = tint(0xff2b3731), PIP = tint(0xff3a463f);
    static final int TEXT = 0xffece5cf, TEXT2 = 0xffb3b39f, TEXT3 = tint(0xff7f8677), TEXT_LIT = 0xfffbf6df;
    static final int SEL = GameBuild.SEL, SEL_GLOW = withAlpha(SEL, 0x6b), SEL_FILL = withAlpha(SEL, 0x21), SEL_DIM = tint(0xff8fa65a);
    static final int ZONAI = GameBuild.ACCENT, ZONAI_GLOW = withAlpha(ZONAI, 0x59), AMBER = 0xffe3ad55, WARN = 0xffe07a5a;
    static final int GOLD = 0xffe1c070, GOLD_DIM = 0xffc8a85a, KEY = 0xffe9e3cc, KEY_INK = tint(0xff121a17);
    static final float DESIGN_W = 1920, DESIGN_H = 1080;

    static Typeface regular, semibold;
    /** Device pixels per design pixel for the current window. */
    static float scale = 1;

    static void init(Context context) {
        if (regular != null) return;
        regular = font(context, "NotoSans-Regular.ttf", Typeface.SANS_SERIF);
        semibold = font(context, "NotoSans-SemiBold.ttf", Typeface.create(Typeface.SANS_SERIF, Typeface.BOLD));
    }
    private static Typeface font(Context context, String name, Typeface fallback) {
        try { return Typeface.createFromAsset(context.getAssets(), "fonts/" + name); }
        catch (RuntimeException missing) { return fallback; }
    }
    /** Fit the 1920 x 1080 design into a window of this size. */
    static void fit(int width, int height) { scale = Math.max(.25f, Math.min(width / DESIGN_W, height / DESIGN_H)); }
    static float f(float design) { return design * scale; }
    static int u(float design) { return Math.round(design * scale); }

    static TextView text(Context context, String value, boolean bold, float size, int color) {
        TextView view = new TextView(context);
        view.setText(value); view.setTypeface(bold ? semibold : regular); view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_PX, f(size));
        view.setIncludeFontPadding(false);
        view.setLetterSpacing(0.01f);
        return view;
    }
    /** Body copy: the mockups' 1.55 line height. */
    static TextView paragraph(Context context, String value, float size, int color) {
        TextView view = text(context, value, false, size, color);
        view.setLineSpacing(0, 1.4f);
        return view;
    }

    static void diamond(Canvas canvas, float x, float y, float radius, Paint paint) {
        Path path = new Path();
        path.moveTo(x, y - radius); path.lineTo(x + radius, y); path.lineTo(x, y + radius); path.lineTo(x - radius, y); path.close();
        canvas.drawPath(path, paint);
    }
    static int alpha(int color, float alpha) { return (color & 0x00ffffff) | (Math.round(alpha * 255) << 24); }
    /** The colour with this alpha byte (the mockups' 8-digit values, exactly). */
    static int withAlpha(int color, int alpha) { return (color & 0x00ffffff) | (alpha << 24); }

    /** A design colour in this build's hue. The design's surfaces, rules and dim accents are all green-tinted (hues
     * above 60 to 180 degrees, olive to teal); with GameBuild.SURFACE_HUE set they turn to hues from 14 degrees below it (olive)
     * to 6 above (teal), keeping alpha, saturation and luma (Rec. 709), so every contrast stays as designed. Greys
     * and warm colours (text, gold, amber, warnings) are outside that range and come back unchanged. */
    static int tint(int color) {
        if (GameBuild.SURFACE_HUE < 0) return color;
        int r = color >> 16 & 255, g = color >> 8 & 255, b = color & 255, max = Math.max(r, Math.max(g, b)), min = Math.min(r, Math.min(g, b));
        if (max == min) return color;
        float d = max - min, hue = max == r ? 60 * (g - b) / d : max == g ? 120 + 60 * (b - r) / d : 240 + 60 * (r - g) / d;
        if (hue <= 60 || hue > 180) return color;
        hue = (GameBuild.SURFACE_HUE - 14 + (hue - 60) / 6 + 360) % 360;
        float[] out = new float[3];
        for (int i = 0; i < 3; i++) {   // HSV to RGB with the same value and chroma: channel n is max - d * clamp(min(k, 4 - k))
            float k = (5 - 2 * i + hue / 60) % 6;
            out[i] = max - d * Math.max(0, Math.min(1, Math.min(k, 4 - k)));
        }
        float gain = (.2126f * r + .7152f * g + .0722f * b) / (.2126f * out[0] + .7152f * out[1] + .0722f * out[2]);
        return (color & 0xff000000) | Math.min(255, Math.round(out[0] * gain)) << 16 | Math.min(255, Math.round(out[1] * gain)) << 8
                | Math.min(255, Math.round(out[2] * gain));
    }

    /** The selection look (.row.sel): a lime fill fading to the right, a 2 px lime outline with a soft glow, and a
     * glowing diamond at each end of the row's centre line. */
    static void selection(Canvas c, RectF box, float radius, Paint paint) {
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new android.graphics.LinearGradient(box.left, 0, box.right, 0, new int[]{SEL_FILL, withAlpha(SEL, 0x08), 0},
                new float[]{0, .7f, 1}, android.graphics.Shader.TileMode.CLAMP));
        c.drawRoundRect(box, radius, radius, paint);
        paint.setShader(null);
        paint.setStyle(Paint.Style.STROKE); paint.setStrokeWidth(f(2)); paint.setColor(SEL);
        paint.setShadowLayer(f(13), 0, 0, SEL_GLOW);
        c.drawRoundRect(box.left + f(1), box.top + f(1), box.right - f(1), box.bottom - f(1), radius, radius, paint);
        paint.setStyle(Paint.Style.FILL); paint.setShadowLayer(f(6), 0, 0, SEL_GLOW);
        diamond(c, box.left, box.centerY(), f(7), paint);
        diamond(c, box.right, box.centerY(), f(7), paint);
        paint.clearShadowLayer();
    }
}
