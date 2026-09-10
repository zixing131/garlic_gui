package demo;

@Marker(value = "中文\u0000\u0001\n\"\\😀")
public class Main implements Contract {
    private final String name;

    public Main(String name) {
        this.name = name;
    }

    public String greet(int count) {
        if (count > 0) {
            return "Hello, " + name;
        }
        return "Welcome to Garlic";
    }

    public String greet(String value) { return "greet: " + value; }

    public String unicode() { return "中文\u0000\u0001\n\"\\😀"; }

    public static class Details {
        public int version() { return 1; }
    }
}
