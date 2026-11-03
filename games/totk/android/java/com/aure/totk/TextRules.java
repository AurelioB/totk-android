package com.aure.totk;

/** What the game's software keyboard (nn::swkbd) accepts, applied while the player types in TextEntry's dialog. The
 * runtime checks the answer again (runtime/swkbd_host.c allowed / clean): at most maxUnits UTF-16 units, never half a
 * surrogate pair; no control characters; none of the keys the game disabled (bit 1 space, 2 '@', 3 '%', 4 '/', 5 '\',
 * 6 digits); the number pad takes digits only. OK needs at least minUnits. Plain Java, so the host tests run it
 * (tests/test_android_text_entry.py). */
public final class TextRules {
    public final int maxUnits, minUnits, disabled;
    public final boolean digitsOnly;

    public TextRules(int maxUnits, int minUnits, boolean digitsOnly, int disabled) {
        this.maxUnits = Math.max(1, maxUnits);
        this.minUnits = Math.max(0, Math.min(minUnits, this.maxUnits));
        this.digitsOnly = digitsOnly;
        this.disabled = disabled;
    }
    private boolean off(int bit) { return (disabled & 1 << bit) != 0; }
    /** One UTF-16 unit outside a surrogate pair. */
    public boolean allowed(char c) {
        boolean digit = c >= '0' && c <= '9';
        if (digitsOnly && !digit) return false;
        if (c < 0x20 || c == 0x7f || Character.isSurrogate(c)) return false;
        if ((off(1) && c == ' ') || (off(2) && c == '@') || (off(3) && c == '%')) return false;
        return !((off(4) && c == '/') || (off(5) && c == '\\') || (off(6) && digit));
    }
    /** text without the characters the game refuses, cut to at most room units (a pair that does not fit whole is dropped). */
    public String clean(CharSequence text, int room) {
        StringBuilder out = new StringBuilder();
        int n = text == null ? 0 : text.length();
        for (int i = 0; i < n; i++) {
            char c = text.charAt(i);
            boolean pair = Character.isHighSurrogate(c) && i + 1 < n && Character.isLowSurrogate(text.charAt(i + 1));
            if (!pair && !allowed(c)) continue;
            if (out.length() + (pair ? 2 : 1) > room) break;
            out.append(c);
            if (pair) out.append(text.charAt(++i));
        }
        return out.toString();
    }
    public String clean(CharSequence text) { return clean(text, maxUnits); }
    /** Whether OK may send this text. */
    public boolean acceptable(CharSequence text) {
        int n = text == null ? 0 : text.length();
        return n >= minUnits && n <= maxUnits && clean(text).length() == n;
    }
}
