fn main() {
    let width = 1600;
    let height = 1600;
    let maxiter = 100;
    let mut count: i64 = 0;
    for py in 0..height {
        let ci = 2.0 * py as f64 / height as f64 - 1.0;
        for px in 0..width {
            let cr = 2.0 * px as f64 / width as f64 - 1.5;
            let mut zr = 0.0f64;
            let mut zi = 0.0f64;
            let mut zr2 = 0.0f64;
            let mut zi2 = 0.0f64;
            let mut it = 0;
            while it < maxiter && zr2 + zi2 <= 4.0 {
                zi = 2.0 * zr * zi + ci;
                zr = zr2 - zi2 + cr;
                zr2 = zr * zr;
                zi2 = zi * zi;
                it += 1;
            }
            if zr2 + zi2 <= 4.0 {
                count += 1;
            }
        }
    }
    println!("{}", count);
}
