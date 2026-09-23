package org.quadrate.qdos;

import android.content.Context;
import android.content.res.AssetManager;
import android.hardware.Sensor;
import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * The simulator backend, told where its three stores are before SDL starts it.
 *
 * The system programs are copied out of the APK on every start, so an update
 * brings its own and drops the ones it no longer ships. The user store is
 * seeded once, as the firmware seeds its writable partition. The inbox sits
 * in external app storage, where a PC can reach it over USB.
 */
public class QdosActivity extends SDLActivity {
    private static final String TAG = "qdos";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        File system = new File(getFilesDir(), "system");
        File user = new File(getFilesDir(), "store");
        File external = getExternalFilesDir(null);
        File inbox = new File(external != null ? external : getFilesDir(), "inbox");

        try {
            delete(system);
            copyAssets("system", system);
            if (!user.exists()) {
                copyAssets("user", user);
            }

            Os.setenv("QDOS_SYSTEM_STORE", system.getPath(), true);
            Os.setenv("QDOS_STORE", user.getPath(), true);
            Os.setenv("QDOS_INBOX", inbox.getPath(), true);
        } catch (IOException | ErrnoException e) {
            Log.e(TAG, "setting up the stores", e);
        }

        super.onCreate(savedInstanceState);
    }

    /**
     * SDL reads the accelerometer at game rate on the UI thread for as long as
     * the app is in front. Nothing here turns, so it is never switched on.
     */
    private static class QuietSurface extends SDLSurface {
        QuietSurface(Context context) {
            super(context);
        }

        @Override
        protected void enableSensor(int sensortype, boolean enabled) {
            if (sensortype != Sensor.TYPE_ACCELEROMETER) {
                super.enableSensor(sensortype, enabled);
            }
        }
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new QuietSurface(context);
    }

    /** A folder in the APK and what is in it, one level deep like the store */
    private void copyAssets(String from, File to) throws IOException {
        AssetManager assets = getAssets();
        String[] names = assets.list(from);
        if (names == null) {
            return;
        }

        to.mkdirs();
        for (String name : names) {
            String path = from + "/" + name;
            String[] inside = assets.list(path);
            if (inside != null && inside.length > 0) {
                copyAssets(path, new File(to, name));
                continue;
            }

            try (InputStream in = assets.open(path);
                 OutputStream out = new FileOutputStream(new File(to, name))) {
                byte[] buf = new byte[8192];
                int n;
                while ((n = in.read(buf)) > 0) {
                    out.write(buf, 0, n);
                }
            }
        }
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                delete(child);
            }
        }
        file.delete();
    }
}
