package com.winlator.xserver.requests;

import com.winlator.xconnector.XInputStream;
import com.winlator.xconnector.XOutputStream;
import com.winlator.xconnector.XStreamLock;
import com.winlator.xserver.Colormap;
import com.winlator.xserver.Visual;
import com.winlator.xserver.XClient;
import com.winlator.xserver.errors.*;

import java.io.IOException;
import java.util.Locale;

/** Core X11 color operations for the server's read-only RGB TrueColor visual. */
public final class ColorRequests {
    private static Colormap map(XClient client, int id) throws XRequestError {
        Colormap map = client.xServer.colormapManager.get(id);
        if (map == null) throw new XRequestError(12, id); // BadColor
        return map;
    }

    private static void header(XClient client, XOutputStream out, int words) throws IOException {
        out.writeByte((byte)1);
        out.writeByte((byte)0);
        out.writeShort(client.getSequenceNumber());
        out.writeInt(words);
    }

    private static void rgb(Colormap map, int pixel, XOutputStream out) throws IOException {
        out.writeShort((short)map.red(pixel));
        out.writeShort((short)map.green(pixel));
        out.writeShort((short)map.blue(pixel));
    }

    public static void create(XClient client, XInputStream in) throws XRequestError {
        int id = in.readInt(), window = in.readInt(), visualId = in.readInt();
        if (!client.isValidResourceId(id)) throw new BadIdChoice(id);
        if (client.xServer.windowManager.getWindow(window) == null) throw new BadWindow(window);
        Visual visual = client.xServer.pixmapManager.getVisual(visualId);
        if (visual == null || visual.visualClass != 4) throw new BadMatch();
        int alloc = client.getRequestData() & 255;
        if (alloc > 1) throw new BadValue(alloc);
        if (alloc == 1) throw new BadMatch(); // TrueColor cells cannot be writable.
        Colormap map = client.xServer.colormapManager.create(id, visual);
        if (map == null) throw new BadIdChoice(id);
        client.registerAsOwnerOfResource(map);
    }

    public static void free(XClient client, XInputStream in) throws XRequestError {
        int id = in.readInt();
        map(client, id);
        client.xServer.colormapManager.free(id);
    }

    public static void alloc(XClient client, XInputStream in, XOutputStream out) throws IOException, XRequestError {
        Colormap map = map(client, in.readInt());
        int pixel = map.pixel(in.readUnsignedShort(), in.readUnsignedShort(), in.readUnsignedShort());
        in.skip(2);
        try (XStreamLock ignored = out.lock()) {
            header(client, out, 0);
            rgb(map, pixel, out);
            out.writePad(2);
            out.writeInt(pixel);
            out.writePad(12);
        }
    }

    // A deliberately small server color database. Unknown names return BadName;
    // never silently turn an unrecognized color into black. Not CSS color values.
    private static int named(String raw) throws XRequestError {
        String name = raw.toLowerCase(Locale.ROOT).replace(" ", "");
        switch (name) {
            case "black": return 0x000000;
            case "white": return 0xffffff;
            case "red": return 0xff0000;
            case "green": return 0x00ff00;
            case "blue": return 0x0000ff;
            case "yellow": return 0xffff00;
            case "cyan": return 0x00ffff;
            case "magenta": return 0xff00ff;
            case "gray": case "grey": return 0xbebebe;
            case "darkgray": case "darkgrey": return 0xa9a9a9;
            case "lightgray": case "lightgrey": return 0xd3d3d3;
            case "navy": case "navyblue": return 0x000080;
        }
        if (name.matches("gr[ae]y[0-9]{1,3}")) {
            int percent = Integer.parseInt(name.substring(4));
            if (percent <= 100) return ((percent * 255 + 50) / 100) * 0x010101;
        }
        throw new XRequestError(15, 0); // BadName
    }

    public static void named(XClient client, XInputStream in, XOutputStream out, boolean allocate)
            throws IOException, XRequestError {
        Colormap map = map(client, in.readInt());
        int length = in.readUnsignedShort();
        in.skip(2);
        if (((length + 3) & ~3) > client.getRemainingRequestLength()) throw new XRequestError(16, 0);
        int exact = named(in.readString8(length));
        int red = ((exact >> 16) & 255) * 257, green = ((exact >> 8) & 255) * 257, blue = (exact & 255) * 257;
        int pixel = map.pixel(red, green, blue);
        try (XStreamLock ignored = out.lock()) {
            header(client, out, 0);
            if (allocate) out.writeInt(pixel);
            out.writeShort((short)red);
            out.writeShort((short)green);
            out.writeShort((short)blue);
            rgb(map, pixel, out);
            out.writePad(allocate ? 8 : 12);
        }
    }

    public static void query(XClient client, XInputStream in, XOutputStream out) throws IOException, XRequestError {
        Colormap map = map(client, in.readInt());
        int count = client.getRemainingRequestLength() / 4;
        try (XStreamLock ignored = out.lock()) {
            header(client, out, count * 2);
            out.writeShort((short)count);
            out.writePad(22);
            for (int i = 0; i < count; i++) {
                rgb(map, in.readInt(), out);
                out.writePad(2);
            }
        }
    }

    public static void freeColors(XClient client, XInputStream in) throws XRequestError {
        map(client, in.readInt());
        // TrueColor has no writable cells and keeps no per-client allocations.
        client.skipRequest();
    }
}
