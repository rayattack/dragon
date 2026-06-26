fn main() {
    let mut s = String::new();
    for _ in 0..10000 {
        s.push_str("hello");
    }
    println!("{}", s.len());
}
