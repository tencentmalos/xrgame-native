package com.winlator.xserver.requests;

import static com.winlator.xserver.XClientRequestHandler.RESPONSE_CODE_SUCCESS;

import com.winlator.xconnector.XInputStream;
import com.winlator.xconnector.XOutputStream;
import com.winlator.xconnector.XStreamLock;
import com.winlator.xserver.Atom;
import com.winlator.xserver.XClient;
import com.winlator.xserver.errors.BadAtom;
import com.winlator.xserver.errors.XRequestError;

import java.io.IOException;

public abstract class AtomRequests {
    public static void internAtom(XClient client, XInputStream inputStream, XOutputStream outputStream) throws IOException, XRequestError {
        boolean onlyIfExists = client.getRequestData() == 1;
        int length = inputStream.readShort() & 0xffff;
        inputStream.skip(2);
        String name = inputStream.readString8(length);
        int id = onlyIfExists ? Atom.getId(name) : Atom.internAtom(name);
        // OnlyIfExists is a lookup: an unknown name returns None, not BadAtom.
        if (onlyIfExists && id < 0) id = 0;

        try (XStreamLock lock = outputStream.lock()) {
            outputStream.writeByte(RESPONSE_CODE_SUCCESS);
            outputStream.writeByte((byte)0);
            outputStream.writeShort(client.getSequenceNumber());
            outputStream.writeInt(0);
            outputStream.writeInt(id);
            outputStream.writePad(20);
        }
    }

    public static void getAtomName(XClient client, XInputStream inputStream, XOutputStream outputStream) throws XRequestError, IOException {
        int id = inputStream.readInt();
        if (!Atom.isValid(id)) {
            throw new BadAtom(id);
        }
        String name = Atom.getName(id);
        short length = (short) name.length();
        try (XStreamLock lock = outputStream.lock()){
            outputStream.writeByte(RESPONSE_CODE_SUCCESS);
            outputStream.writeByte((byte) 0);
            outputStream.writeShort(client.getSequenceNumber());
            outputStream.writeInt((((-length) & 3) + length) / 4);
            outputStream.writeShort((short) length);
            outputStream.writePad(22);
            outputStream.writeString8(name);
        }
    }

}
