package demo;
public class Use {
    public String run(Main item) {
        return item.greet(2) + item.greet("literal greet should not change");
    }
}
