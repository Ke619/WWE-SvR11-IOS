// WWE SmackDown vs. Raw 2011 - the game (SDL's activity running the port's
// native code; InstallActivity starts it once the game files are in place).

package io.github.kaikoclanworth1.svr2011;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLControllerManager;

public class GameActivity extends SDLActivity {
    // While the game runs (the launcher's Saves / Paint Tool tabs wait: it keeps its saves open).
    static volatile boolean running;

    // A controller's sticks, whichever view has the focus. SDL reads them only
    // in its game view's motion listener, and only for events whose source is
    // exactly SOURCE_JOYSTICK. When the phone keyboard's hidden text field has
    // the focus (the game's keyboard, the online overlay, a phone keyboard
    // tap), or a source carries more bits, the motion goes unhandled, and
    // Android turns an unhandled stick into D-pad key presses (ViewRootImpl's
    // synthetic joystick keys) that SDL takes as the controller's D-pad: the
    // left stick "becomes the D-pad" until the focus comes back
    // (bug 2.1.1: Galaxy S26 Ultra, Xbox Series X / PS5 controllers).
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0 &&
            event.getActionMasked() == MotionEvent.ACTION_MOVE) {
            try {
                SDLControllerManager.handleJoystickMotionEvent(event);
                return true;  // (handled: no synthetic D-pad keys from it)
            } catch (RuntimeException e) {  // (SDL not set up: its libraries didn't load)
                Log.w("SvR2011", "joystick motion: " + e);
            }
        }
        return super.dispatchGenericMotionEvent(event);
    }

    // Belt and braces: D-pad keys Android made up from a stick (FLAG_FALLBACK,
    // from a joystick-class device) never reach the game - neither as the
    // controller's D-pad nor, while the controller is between a removal and
    // its re-add, as the arrow keys of the keyboard binds.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if ((event.getFlags() & KeyEvent.FLAG_FALLBACK) != 0 && IsDpad(event.getKeyCode()) &&
            FromJoystick(event)) {
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private static boolean IsDpad(int key) {
        return key == KeyEvent.KEYCODE_DPAD_UP || key == KeyEvent.KEYCODE_DPAD_DOWN ||
               key == KeyEvent.KEYCODE_DPAD_LEFT || key == KeyEvent.KEYCODE_DPAD_RIGHT ||
               key == KeyEvent.KEYCODE_DPAD_UP_LEFT || key == KeyEvent.KEYCODE_DPAD_UP_RIGHT ||
               key == KeyEvent.KEYCODE_DPAD_DOWN_LEFT || key == KeyEvent.KEYCODE_DPAD_DOWN_RIGHT;
    }

    private static boolean FromJoystick(KeyEvent event) {
        if ((event.getSource() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0) return true;
        InputDevice device = InputDevice.getDevice(event.getDeviceId());
        return device != null && (device.getSources() & InputDevice.SOURCE_CLASS_JOYSTICK) != 0;
    }

    // The device's own vibrator for player 1's rumble when the controller can't rumble (pad_types.cpp:
    // a handheld's built-in pad - AYN Thor / Odin, Retroid ... - has no vibrator on its InputDevice; its
    // motor is the device's). low / high: the game's two motor speeds, 0-65535; 0 and 0 stop it. An Xbox
    // 360 rumble holds until the game changes it: a long one-shot, cancelled or replaced on the next call.
    private int rumbleAmplitude;

    public synchronized void deviceRumble(int low, int high) {
        int amplitude = Math.round((low * 0.6f + high * 0.4f) / 65535f * 255f);
        amplitude = Math.max(0, Math.min(255, amplitude));
        if (amplitude == rumbleAmplitude) return;
        rumbleAmplitude = amplitude;
        try {
            android.os.Vibrator vibrator = DeviceVibrator();
            if (vibrator == null || !vibrator.hasVibrator()) return;
            if (amplitude == 0) {
                vibrator.cancel();
            } else if (android.os.Build.VERSION.SDK_INT >= 26) {
                vibrator.vibrate(android.os.VibrationEffect.createOneShot(60000,
                    vibrator.hasAmplitudeControl() ? amplitude : android.os.VibrationEffect.DEFAULT_AMPLITUDE));
            } else {
                vibrator.vibrate(60000);
            }
        } catch (Exception e) {
            Log.w("SvR2011", "rumble: device vibrator failed", e);
        }
    }

    private android.os.Vibrator DeviceVibrator() {
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            android.os.VibratorManager manager =
                (android.os.VibratorManager) getSystemService(android.content.Context.VIBRATOR_MANAGER_SERVICE);
            if (manager != null) return manager.getDefaultVibrator();
        }
        return (android.os.Vibrator) getSystemService(android.content.Context.VIBRATOR_SERVICE);
    }

    @Override
    protected void onPause() {
        deviceRumble(0, 0);  // (no rumble left running behind another app)
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        deviceRumble(0, 0);
        running = false;
        super.onDestroy();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        running = true;
        InstallActivity.init(this);
        // The native side's "program folder" (settings, saves, logs, disc
        // files): rex::filesystem::GetExecutableFolder() reads it. Set before
        // SDLActivity loads the libraries and starts the game's main.
        try {
            Os.setenv("REX_APP_FOLDER", InstallActivity.gameFolder().getAbsolutePath(), true);
            // Custom GPU drivers (src/gpu_driver.cpp): libadrenotools' hooks are with the app's libraries.
            Os.setenv("SVR2011_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            Os.setenv("SVR2011_CACHE_DIR", getCacheDir().getAbsolutePath(), true);
            // Automated tests over USB debugging (tools/phone_session.ps1):
            // am start ... --es SVR2011_INPUT_FILE <file> --es SVR2011_USER_DATA <folder>
            // become the environment the game reads, as on the PC.
            Bundle extras = getIntent().getExtras();
            if (extras != null) {
                for (String key : extras.keySet()) {
                    Object value = extras.get(key);
                    if (key.startsWith("SVR2011_") && value != null) Os.setenv(key, value.toString(), true);
                }
            }
        } catch (ErrnoException e) {
            Log.e("SvR2011", "setenv failed", e);
        }
        super.onCreate(savedInstanceState);
        ProblemReport.ctx = getApplicationContext();  // (report.txt's "last exit" lines)
        PreferSixtyHz();
        GameModeSetup();
    }

    // Android 13+ Game Mode: the phone's game dashboard mode (logged: a measurement aid) and the game's state
    // (playing - the phone's game services may then favour it). Setting android_sustained_performance: the
    // clocks the phone can hold for long (lower peaks, no throttling dips) - for phones that support it.
    private void GameModeSetup() {
        try {
            if (android.os.Build.VERSION.SDK_INT >= 33) {
                android.app.GameManager gm = getSystemService(android.app.GameManager.class);
                if (gm != null) {
                    gm.setGameState(new android.app.GameState(false, android.app.GameState.MODE_GAMEPLAY_UNINTERRUPTIBLE));
                    Log.i("SvR2011", "game mode: " + gm.getGameMode() + " (1 standard, 2 performance, 3 battery, 4 custom)");
                }
            }
            // (tests: --es SVR2011_TEST_SUSTAINED 1)
            if (new GameSettings(InstallActivity.gameFolder()).getBool("android_sustained_performance", false)
                || "1".equals(Os.getenv("SVR2011_TEST_SUSTAINED"))) {
                android.os.PowerManager pm = getSystemService(android.os.PowerManager.class);
                boolean ok = pm != null && pm.isSustainedPerformanceModeSupported();
                if (ok) getWindow().setSustainedPerformanceMode(true);
                Log.i("SvR2011", "game mode: sustained performance " + (ok ? "on" : "not supported"));
            }
        } catch (Exception e) {
            Log.w("SvR2011", "game mode: setup failed", e);
        }
    }

    // The game runs at 60 fps; on a 120 Hz screen its frames land on 2 or 3
    // of the screen's refreshes in turn (8 / 25 ms: a steady judder). Asks for
    // the screen's 60 Hz mode (same size) while the game is in front.
    private void PreferSixtyHz() {
        try {
            android.view.Display display = getWindowManager().getDefaultDisplay();
            android.view.Display.Mode current = display.getMode();
            android.view.Display.Mode best = null;
            for (android.view.Display.Mode m : display.getSupportedModes()) {
                if (m.getPhysicalWidth() != current.getPhysicalWidth() ||
                    m.getPhysicalHeight() != current.getPhysicalHeight()) continue;
                if (Math.abs(m.getRefreshRate() - 60f) < 1f) best = m;
            }
            android.view.WindowManager.LayoutParams lp = getWindow().getAttributes();
            if (best != null) lp.preferredDisplayModeId = best.getModeId();
            lp.preferredRefreshRate = 60f;
            getWindow().setAttributes(lp);
            Log.i("SvR2011", "display: asked for 60 Hz (mode " + (best != null ? best.getModeId() : -1) + ")");
        } catch (Exception e) {
            Log.w("SvR2011", "display: 60 Hz request failed", e);
        }
    }

    // The game's surface votes for 60 fps itself (a fixed-rate source): the
    // compositor then runs the screen at 60 Hz (the window's preference
    // alone left the Fold's cover screen at 120 Hz).
    @Override
    protected org.libsdl.app.SDLSurface createSDLSurface(android.content.Context context) {
        org.libsdl.app.SDLSurface surface = super.createSDLSurface(context);
        // (test aid: SVR2011_TEST_SURFACE=480x216 - a surface smaller than the screen, as a phone's game downscaling
        // makes it: report e69c2e)
        String fixed = Os.getenv("SVR2011_TEST_SURFACE");
        if (fixed != null && fixed.matches("\\d+x\\d+")) {
            String[] wh = fixed.split("x");
            surface.getHolder().setFixedSize(Integer.parseInt(wh[0]), Integer.parseInt(wh[1]));
            Log.i("SvR2011", "test: surface fixed at " + fixed);
        }
        surface.getHolder().addCallback(new android.view.SurfaceHolder.Callback() {
            @Override public void surfaceCreated(android.view.SurfaceHolder holder) { Sixty(holder); }
            @Override public void surfaceChanged(android.view.SurfaceHolder holder, int f, int w, int h) { Sixty(holder); }
            @Override public void surfaceDestroyed(android.view.SurfaceHolder holder) {}
        });
        return surface;
    }

    private static void Sixty(android.view.SurfaceHolder holder) {
        if (android.os.Build.VERSION.SDK_INT < 31) return;  // (Android 10 / 11: the display mode above only)
        try {
            holder.getSurface().setFrameRate(60f, android.view.Surface.FRAME_RATE_COMPATIBILITY_FIXED_SOURCE,
                                             android.view.Surface.CHANGE_FRAME_RATE_ALWAYS);
        } catch (Exception e) {
            Log.w("SvR2011", "display: surface frame rate failed", e);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) PreferSixtyHz();  // (the Fold: the other screen has its own modes)
    }

    // Settings on the command line (tests: --es args "--audio_mute=true ..."); a value with spaces in double
    // quotes stays one (--dbg_match="people=EDGE,KANE cpu=all"), the quotes left out.
    @Override
    protected String[] getArguments() {
        Bundle extras = getIntent().getExtras();
        String args = extras != null ? extras.getString("args") : null;
        java.util.List<String> out = new java.util.ArrayList<>();
        if (args == null) return new String[0];
        StringBuilder word = new StringBuilder();
        boolean quoted = false, any = false;
        for (char ch : args.toCharArray()) {
            if (ch == '"') {
                quoted = !quoted;
                any = true;
            } else if (Character.isWhitespace(ch) && !quoted) {
                if (any) out.add(word.toString());
                word.setLength(0);
                any = false;
            } else {
                word.append(ch);
                any = true;
            }
        }
        if (any) out.add(word.toString());
        return out.toArray(new String[0]);
    }

    @Override
    protected String[] getLibraries() {
        // Dependencies first: libmain.so (the game, with SDL_main) needs the
        // runtime (with SDL); the GPU plugin is loaded by name at startup.
        return new String[] { "c++_shared", "rexruntime", "rexgpu-xenos", "main" };
    }
}
