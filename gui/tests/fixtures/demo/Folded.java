package demo;
public class Folded {
    public static String array() { return new String(new char[]{115, 101, 99, 114, 101, 116}); }
    public static String text() { return "xxsec".substring(2).concat("r-t").replace("-", "e"); }
    public static String unsafe() { return "x".substring(20); }
    public static int constant() { int x = 2147483647; return (x + 2) ^ 77; }
    public static int unknown(int x) { return x / 0; }
    public static int identity(int x) { return x ^ x; }
    public static boolean boolIdentity(boolean x) { return x ^ x; }
    public static String hostile() { return "*/\\u000a".concat("x"); }
}
