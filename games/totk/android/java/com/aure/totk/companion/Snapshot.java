package com.aure.totk.companion;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

/** Immutable by convention: parsed JSON is owned by the poller and never mutated after publication. */
public final class Snapshot {
    public static final String[] CATEGORIES = {"weapons", "bows", "shields", "armor", "materials", "food", "zonai", "keyItems"};
    public final JSONObject root, player;
    public final String status, fingerprint;
    /** What an item action can change or depend on; a shown action result clears when this changes. */
    public final String actionKey;
    public final long seq;

    private Snapshot(JSONObject root) {
        this.root = root;
        player = object(root, "player");
        status = root.optString("status", "starting");
        seq = root.optLong("seq", 0);
        // seq is a poll sequence, not a content generation. Idle screens must not redraw at 8 Hz.
        JSONObject content = new JSONObject();
        for (java.util.Iterator<String> it = root.keys(); it.hasNext();) {
            String key = it.next();
            if (!key.equals("seq")) put(content, key, root.opt(key));
        }
        fingerprint = content.toString();
        actionKey = status + "#" + root.opt("pouchGen") + "#" + root.opt("partition") + "#" + root.opt("actionState")
                + "#" + root.opt("pouch");
    }

    public static Snapshot parse(String text, Snapshot previous, boolean full) throws JSONException {
        JSONObject value = new JSONObject(text == null ? "{}" : text);
        if (value.optInt("schema", 1) != 1) put(value, "status", "unsupported");
        if (!full && previous != null && "ready".equals(previous.status) && "ready".equals(value.optString("status"))) {
            for (String key : new String[]{"pouch", "partition", "pouchGen", "actionState", "slots", "abilities", "sages", "map"}) {
                if (!value.has(key) && previous.root.has(key)) put(value, key, previous.root.opt(key));
            }
        }
        return new Snapshot(value);
    }

    public static Snapshot waiting() {
        return new Snapshot(new JSONObject());
    }

    static void put(JSONObject object, String key, Object value) {
        try { object.put(key, value); } catch (JSONException ignored) { /* fixed keys and finite source values */ }
    }

    public static JSONObject object(JSONObject value, String key) {
        JSONObject result = value.optJSONObject(key);
        return result == null ? new JSONObject() : result;
    }

    public static JSONArray array(JSONObject value, String key) {
        JSONArray result = value.optJSONArray(key);
        return result == null ? new JSONArray() : result;
    }

    public JSONArray items(int category) {
        JSONArray items = array(object(root, "pouch"), CATEGORIES[category]);
        if (category != 1) return items;
        JSONArray combined = new JSONArray();
        for (int i = 0; i < items.length(); i++) combined.put(items.opt(i));
        JSONArray arrows = array(object(root, "pouch"), "arrows");
        for (int i = 0; i < arrows.length(); i++) combined.put(arrows.opt(i));
        return combined;
    }

    /** The pouch key an items(category) index came from; the Bows view lists bows, then arrows. */
    public String itemCategory(int category, int index) {
        if (category != 1) return CATEGORIES[category];
        return index < array(object(root, "pouch"), "bows").length() ? "bows" : "arrows";
    }

    public double coordinate(int axis) {
        JSONArray pos = player.optJSONArray("pos");
        return pos == null ? Double.NaN : pos.optDouble(axis, Double.NaN);
    }
}
