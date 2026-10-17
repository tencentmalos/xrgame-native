package com.winlator.xserver;

import java.util.HashMap;

/** Access is serialized with the window-manager lock, including client cleanup. */
public final class ColormapManager extends XResourceManager {
    public final Colormap defaultColormap;
    private final HashMap<Integer, Colormap> maps = new HashMap<>();

    public ColormapManager(Visual visual) {
        defaultColormap = new Colormap(IDGenerator.generate(), visual);
        maps.put(defaultColormap.id, defaultColormap);
    }

    public Colormap get(int id) { return maps.get(id); }

    public Colormap create(int id, Visual visual) {
        if (maps.containsKey(id)) return null;
        Colormap map = new Colormap(id, visual);
        maps.put(id, map);
        return map;
    }

    public void free(int id) {
        if (id == defaultColormap.id) return;
        Colormap map = maps.remove(id);
        if (map != null) triggerOnFreeResourceListener(map);
    }
}
