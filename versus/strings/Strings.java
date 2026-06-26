public class Strings {
    public static void main(String[] args) {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < 10000; i++) {
            s.append("hello");
        }
        System.out.println(s.length());
    }
}
