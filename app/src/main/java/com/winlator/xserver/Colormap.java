package com.winlator.xserver;

/** Read-only TrueColor map. Pixel values follow the visual's masks. */
public final class Colormap extends XResource {
    public final Visual visual;

    public Colormap(int id, Visual visual) {
        super(id);
        this.visual = visual;
    }

    private static int encode(int value, int mask) {
        int shift = Integer.numberOfTrailingZeros(mask);
        int levels = mask >>> shift;
        return (int)((value * (long)levels + 32767) / 65535) << shift;
    }

    private static int decode(int pixel, int mask) {
        int shift = Integer.numberOfTrailingZeros(mask);
        int levels = mask >>> shift;
        return (int)(((pixel & mask) >>> shift) * 65535L / levels);
    }

    public int pixel(int red, int green, int blue) {
        return encode(red, visual.redMask) | encode(green, visual.greenMask) | encode(blue, visual.blueMask);
    }

    public int red(int pixel) { return decode(pixel, visual.redMask); }
    public int green(int pixel) { return decode(pixel, visual.greenMask); }
    public int blue(int pixel) { return decode(pixel, visual.blueMask); }
}
