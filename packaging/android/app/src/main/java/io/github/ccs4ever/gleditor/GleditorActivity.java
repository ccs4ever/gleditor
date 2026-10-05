package io.github.ccs4ever.gleditor;

import android.content.Context;
import android.view.View;
import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/** The AccessKit provider describes the same surface that SDL renders into. */
public final class GleditorActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // System.loadLibrary also calls LibVLC's JNI_OnLoad with the VM.
        return new String[] {"SDL3", "vlc", "main"};
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        SDLSurface surface = new AccessibleSurface(context);
        surface.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
        return surface;
    }

    public View getAccessibilityHost() {
        return mSurface;
    }

    private static final class AccessibleSurface extends SDLSurface {
        private View.AccessibilityDelegate accessibilityDelegate;

        AccessibleSurface(Context context) {
            super(context);
        }

        @Override
        public void setAccessibilityDelegate(View.AccessibilityDelegate delegate) {
            accessibilityDelegate = delegate;
            super.setAccessibilityDelegate(delegate);
        }

        @Override
        public View.AccessibilityDelegate getAccessibilityDelegate() {
            // The framework getter starts at API29; AccessKit also works on
            // our API28 minimum when its JNI lookup finds this implementation.
            return accessibilityDelegate;
        }
    }
}
