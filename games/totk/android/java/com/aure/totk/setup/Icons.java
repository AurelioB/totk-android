package com.aure.totk.setup;

import android.graphics.Path;
import android.graphics.RectF;
import java.util.HashMap;
import java.util.Map;

/** The menu's line icons (original, 30 x 30 units, drawn with a 1.8-unit round stroke), kept as SVG path data and
 * turned into Paths by a small parser (M L H V C S A Z, absolute and relative, implicit repeats). */
final class Icons {
    private Icons() { }
    static final float BOX = 30, STROKE = 1.8f;
    private static final Map<String, String> DATA = new HashMap<>();
    static {
        DATA.put("perf", "M4 18a10 10 0 1 1 22 0M15 18l6-7M13 18a2 2 0 1 0 4 0a2 2 0 1 0-4 0");
        DATA.put("picture", "M5 6h20a2 2 0 0 1 2 2v13a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2zM3 20l7-7 5 5 4-4 8 7M18 11a2 2 0 1 0 4 0a2 2 0 1 0-4 0");
        DATA.put("screen", "M5 7h20a3 3 0 0 1 3 3v9a3 3 0 0 1-3 3H5a3 3 0 0 1-3-3v-9a3 3 0 0 1 3-3zM9 26h12M22.5 14.5a1.5 1.5 0 1 0 3 0a1.5 1.5 0 1 0-3 0");
        DATA.put("controls", "M7 10h16a5 5 0 0 1 4.8 6.3l-1.6 6a3 3 0 0 1-5.2 1.1L18 20h-6l-3 3.4a3 3 0 0 1-5.2-1.1l-1.6-6A5 5 0 0 1 7 10z"
                + "M9 14v4M7 16h4M20 15a1 1 0 1 0 2 0a1 1 0 1 0-2 0M22 18a1 1 0 1 0 2 0a1 1 0 1 0-2 0");
        DATA.put("audio", "M5 12h5l6-5v16l-6-5H5zM20 11a5 5 0 0 1 0 8M23 8a9 9 0 0 1 0 14");
        DATA.put("play", "M10 6l14 9-14 9z");
        DATA.put("data", "M5 7a10 3.5 0 1 0 20 0a10 3.5 0 1 0-20 0M5 7v16c0 2 4.5 3.5 10 3.5S25 25 25 23V7M5 15c0 2 4.5 3.5 10 3.5S25 17 25 15");
        DATA.put("code", "M11 8l-7 7 7 7M19 8l7 7-7 7M17 6l-4 18");
        DATA.put("check", "M6 15.5l6 6L24 9");
        DATA.put("about", "M4 15a11 11 0 1 0 22 0a11 11 0 1 0-22 0M15 13v8M15 9v.5");
        DATA.put("gfx", "M10 15a5 5 0 1 0 10 0a5 5 0 1 0-10 0M15 3v3M15 24v3M3 15h3M24 15h3M6.5 6.5l2.1 2.1M21.4 21.4l2.1 2.1M6.5 23.5l2.1-2.1M21.4 8.6l2.1-2.1");
        DATA.put("key", "M4 15a5 5 0 1 0 10 0a5 5 0 1 0-10 0M14 15h13M22 15v4M26 15v3");
        DATA.put("cart", "M9 4h12a2 2 0 0 1 2 2v20H7V6a2 2 0 0 1 2-2zM11 9h8M11 13h8M12 22h6");
        DATA.put("refresh", "M24.5 15a9.5 9.5 0 1 1-2.8-6.7M22 3.5v5.3h-5.3");
        DATA.put("trash", "M6 9h18M12 9V6h6v3M8 9l1.5 16h11L22 9M13 13v8M17 13v8");
        DATA.put("chip", "M9 9h12v12H9zM12 9V5M18 9V5M12 21v4M18 21v4M9 12H5M9 18H5M21 12h4M21 18h4");
        DATA.put("globe", "M4 15a11 11 0 1 0 22 0a11 11 0 1 0-22 0M4 15h22M15 4c-4.5 4-4.5 18 0 22M15 4c4.5 4 4.5 18 0 22");
        DATA.put("book", "M6 5h13l5 5v15H6zM19 5v5h5M10 15h10M10 19h7");
        DATA.put("folder", "M4 8a2 2 0 0 1 2-2h6l3 3h9a2 2 0 0 1 2 2v11a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2z");
        DATA.put("back", "M18 7l-8 8 8 8");
        DATA.put("save", "M7 4h13l5 5v16a1 1 0 0 1-1 1H7a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1zM10 4v6h9V4M10 26v-8h10v8");
        DATA.put("export", "M15 19V5M9 10l6-6 6 6M5 18v5a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-5");
        DATA.put("import", "M15 4v14M9 12l6 6 6-6M5 18v5a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-5");
        DATA.put("clock", "M4 15a11 11 0 1 0 22 0a11 11 0 1 0-22 0M15 8v7l5 3");
        DATA.put("sd", "M10 4h10l5 5v16a1 1 0 0 1-1 1H10a1 1 0 0 1-1-1V5a1 1 0 0 1 1-1zM13 8v4M16.5 8v4M20 9v3");
    }
    private static final Map<String, Path> CACHE = new HashMap<>();

    /** The icon's outline in 30-unit space (shared; do not modify). */
    static Path get(String name) {
        Path path = CACHE.get(name);
        if (path == null) {
            String data = DATA.get(name);
            if (data == null) throw new IllegalArgumentException(name);
            path = parse(data);
            CACHE.put(name, path);
        }
        return path;
    }

    private static final class Reader {
        final String s; int i;
        Reader(String s) { this.s = s; }
        void skip() { while (i < s.length() && (s.charAt(i) == ' ' || s.charAt(i) == ',')) i++; }
        boolean number() {
            skip();
            if (i >= s.length()) return false;
            char ch = s.charAt(i);
            return ch == '-' || ch == '.' || Character.isDigit(ch);
        }
        float next() {
            skip();
            int start = i;
            if (s.charAt(i) == '-') i++;
            boolean dot = false;
            while (i < s.length()) {
                char ch = s.charAt(i);
                if (Character.isDigit(ch)) i++;
                else if (ch == '.' && !dot) { dot = true; i++; }
                else break;
            }
            return Float.parseFloat(s.substring(start, i));
        }
        boolean flag() { skip(); return s.charAt(i++) == '1'; }
    }

    static Path parse(String data) {
        Path path = new Path();
        Reader r = new Reader(data);
        float x = 0, y = 0, startX = 0, startY = 0, ctrlX = 0, ctrlY = 0;
        char command = 0, previous = 0;
        while (true) {
            r.skip();
            if (r.i >= data.length()) break;
            char ch = data.charAt(r.i);
            if (Character.isLetter(ch)) { command = ch; r.i++; }
            else if (command == 'M') command = 'L';   // implicit lineto after a moveto
            else if (command == 'm') command = 'l';
            boolean rel = Character.isLowerCase(command);
            float ox = rel ? x : 0, oy = rel ? y : 0;
            switch (Character.toUpperCase(command)) {
                case 'M': x = ox + r.next(); y = oy + r.next(); path.moveTo(x, y); startX = x; startY = y; break;
                case 'L': x = ox + r.next(); y = oy + r.next(); path.lineTo(x, y); break;
                case 'H': x = ox + r.next(); path.lineTo(x, y); break;
                case 'V': y = oy + r.next(); path.lineTo(x, y); break;
                case 'C': {
                    float x1 = ox + r.next(), y1 = oy + r.next(), x2 = ox + r.next(), y2 = oy + r.next();
                    x = ox + r.next(); y = oy + r.next();
                    path.cubicTo(x1, y1, x2, y2, x, y); ctrlX = x2; ctrlY = y2;
                    break;
                }
                case 'S': {
                    boolean smooth = "CcSs".indexOf(previous) >= 0;
                    float x1 = smooth ? 2 * x - ctrlX : x, y1 = smooth ? 2 * y - ctrlY : y;
                    float x2 = ox + r.next(), y2 = oy + r.next();
                    x = ox + r.next(); y = oy + r.next();
                    path.cubicTo(x1, y1, x2, y2, x, y); ctrlX = x2; ctrlY = y2;
                    break;
                }
                case 'A': {
                    float rx = r.next(), ry = r.next();
                    r.next();   // x-axis rotation: every icon uses 0
                    boolean large = r.flag(), sweep = r.flag();
                    float ex = ox + r.next(), ey = oy + r.next();
                    arc(path, x, y, ex, ey, rx, ry, large, sweep);
                    x = ex; y = ey;
                    break;
                }
                case 'Z': path.close(); x = startX; y = startY; break;
                default: throw new IllegalArgumentException("path command " + command);
            }
            previous = command;
            if (Character.toUpperCase(command) == 'Z') { command = 0; continue; }
            if (!r.number()) continue;
        }
        return path;
    }

    /** SVG endpoint arc (no rotation) to Path.arcTo through the centre parameterisation (SVG 1.1 appendix F.6.5). */
    private static void arc(Path path, float x1, float y1, float x2, float y2, float rx, float ry, boolean large, boolean sweep) {
        if (rx == 0 || ry == 0) { path.lineTo(x2, y2); return; }
        double dx = (x1 - x2) / 2.0, dy = (y1 - y2) / 2.0;
        double lambda = dx * dx / (rx * rx) + dy * dy / (ry * ry);
        if (lambda > 1) { rx *= Math.sqrt(lambda); ry *= Math.sqrt(lambda); }
        double num = rx * rx * ry * ry - rx * rx * dy * dy - ry * ry * dx * dx, den = rx * rx * dy * dy + ry * ry * dx * dx;
        double k = Math.sqrt(Math.max(0, num / den)) * (large == sweep ? -1 : 1);
        double cxp = k * rx * dy / ry, cyp = -k * ry * dx / rx;
        double cx = cxp + (x1 + x2) / 2.0, cy = cyp + (y1 + y2) / 2.0;
        double start = Math.atan2((dy - cyp) / ry, (dx - cxp) / rx);
        double end = Math.atan2((-dy - cyp) / ry, (-dx - cxp) / rx);
        double delta = end - start;
        if (sweep && delta < 0) delta += 2 * Math.PI;
        if (!sweep && delta > 0) delta -= 2 * Math.PI;
        path.arcTo(new RectF((float) (cx - rx), (float) (cy - ry), (float) (cx + rx), (float) (cy + ry)),
                (float) Math.toDegrees(start), (float) Math.toDegrees(delta), false);
    }
}
