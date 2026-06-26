struct Point {
    x: i64,
    y: i64,
}

fn main() {
    let mut total: i64 = 0;
    for i in 0..1000000_i64 {
        let p = Box::new(Point { x: i, y: i + 1 });
        total += p.x + p.y;
    }
    println!("{}", total);
}
