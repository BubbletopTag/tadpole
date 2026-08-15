package org.tadpole.hello;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;

/* Not a hello-world. This is the ABI probe.
 *
 * The one question that decides whether Tadpole can run the LeapPad's own
 * ARM32 binaries on a given phone is whether that phone has a 32-bit userspace
 * at all — and the answer is not the SoC's. A Cortex-A78 implements AArch32 at
 * EL0; a ROM built 64-bit-only will still have no /system/bin/linker and an
 * empty ro.product.cpu.abilist32, and nothing 32-bit will start.
 *
 * So this prints the properties, and it also prints whether the 32-bit dynamic
 * linker actually exists on disk, because the property is a claim and the file
 * is a fact. Both go to the screen and to logcat under the tag "tadpole", so
 * `adb logcat -s tadpole` gets the answer off a device you cannot see.
 */
public class HelloActivity extends Activity {
    static final String TAG = "tadpole";

    private String probe() {
        StringBuilder b = new StringBuilder();
        b.append("Build.SUPPORTED_ABIS    ").append(join(Build.SUPPORTED_ABIS)).append('\n');
        b.append("SUPPORTED_32_BIT_ABIS   ").append(join(Build.SUPPORTED_32_BIT_ABIS)).append('\n');
        b.append("SUPPORTED_64_BIT_ABIS   ").append(join(Build.SUPPORTED_64_BIT_ABIS)).append('\n');
        b.append("CPU_ABI                 ").append(Build.CPU_ABI).append('\n');
        b.append("SDK_INT                 ").append(Build.VERSION.SDK_INT).append('\n');
        b.append("MODEL                   ").append(Build.MODEL).append('\n');
        b.append("HARDWARE                ").append(Build.HARDWARE).append('\n');
        b.append('\n');
        b.append("/system/bin/linker      ")
         .append(new java.io.File("/system/bin/linker").exists() ? "PRESENT (32-bit userspace)"
                                                                 : "ABSENT (64-bit only)")
         .append('\n');
        b.append("/system/bin/linker64    ")
         .append(new java.io.File("/system/bin/linker64").exists() ? "present" : "absent")
         .append('\n');
        b.append("this process is         ")
         .append(System.getProperty("os.arch")).append('\n');
        return b.toString();
    }

    private static String join(String[] a) {
        if (a == null || a.length == 0) return "(none)";
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < a.length; i++) { if (i > 0) b.append(','); b.append(a[i]); }
        return b.toString();
    }

    @Override protected void onCreate(Bundle s) {
        super.onCreate(s);
        String t = probe();
        for (String line : t.split("\n")) Log.i(TAG, line);
        TextView v = new TextView(this);
        v.setTextSize(13);
        v.setPadding(24, 48, 24, 24);
        v.setTypeface(android.graphics.Typeface.MONOSPACE);
        v.setText("Tadpole ABI probe\n\n" + t);
        ScrollView sv = new ScrollView(this);
        sv.addView(v);
        setContentView(sv);
    }
}
