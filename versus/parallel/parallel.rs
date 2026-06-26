use std::thread;

const WORKERS: i64 = 8;
const CHUNK: i64 = 30000000;
const M: i64 = 1000000007;

fn work(lo: i64, hi: i64) -> i64 {
    let mut acc = 0i64;
    let mut i = lo;
    while i < hi {
        acc = (acc + i) % M;
        i += 1;
    }
    acc
}

fn main() {
    let handles: Vec<_> = (0..WORKERS)
        .map(|w| thread::spawn(move || work(w * CHUNK, w * CHUNK + CHUNK)))
        .collect();
    let mut total = 0i64;
    for h in handles {
        total += h.join().unwrap();
    }
    println!("{}", total);
}
