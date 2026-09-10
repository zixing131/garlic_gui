package demo;

public class Main {
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

    public static class Details {
        public int version() { return 1; }
    }
}
