package com.winlator.xserver.errors;

import static com.winlator.xserver.XClientRequestHandler.RESPONSE_CODE_ERROR;

import com.winlator.xconnector.XOutputStream;
import com.winlator.xconnector.XStreamLock;
import com.winlator.xserver.XClient;

import java.io.IOException;

public class XRequestError extends Exception  {
    private final byte code;
    private final int data;

    public XRequestError(int code, int data) {
        this.code = (byte)code;
        this.data = data;
    }

    public byte getCode() {
        return code;
    }

    public int getData() {
        return data;
    }

    public void sendError(XClient client, byte opcode) throws IOException {
        sendError(client, opcode, client.getSequenceNumber(), client.getRequestData());
    }

    /** Deferred work must retain the original request identity. */
    public void sendError(XClient client, byte opcode, short sequence, byte requestData) throws IOException {
        XOutputStream outputStream = client.getOutputStream();
        try (XStreamLock lock = outputStream.lock()) {
            outputStream.writeByte(RESPONSE_CODE_ERROR);
            outputStream.writeByte(code);
            outputStream.writeShort(sequence);
            outputStream.writeInt(data);
            outputStream.writeShort(requestData);
            outputStream.writeByte(opcode);
            outputStream.writePad(21);
        }
    }
}
