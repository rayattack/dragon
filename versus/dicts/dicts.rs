use std::collections::HashMap;

fn main() {
    let n: i64 = 3000000;
    let k: i64 = 200000;
    let mut counts: HashMap<String, i64> = HashMap::new();
    for i in 0..n {
        *counts.entry((i % k).to_string()).or_insert(0) += 1;
    }
    let mut total: i64 = 0;
    for j in 0..k {
        total += j * counts[&j.to_string()];
    }
    println!("{}", total);
}
