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
 * handheld shows the screens exactly as approved and other sizes keep the proportions (letterboxed). */
final class Theme {
    private Theme() { }
    static final int INK = 0xff0a100e, DEEP = 0xff0f1714, SLATE = 0xff17221e, GLASS = 0xd1141f1b, GLASS2 = 0xc71c2924;
    static final int RULE = 0xff3a4a41, RULE_SOFT = 0x38788c78, NOTCH = 0xff5d6b55, TRACK = 0xff2b3731, PIP = 0xff3a463f;
    static final int TEXT = 0xffece5cf, TEXT2 = 0xffb3b39f, TEXT3 = 0xff7f8677, TEXT_LIT = 0xfffbf6df;
    static final int SEL = 0xffd4ea74, SEL_GLOW = 0x6bd4ea74, SEL_FILL = 0x21d4ea74, SEL_DIM = 0xff8fa65a;
    static final int ZONAI = 0xff57dfbf, ZONAI_GLOW = 0x5957dfbf, AMBER = 0xffe3ad55, WARN = 0xffe07a5a;
    static final int GOLD = 0xffe1c070, GOLD_DIM = 0xffc8a85a, KEY = 0xffe9e3cc, KEY_INK = 0xff121a17;
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

    /** The selection look (.row.sel): a lime fill fading to the right, a 2 px lime outline with a soft glow, and a
     * glowing diamond at each end of the row's centre line. */
    static void selection(Canvas c, RectF box, float radius, Paint paint) {
        paint.setStyle(Paint.Style.FILL);
        paint.setShader(new android.graphics.LinearGradient(box.left, 0, box.right, 0, new int[]{SEL_FILL, 0x08d4ea74, 0},
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
