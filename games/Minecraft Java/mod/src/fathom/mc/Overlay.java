package fathom.mc;

import java.io.InputStream;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.Locale;
import java.util.zip.CRC32;

final class Overlay {
    private static volatile Link link;
    private static volatile boolean loaded;

    private Overlay() {
    }

    static native boolean start(String dir);

    static native boolean rendering();

    static boolean load(Link owner, Path dir) {
        link = owner;
        String os = System.getProperty("os.name", "").toLowerCase(Locale.ROOT);
        String arch = System.getProperty("os.arch", "").toLowerCase(Locale.ROOT);
        if (!os.startsWith("windows") || !(arch.equals("amd64") || arch.equals("x86_64"))) return false;
        try (InputStream in = Overlay.class.getResourceAsStream("/fathom/native/fathom_mc.dll")) {
            if (in == null) return false;
            byte[] data = in.readAllBytes();
            CRC32 crc = new CRC32();
            crc.update(data);
            Path folder = dir.resolve("native");
            String name = "fathom_mc-" + Long.toHexString(crc.getValue()) + ".dll";
            Path lib = folder.resolve(name);
            if (!Files.exists(lib) || Files.size(lib) != data.length) {
                Files.createDirectories(folder);
                Path tmp = folder.resolve(name + ".tmp");
                Files.write(tmp, data);
                Files.move(tmp, lib, StandardCopyOption.REPLACE_EXISTING);
            }
            try (DirectoryStream<Path> old = Files.newDirectoryStream(folder, "fathom_mc-*.dll")) {
                for (Path p : old)
                    if (!p.getFileName().toString().equals(name)) {
                        try {
                            Files.deleteIfExists(p);
                        } catch (Exception ignored) {
                        }
                    }
            }
            System.load(lib.toAbsolutePath().toString());
            loaded = start(dir.toAbsolutePath().toString());
            return loaded;
        } catch (Throwable t) {
            owner.problem(t);
            return false;
        }
    }

    static boolean isRendering() {
        if (!loaded) return false;
        try {
            return rendering();
        } catch (Throwable t) {
            return false;
        }
    }

    static String state() {
        Link l = link;
        return l == null ? "" : l.snapshotNow();
    }

    static void command(String json) {
        Link l = link;
        if (l != null) l.menuCommand(json);
    }

    static boolean releaseMouse() {
        Link l = link;
        return l != null && l.releaseMouse();
    }

    static void grabMouse() {
        Link l = link;
        if (l != null) l.grabMouse();
    }

    static boolean grabbed() {
        Link l = link;
        return l != null && l.isGrabbed();
    }
}
