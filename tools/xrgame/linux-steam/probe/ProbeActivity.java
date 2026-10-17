/* SPDX-License-Identifier: GPL-3.0-or-later */
package com.tencentmalos.linuxsteamprobe;

import android.app.Activity;
import android.os.Bundle;
import android.widget.TextView;
import java.io.*;
import java.nio.file.Files;
import java.util.concurrent.TimeUnit;
import java.util.zip.*;

/** Fixed, bounded app-domain diagnostic. No command/arguments accepted from intents. */
public final class ProbeActivity extends Activity {
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        TextView text = new TextView(this); text.setText("Checking Linux Steam execution prerequisites…");
        setContentView(text);
        new Thread(() -> {
            String result;
            try { result = probe(); } catch (Exception e) { result = "error=" + e + "\n"; }
            try { Files.write(new File(getFilesDir(), "result.txt").toPath(), result.getBytes("UTF-8")); }
            catch (Exception ignored) {}
            final String display = result;
            runOnUiThread(() -> text.setText(display));
        }, "LinuxSteamProbe").start();
    }
    private String probe() throws Exception {
        long started = android.os.SystemClock.elapsedRealtime();
        File root = new File(getFilesDir(), "root"); root.mkdirs();
        try (ZipInputStream zip = new ZipInputStream(getAssets().open("root.zip"))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null;) {
                File target = new File(root, entry.getName()).getCanonicalFile();
                if (!target.toPath().startsWith(root.getCanonicalFile().toPath())) throw new IOException("invalid asset path");
                if (entry.isDirectory()) { target.mkdirs(); continue; }
                target.getParentFile().mkdirs();
                try (OutputStream out = new FileOutputStream(target)) {
                    byte[] buf = new byte[65536];
                    for (int n; (n = zip.read(buf)) != -1;) out.write(buf, 0, n);
                }
                if (!target.setExecutable(true, true)) throw new IOException("chmod failed");
            }
        }
        for (String path : new String[]{"proc", "dev", "root", "tmp"}) new File(root, path).mkdirs();
        String nativeDir = getApplicationInfo().nativeLibraryDir;
        File log = new File(getFilesDir(), "proot.log");
        ProcessBuilder builder = new ProcessBuilder(nativeDir + "/libproot.so",
            "--kill-on-exit", "-r", root.toString(), "-w", "/", "-b", "/dev", "-b", "/proc", "/probe");
        builder.environment().remove("LD_PRELOAD");
        builder.environment().remove("LD_LIBRARY_PATH");
        builder.environment().put("PROOT_LOADER", nativeDir + "/libproot-loader.so");
        builder.environment().put("PROOT_TMP_DIR", getCacheDir().toString());
        builder.environment().put("HOME", "/root");
        builder.redirectErrorStream(true).redirectOutput(log);
        Process process = builder.start();
        boolean done = process.waitFor(35, TimeUnit.SECONDS);
        if (!done) { process.destroyForcibly(); process.waitFor(5, TimeUnit.SECONDS); }
        String clientResult = "official-client=not-staged\n";
        File client = new File(getFilesDir(), "valve/androidarm64/libsteamclient.so");
        if (client.isFile()) {
            File clientLog = new File(getFilesDir(), "client.log");
            ProcessBuilder clientBuilder = new ProcessBuilder(nativeDir + "/libclientprobe.so", client.toString());
            clientBuilder.environment().put("HOME", new File(root, "root").toString());
            clientBuilder.redirectErrorStream(true).redirectOutput(clientLog);
            Process clientProbe = clientBuilder.start();
            boolean clientDone = clientProbe.waitFor(20, TimeUnit.SECONDS);
            if (!clientDone) { clientProbe.destroyForcibly(); clientProbe.waitFor(5, TimeUnit.SECONDS); }
            clientResult = "clientExit=" + (clientDone ? clientProbe.exitValue() : "timeout") + "\n" +
                new String(Files.readAllBytes(clientLog.toPath()), "UTF-8");
        }
        return "targetSdk=" + getApplicationInfo().targetSdkVersion + "\nuid=" + android.os.Process.myUid() +
            "\nselinux=" + new String(Files.readAllBytes(new File("/proc/self/attr/current").toPath()), "UTF-8").trim() +
            "\nexit=" + (done ? process.exitValue() : "timeout") + "\n" +
            new String(Files.readAllBytes(log.toPath()), "UTF-8") + clientResult +
            "durationMs=" + (android.os.SystemClock.elapsedRealtime() - started) + "\n";
    }
}
