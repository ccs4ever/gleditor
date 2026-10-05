package io.github.ccs4ever.gleditor;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.StrictMode;
import android.os.SystemClock;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.View;
import org.libsdl.app.SDLActivity;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;

/** Exercises the platform client, including actions returning to native state. */
public final class AccessibilitySmoke extends Instrumentation {
    private void checkpoint(String stage) {
        Bundle progress = new Bundle();
        progress.putString("accessibility_stage", stage);
        sendStatus(0, progress);
    }

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        start();
    }

    private AccessibilityNodeInfo findEditor(AccessibilityNodeInfo node) {
        if (node == null) {
            return null;
        }
        if ("android.widget.EditText".contentEquals(node.getClassName())) {
            return node;
        }
        for (int child = 0; child < node.getChildCount(); ++child) {
            AccessibilityNodeInfo found = findEditor(node.getChild(child));
            if (found != null) {
                return found;
            }
        }
        return null;
    }

    @Override
    public void onStart() {
        Bundle results = new Bundle();
        try {
            File document = new File(getTargetContext().getCacheDir(), "accessibility-smoke.txt");
            try (FileOutputStream stream = new FileOutputStream(document)) {
                stream.write("Android accessibility fixture\n".getBytes(StandardCharsets.UTF_8));
            }
            Intent launch = new Intent(Intent.ACTION_VIEW);
            launch.setClassName(getTargetContext(), GleditorActivity.class.getName());
            launch.setDataAndType(Uri.fromFile(document), "text/plain");
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            // The fixture stays in this app's disposable cache. Android's
            // file-URI exposure check is meant for sending it to another app.
            StrictMode.VmPolicy policy = StrictMode.getVmPolicy();
            try {
                StrictMode.setVmPolicy(new StrictMode.VmPolicy.Builder().build());
                checkpoint("starting Android document-open Intent");
                getTargetContext().startActivity(launch);
            } finally {
                StrictMode.setVmPolicy(policy);
            }
            checkpoint("Android document-open Intent dispatched");
            boolean[] nativeReady = {false};
            String[] readiness = {"SDL Activity absent"};
            long deadline = SystemClock.uptimeMillis() + 120000;
            while (!nativeReady[0] && SystemClock.uptimeMillis() < deadline) {
                runOnMainSync(() -> {
                    Context context = SDLActivity.getContext();
                    if (context instanceof GleditorActivity) {
                        View host = ((GleditorActivity) context).getAccessibilityHost();
                        nativeReady[0] = host != null && host.getAccessibilityNodeProvider() != null;
                        readiness[0] = host == null ? "SDL surface absent"
                                : nativeReady[0] ? "provider attached" : "native provider absent";
                    }
                });
                if (!nativeReady[0]) {
                    SystemClock.sleep(100);
                }
            }
            if (!nativeReady[0]) {
                throw new AssertionError("Native Android accessibility provider did not attach: "
                        + readiness[0]);
            }
            checkpoint("native provider attached to SDL surface");
            AccessibilityNodeInfo editor = null;
            deadline = SystemClock.uptimeMillis() + 90000;
            while (editor == null && SystemClock.uptimeMillis() < deadline) {
                editor = findEditor(getUiAutomation().getRootInActiveWindow());
                if (editor == null) {
                    SystemClock.sleep(100);
                }
            }
            if (editor == null || !editor.isEditable()) {
                throw new AssertionError("Native editable document is absent from Android accessibility");
            }
            checkpoint("editable document read through platform client");
            if (!editor.performAction(AccessibilityNodeInfo.ACTION_FOCUS)
                    || !editor.performAction(AccessibilityNodeInfo.ACTION_CLICK)) {
                throw new AssertionError("Android provider refused document focus/click actions");
            }
            boolean focused = false;
            deadline = SystemClock.uptimeMillis() + 20000;
            while (!focused && SystemClock.uptimeMillis() < deadline) {
                editor = findEditor(getUiAutomation().getRootInActiveWindow());
                focused = editor != null && editor.isFocused();
                if (!focused) {
                    SystemClock.sleep(100);
                }
            }
            if (!focused) {
                throw new AssertionError("Document focus action did not return to native state");
            }
            checkpoint("focus and click returned to native document state");
            sendStringSync("android-a11y-probe");
            boolean textVisible = false;
            deadline = SystemClock.uptimeMillis() + 45000;
            while (!textVisible && SystemClock.uptimeMillis() < deadline) {
                editor = findEditor(getUiAutomation().getRootInActiveWindow());
                textVisible = editor != null && editor.getText() != null
                        && editor.getText().toString().contains("android-a11y-probe");
                if (!textVisible) {
                    SystemClock.sleep(100);
                }
            }
            if (!textVisible) {
                throw new AssertionError("Edited text is absent from the platform accessibility tree");
            }
            results.putString("accessibility",
                    "PASS: editable text, focus and click via platform client");
            finish(Activity.RESULT_OK, results);
        } catch (Throwable error) {
            results.putString("accessibility", "FAIL: " + error);
            finish(Activity.RESULT_CANCELED, results);
        }
    }
}
