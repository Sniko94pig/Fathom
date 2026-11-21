import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.HashSet;
import java.util.Locale;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import java.util.zip.ZipOutputStream;

public final class Rebrand {
    private static final String KEEP = "Night Client encryption";
    private static final String HOLD = "\u0001keep\u0001";
    private static final String[][] RULES = {
        {"https://linkforgearthur.com", "https://github.com/Sniko94pig/Fathom"},
        {"http://linkforgearthur.com", "https://github.com/Sniko94pig/Fathom"},
        {"LinkForgeArthur.com", "Fathom"},
        {"linkforgearthur.com", "github.com/Sniko94pig/Fathom"},
        {"Night Client", "Fathom"},
    };
    private static final long TIME = 1704067200000L;
    private static int changed;

    public static void main(String[] args) throws IOException {
        byte[] out = jar(Files.readAllBytes(Paths.get(args[0])));
        Files.write(Paths.get(args[1]), out);
        System.out.println("rebranded " + changed + " entries");
    }

    private static String text(String s) {
        if (s.indexOf("inkForge") < 0 && s.indexOf("inkforge") < 0 && s.indexOf("Night Client") < 0) return s;
        String r = s.replace(KEEP, HOLD);
        for (String[] rule : RULES) r = r.replace(rule[0], rule[1]);
        return r.replace(HOLD, KEEP);
    }

    private static byte[] jar(byte[] data) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream(data.length);
        try (ZipInputStream in = new ZipInputStream(new ByteArrayInputStream(data)); ZipOutputStream out = new ZipOutputStream(bo)) {
            Set<String> seen = new HashSet<>();
            ZipEntry e;
            while ((e = in.getNextEntry()) != null) {
                String name = e.getName();
                if (!seen.add(name)) continue;
                byte[] b = in.readAllBytes();
                if (!e.isDirectory()) b = file(name, b);
                ZipEntry o = new ZipEntry(name);
                o.setTime(TIME);
                out.putNextEntry(o);
                out.write(b);
                out.closeEntry();
            }
        }
        return bo.toByteArray();
    }

    private static byte[] file(String name, byte[] b) throws IOException {
        String lower = name.toLowerCase(Locale.ROOT);
        if (lower.endsWith(".jar")) return jar(b);
        if (lower.endsWith(".class")) return classFile(b);
        if (lower.endsWith(".json") || lower.endsWith(".txt") || lower.endsWith(".properties") || lower.endsWith(".mcmeta")
                || lower.endsWith(".lang") || lower.endsWith(".md")) {
            String s = new String(b, StandardCharsets.UTF_8);
            String r = text(s);
            if (r.equals(s)) return b;
            changed++;
            return r.getBytes(StandardCharsets.UTF_8);
        }
        return b;
    }

    private static byte[] classFile(byte[] b) throws IOException {
        DataInputStream in = new DataInputStream(new ByteArrayInputStream(b));
        ByteArrayOutputStream bo = new ByteArrayOutputStream(b.length + 64);
        DataOutputStream out = new DataOutputStream(bo);
        int magic = in.readInt();
        if (magic != 0xCAFEBABE) return b;
        out.writeInt(magic);
        out.writeShort(in.readUnsignedShort());
        out.writeShort(in.readUnsignedShort());
        int count = in.readUnsignedShort();
        out.writeShort(count);
        boolean touched = false;
        for (int i = 1; i < count; i++) {
            int tag = in.readUnsignedByte();
            out.writeByte(tag);
            switch (tag) {
                case 1: {
                    String s = in.readUTF();
                    String r = text(s);
                    if (!r.equals(s)) touched = true;
                    out.writeUTF(r);
                    break;
                }
                case 3:
                case 4:
                case 9:
                case 10:
                case 11:
                case 12:
                case 17:
                case 18:
                    out.writeInt(in.readInt());
                    break;
                case 5:
                case 6:
                    out.writeLong(in.readLong());
                    i++;
                    break;
                case 7:
                case 8:
                case 16:
                case 19:
                case 20:
                    out.writeShort(in.readUnsignedShort());
                    break;
                case 15:
                    out.writeByte(in.readUnsignedByte());
                    out.writeShort(in.readUnsignedShort());
                    break;
                default:
                    return b;
            }
        }
        if (!touched) return b;
        out.write(in.readAllBytes());
        out.flush();
        changed++;
        return bo.toByteArray();
    }
}
