package fathom.mc;

import java.io.IOException;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.nio.file.StandardOpenOption;
import java.nio.file.attribute.FileTime;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Collection;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.TreeMap;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;

final class Link implements Runnable {
    private static final String NIGHT = "net.nightclient.NightClient";
    private static final String HACK_LIST = "net.nightclient.hack.HackList";
    private static final String HACK = "net.nightclient.hack.Hack";
    private static final String API = "baritone.api.BaritoneAPI";
    private static final String BARITONE = "baritone.api.IBaritone";

    private final Path dir;
    private final Path control;
    private final Path state;
    private final Path log;
    private final ClassLoader loader = Link.class.getClassLoader();
    private final Map<String, Method> methods = new ConcurrentHashMap<>();
    private final Map<String, Object> hacks = new TreeMap<>();
    private final Set<String> logged = new HashSet<>();

    private Object mc;
    private Executor client;
    private Object night;
    private Field playerField;
    private boolean playerChecked;
    private Object mouseHandler;
    private Method mouseGrabbed;
    private Method mouseRelease;
    private Method mouseGrab;
    private String game = "";
    private int hackCount = -1;
    private FileTime controlTime;
    private boolean primed;
    private volatile String session = "";
    private long seq;
    private volatile long ack;
    private volatile String snapshot;
    private long lastState;

    static void start() {
        Thread t = new Thread(new Link(), "Fathom link");
        t.setDaemon(true);
        t.start();
    }

    private Link() {
        String appdata = System.getenv("APPDATA");
        Path base = appdata != null && !appdata.isEmpty() ? Paths.get(appdata, ".minecraft")
                : Paths.get(System.getProperty("user.home"), ".minecraft");
        dir = base.resolve("fathom");
        control = dir.resolve("control.json");
        state = dir.resolve("state.json");
        log = dir.resolve("mod-log.txt");
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            try {
                Files.deleteIfExists(state);
            } catch (IOException ignored) {
            }
        }));
    }

    @Override
    public void run() {
        log("Fathom mod started");
        while (true) {
            try {
                Thread.sleep(100);
                step();
            } catch (InterruptedException e) {
                return;
            } catch (Throwable t) {
                problem(t);
                try {
                    Thread.sleep(1000);
                } catch (InterruptedException e) {
                    return;
                }
            }
        }
    }

    private void step() throws Exception {
        if (client == null && !connect()) {
            Thread.sleep(900);
            return;
        }
        readControl();
        long now = System.currentTimeMillis();
        if (now - lastState < 500) return;
        lastState = now;
        client.execute(this::capture);
        String snap = snapshot;
        if (snap != null) write(state, snap);
    }

    private boolean connect() {
        Class<?> nc = load(NIGHT);
        if (nc != null) {
            night = staticField(nc, "INSTANCE");
            mc = staticField(nc, "MC");
        }
        if (mc == null) mc = minecraft();
        if (!(mc instanceof Executor)) {
            mc = null;
            return false;
        }
        client = (Executor) mc;
        game = gameVersion();
        log("Connected to Minecraft " + game
                + (night != null ? ", cheats " + staticField(nc, "VERSION") : ", cheats not found")
                + (load(API) != null ? ", Baritone found" : ", Baritone not found"));
        log(Overlay.load(this, dir) ? "Fathom menu loaded" : "Fathom menu could not be loaded");
        client.execute(this::hideNightLogo);
        return true;
    }

    private void hideNightLogo() {
        try {
            if (night == null) return;
            Object otfs = method(NIGHT, "getOtfs").invoke(night);
            Object logo = otfs.getClass().getField("wurstLogoOtf").get(otfs);
            Object vis = ((Map<?, ?>) logo.getClass().getMethod("getSettings").invoke(logo)).get("Visibility");
            if (vis == null) return;
            Object selected = vis.getClass().getMethod("getSelected").invoke(vis);
            if (selected != null && selected.toString().equals("Always"))
                vis.getClass().getMethod("setSelected", String.class).invoke(vis, "Only when outdated");
        } catch (Throwable t) {
            problem(t);
        }
    }

    String snapshotNow() {
        String s = build();
        if (s == null) return "";
        snapshot = s;
        return s;
    }

    void menuCommand(String json) {
        try {
            Object o = Json.parse(json);
            if (o instanceof Map && client != null) {
                Map<?, ?> c = (Map<?, ?>) o;
                client.execute(() -> run(c));
            }
        } catch (RuntimeException e) {
            problem(e);
        }
    }

    boolean releaseMouse() {
        try {
            Object h = mouse();
            if (h == null) return false;
            boolean grabbed = Boolean.TRUE.equals(mouseGrabbed.invoke(h));
            if (grabbed) mouseRelease.invoke(h);
            return grabbed;
        } catch (Throwable t) {
            problem(t);
            return false;
        }
    }

    boolean isGrabbed() {
        try {
            Object h = mouse();
            return h != null && Boolean.TRUE.equals(mouseGrabbed.invoke(h));
        } catch (Throwable t) {
            problem(t);
            return false;
        }
    }

    void grabMouse() {
        try {
            Object h = mouse();
            if (h != null && !Boolean.TRUE.equals(mouseGrabbed.invoke(h))) mouseGrab.invoke(h);
        } catch (Throwable t) {
            problem(t);
        }
    }

    private Object mouse() throws Exception {
        if (mouseHandler != null || mc == null) return mouseHandler;
        Field f = null;
        for (String n : new String[] {"mouseHandler", "field_1729"}) {
            try {
                f = mc.getClass().getField(n);
                break;
            } catch (NoSuchFieldException ignored) {
            }
        }
        if (f == null) return null;
        Object h = f.get(mc);
        if (h == null) return null;
        mouseGrabbed = find(h.getClass(), "isMouseGrabbed", "method_1613");
        mouseRelease = find(h.getClass(), "releaseMouse", "method_1610");
        mouseGrab = find(h.getClass(), "grabMouse", "method_1612");
        if (mouseGrabbed != null && mouseRelease != null && mouseGrab != null) mouseHandler = h;
        return mouseHandler;
    }

    private static Method find(Class<?> c, String... names) {
        for (String n : names) {
            try {
                return c.getMethod(n);
            } catch (NoSuchMethodException ignored) {
            }
        }
        return null;
    }

    private void readControl() {
        FileTime t;
        try {
            t = Files.getLastModifiedTime(control);
        } catch (IOException e) {
            primed = true;
            return;
        }
        if (t.equals(controlTime)) return;
        Object root;
        try {
            root = Json.parse(new String(Files.readAllBytes(control), StandardCharsets.UTF_8));
        } catch (IOException | RuntimeException e) {
            return;
        }
        controlTime = t;
        if (!(root instanceof Map)) return;
        Map<?, ?> m = (Map<?, ?>) root;
        String s = String.valueOf(m.get("session"));
        List<Map<?, ?>> cmds = new ArrayList<>();
        Object list = m.get("commands");
        if (list instanceof List)
            for (Object o : (List<?>) list)
                if (o instanceof Map) cmds.add((Map<?, ?>) o);
        cmds.sort(Comparator.comparingLong(c -> number(c.get("seq"))));
        long last = cmds.isEmpty() ? 0 : number(cmds.get(cmds.size() - 1).get("seq"));
        if (!primed) {
            primed = true;
            session = s;
            seq = last;
            ack = last;
            return;
        }
        if (!s.equals(session)) {
            ack = 0;
            session = s;
            seq = 0;
        }
        List<Map<?, ?>> todo = new ArrayList<>();
        for (Map<?, ?> c : cmds)
            if (number(c.get("seq")) > seq) todo.add(c);
        if (todo.isEmpty()) return;
        seq = last;
        long done = last;
        client.execute(() -> {
            for (Map<?, ?> c : todo) run(c);
            ack = done;
        });
    }

    private void run(Map<?, ?> c) {
        String op = String.valueOf(c.get("op"));
        try {
            if (op.equals("hack")) setHack(String.valueOf(c.get("name")), Boolean.TRUE.equals(c.get("on")));
            else if (op.equals("alloff")) allOff();
            else if (op.equals("baritone")) baritoneCommand(String.valueOf(c.get("text")));
            else if (op.equals("bset")) baritoneSet(String.valueOf(c.get("name")), String.valueOf(c.get("value")));
        } catch (Throwable t) {
            problem(t);
        }
    }

    private void capture() {
        String s = build();
        if (s != null) snapshot = s;
    }

    private String build() {
        try {
            StringBuilder b = new StringBuilder(8192);
            b.append("{\"version\":1,\"session\":").append(Json.quote(session)).append(",\"ack\":").append(ack);
            b.append(",\"mc\":").append(Json.quote(game));
            b.append(",\"night\":").append(night != null);
            if (night != null) {
                b.append(",\"client\":").append(Json.quote(String.valueOf(staticField(load(NIGHT), "VERSION"))));
                b.append(",\"nightOn\":").append(Boolean.TRUE.equals(method(NIGHT, "isEnabled").invoke(night)));
            }
            b.append(",\"world\":").append(player() != null);
            b.append(",\"menu\":").append(Overlay.isRendering());
            b.append(",\"hacks\":{");
            Method enabled = night != null ? method(HACK, "isEnabled") : null;
            boolean first = true;
            for (Map.Entry<String, Object> e : hackMap().entrySet()) {
                if (!first) b.append(',');
                first = false;
                b.append(Json.quote(e.getKey())).append(':').append(Boolean.TRUE.equals(enabled.invoke(e.getValue())));
            }
            b.append('}');
            Object bar = baritone();
            b.append(",\"baritone\":").append(bar != null);
            if (bar != null) baritoneState(b, bar);
            b.append('}');
            return b.toString();
        } catch (Throwable t) {
            problem(t);
            return null;
        }
    }

    private Map<String, Object> hackMap() throws Exception {
        if (night == null) return Collections.emptyMap();
        Object list = method(NIGHT, "getHax").invoke(night);
        if (list == null) return Collections.emptyMap();
        Collection<?> all = (Collection<?>) method(HACK_LIST, "getAllHax").invoke(list);
        if (all.size() != hackCount) {
            hacks.clear();
            Method name = method(HACK, "getName");
            for (Object h : all) hacks.put(key(String.valueOf(name.invoke(h))), h);
            hackCount = all.size();
        }
        return hacks;
    }

    private void setHack(String name, boolean on) throws Exception {
        Object h = hackMap().get(key(name));
        if (h == null) return;
        if (Boolean.TRUE.equals(method(HACK, "isEnabled").invoke(h)) == on) return;
        method(HACK, "setEnabled", boolean.class).invoke(h, on);
    }

    private void allOff() throws Exception {
        Method is = method(HACK, "isEnabled");
        Method set = method(HACK, "setEnabled", boolean.class);
        for (Object h : new ArrayList<>(hackMap().values()))
            if (Boolean.TRUE.equals(is.invoke(h))) set.invoke(h, false);
    }

    private Object baritone() throws Exception {
        if (load(API) == null) return null;
        Object provider = method(API, "getProvider").invoke(null);
        return provider == null ? null : method("baritone.api.IBaritoneProvider", "getPrimaryBaritone").invoke(provider);
    }

    private void baritoneCommand(String text) throws Exception {
        Object b = baritone();
        if (b == null) return;
        Object manager = method(BARITONE, "getCommandManager").invoke(b);
        method("baritone.api.command.manager.ICommandManager", "execute", String.class).invoke(manager, text);
    }

    private void baritoneSet(String name, String value) throws Exception {
        if (load(API) == null) return;
        Object settings = method(API, "getSettings").invoke(null);
        Class<?> type = load("baritone.api.Settings");
        method("baritone.api.utils.SettingsUtil", "parseAndApply", type, String.class, String.class)
                .invoke(null, settings, name.toLowerCase(Locale.ROOT), value);
        method("baritone.api.utils.SettingsUtil", "save", type).invoke(null, settings);
    }

    private void baritoneState(StringBuilder b, Object bar) throws Exception {
        Object control = method(BARITONE, "getPathingControlManager").invoke(bar);
        Optional<?> process = (Optional<?>) method("baritone.api.pathing.calc.IPathingControlManager", "mostRecentInControl").invoke(control);
        String task = process.isPresent()
                ? String.valueOf(method("baritone.api.process.IBaritoneProcess", "displayName").invoke(process.get()))
                : "";
        Object pathing = method(BARITONE, "getPathingBehavior").invoke(bar);
        String behavior = "baritone.api.behavior.IPathingBehavior";
        Object goal = method(behavior, "getGoal").invoke(pathing);
        Optional<?> eta = (Optional<?>) method(behavior, "estimatedTicksToGoal").invoke(pathing);
        b.append(",\"task\":").append(Json.quote(task));
        b.append(",\"pathing\":").append(Boolean.TRUE.equals(method(behavior, "isPathing").invoke(pathing)));
        b.append(",\"goal\":").append(Json.quote(goal == null ? "" : String.valueOf(goal)));
        if (eta.isPresent() && eta.get() instanceof Number) b.append(",\"eta\":").append(((Number) eta.get()).longValue());
        Object context = method(BARITONE, "getPlayerContext").invoke(bar);
        if (method("baritone.api.utils.IPlayerContext", "player").invoke(context) != null) {
            Object feet = method("baritone.api.utils.IPlayerContext", "playerFeet").invoke(context);
            Class<?> pos = feet.getClass();
            b.append(",\"pos\":[").append(pos.getField("x").getInt(feet)).append(',').append(pos.getField("y").getInt(feet))
                    .append(',').append(pos.getField("z").getInt(feet)).append(']');
        }
        Object settings = method(API, "getSettings").invoke(null);
        List<?> all = (List<?>) load("baritone.api.Settings").getField("allSettings").get(settings);
        Field value = load("baritone.api.Settings$Setting").getField("value");
        Method name = method("baritone.api.Settings$Setting", "getName");
        b.append(",\"settings\":{");
        boolean first = true;
        for (Object setting : all) {
            Object v = value.get(setting);
            if (!(v instanceof Boolean) && !(v instanceof Number)) continue;
            if (!first) b.append(',');
            first = false;
            b.append(Json.quote(String.valueOf(name.invoke(setting)))).append(':');
            if (v instanceof Boolean) b.append(v);
            else if (v instanceof Float || v instanceof Double) {
                double d = ((Number) v).doubleValue();
                b.append(Double.isFinite(d) ? Double.toString(d) : "0");
            } else b.append(((Number) v).longValue());
        }
        b.append('}');
    }

    private Object player() {
        if (!playerChecked) {
            playerChecked = true;
            for (String n : new String[] {"player", "field_1724"}) {
                try {
                    playerField = mc.getClass().getField(n);
                    break;
                } catch (Throwable ignored) {
                }
            }
        }
        try {
            return playerField == null ? null : playerField.get(mc);
        } catch (Throwable t) {
            return null;
        }
    }

    private Object minecraft() {
        for (String name : new String[] {"net.minecraft.client.Minecraft", "net.minecraft.class_310"}) {
            Class<?> c = load(name);
            if (c == null) continue;
            for (Method m : c.getDeclaredMethods()) {
                if (!Modifier.isStatic(m.getModifiers()) || m.getParameterCount() != 0 || m.getReturnType() != c) continue;
                try {
                    m.setAccessible(true);
                    Object o = m.invoke(null);
                    if (o != null) return o;
                } catch (Throwable ignored) {
                }
            }
        }
        return null;
    }

    private String gameVersion() {
        try {
            Class<?> fl = load("net.fabricmc.loader.api.FabricLoader");
            Object instance = fl.getMethod("getInstance").invoke(null);
            Optional<?> mod = (Optional<?>) fl.getMethod("getModContainer", String.class).invoke(instance, "minecraft");
            if (!mod.isPresent()) return "";
            Object meta = load("net.fabricmc.loader.api.ModContainer").getMethod("getMetadata").invoke(mod.get());
            Object version = load("net.fabricmc.loader.api.metadata.ModMetadata").getMethod("getVersion").invoke(meta);
            return String.valueOf(load("net.fabricmc.loader.api.Version").getMethod("getFriendlyString").invoke(version));
        } catch (Throwable t) {
            return "";
        }
    }

    private Method method(String cls, String name, Class<?>... params) throws Exception {
        StringBuilder k = new StringBuilder(cls).append('#').append(name);
        for (Class<?> p : params) k.append(',').append(p.getName());
        String key = k.toString();
        Method m = methods.get(key);
        if (m != null) return m;
        Class<?> c = load(cls);
        if (c == null) throw new ClassNotFoundException(cls);
        m = c.getMethod(name, params);
        methods.put(key, m);
        return m;
    }

    private Class<?> load(String name) {
        try {
            return Class.forName(name, false, loader);
        } catch (Throwable t) {
            return null;
        }
    }

    private static Object staticField(Class<?> c, String name) {
        try {
            return c == null ? null : c.getField(name).get(null);
        } catch (Throwable t) {
            return null;
        }
    }

    private static long number(Object o) {
        return o instanceof Number ? ((Number) o).longValue() : 0;
    }

    private static String key(String s) {
        StringBuilder b = new StringBuilder(s.length());
        for (char c : s.toLowerCase(Locale.ROOT).toCharArray())
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) b.append(c);
        return b.toString();
    }

    private void write(Path file, String text) {
        try {
            Files.createDirectories(dir);
            Path tmp = dir.resolve(file.getFileName().toString() + ".tmp");
            Files.write(tmp, text.getBytes(StandardCharsets.UTF_8));
            try {
                Files.move(tmp, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException e) {
                Files.move(tmp, file, StandardCopyOption.REPLACE_EXISTING);
            }
        } catch (IOException ignored) {
        }
    }

    void problem(Throwable t) {
        while (t instanceof InvocationTargetException && t.getCause() != null) t = t.getCause();
        StackTraceElement[] st = t.getStackTrace();
        String line = t + (st.length > 0 ? " at " + st[0] : "");
        synchronized (logged) {
            if (logged.size() > 100 || !logged.add(line)) return;
        }
        log("Problem: " + line);
    }

    private synchronized void log(String line) {
        try {
            Files.createDirectories(dir);
            if (Files.exists(log) && Files.size(log) > 256 * 1024) Files.delete(log);
            String stamp = LocalDateTime.now().format(DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm:ss"));
            Files.write(log, (stamp + "  " + line + System.lineSeparator()).getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (IOException ignored) {
        }
    }
}
