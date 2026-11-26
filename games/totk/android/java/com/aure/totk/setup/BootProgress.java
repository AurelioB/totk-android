package com.aure.totk.setup;

import android.app.Activity;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.PopupWindow;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.text.NumberFormat;

/** Startup UI on its own Android surface, above the NativeActivity's game surface. */
public final class BootProgress {
    public interface Source {
        /** Current stage, or null after the first game frame. Fills the completed and total work counts. */
        String read(long[] counts);
    }

    private static final int REFRESH_MS = 250;
    private final Activity owner;
    private final Source source;
    private final Runnable onFinished;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Runnable tick = this::refresh;
    private final long[] counts = new long[2];
    private final NumberFormat format = NumberFormat.getPercentInstance();
    private PopupWindow popup;
    private TextView title, percent;
    private ProgressBar bar;
    private boolean finished;
    private int lastPercent = -2;

    public BootProgress(Activity owner, Source source, Runnable onFinished) {
        this.owner = owner;
        this.source = source;
        this.onFinished = onFinished;
    }

    public void resume() {
        main.removeCallbacks(tick);
        if (!finished) main.post(tick);
    }

    public void pause() {
        main.removeCallbacks(tick);
        if (popup != null) popup.dismiss();
    }

    private void refresh() {
        if (owner.isFinishing() || owner.isDestroyed()) {
            pause();
            return;
        }
        String stage = source.read(counts);
        if (stage == null) {
            finished = true;
            pause();
            popup = null;
            title = null;
            percent = null;
            bar = null;
            onFinished.run(); // The popup is already dismissed before modal UI may take focus.
            return;
        }
        View anchor = owner.getWindow().getDecorView();
        if (anchor.isAttachedToWindow() && anchor.getWidth() > 0) {
            if (popup == null) createPopup();
            String label = stage.isEmpty() ? "Starting game" : stage;
            if (!label.contentEquals(title.getText())) title.setText(label);
            int progress = counts[1] > 0 ? (int)Math.min(100, 100.0 * counts[0] / counts[1]) : -1;
            if (progress != lastPercent) {
                bar.setIndeterminate(progress < 0);
                percent.setVisibility(progress < 0 ? View.GONE : View.VISIBLE);
                if (progress >= 0) {
                    bar.setProgress(progress);
                    percent.setText(format.format(progress / 100.0));
                }
                lastPercent = progress;
            }
            if (!popup.isShowing()) {
                popup.setWidth(Math.max(1, Math.min(dp(432), anchor.getWidth() - dp(32))));
                popup.showAtLocation(anchor, Gravity.CENTER, 0, 0);
            }
        }
        main.postDelayed(tick, REFRESH_MS);
    }

    private void createPopup() {
        Theme.init(owner);
        LinearLayout content = new LinearLayout(owner);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), dp(18), dp(24), dp(20));
        GradientDrawable background = new GradientDrawable();
        background.setColor(Theme.DEEP);
        background.setCornerRadius(dp(14));
        background.setStroke(dp(1), Theme.RULE);
        content.setBackground(background);

        LinearLayout row = new LinearLayout(owner);
        row.setGravity(Gravity.CENTER_VERTICAL);
        content.addView(row, ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        title = new TextView(owner);
        title.setTypeface(Theme.semibold);
        title.setTextSize(18);
        title.setTextColor(Theme.TEXT);
        title.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        row.addView(title, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        percent = new TextView(owner);
        percent.setTypeface(Theme.semibold);
        percent.setTextSize(16);
        percent.setTextColor(Theme.ZONAI);
        percent.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        LinearLayout.LayoutParams number = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        number.setMarginStart(dp(12));
        row.addView(percent, number);

        bar = new ProgressBar(owner, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(100);
        bar.setProgressTintList(ColorStateList.valueOf(Theme.ZONAI));
        bar.setIndeterminateTintList(ColorStateList.valueOf(Theme.ZONAI));
        bar.setProgressBackgroundTintList(ColorStateList.valueOf(Theme.TRACK));
        LinearLayout.LayoutParams track = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(8));
        track.topMargin = dp(12);
        content.addView(bar, track);

        popup = new PopupWindow(content, ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, false);
        popup.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
        popup.setTouchable(false);
        popup.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        popup.setAnimationStyle(0);
    }

    private int dp(int value) {
        return Math.round(value * owner.getResources().getDisplayMetrics().density);
    }
}
