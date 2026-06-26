package main

import "fmt"

func main() {
	width, height, maxiter := 1600, 1600, 100
	var count int64 = 0
	for py := 0; py < height; py++ {
		ci := 2.0*float64(py)/float64(height) - 1.0
		for px := 0; px < width; px++ {
			cr := 2.0*float64(px)/float64(width) - 1.5
			zr, zi, zr2, zi2 := 0.0, 0.0, 0.0, 0.0
			it := 0
			for it < maxiter && zr2+zi2 <= 4.0 {
				zi = 2.0*zr*zi + ci
				zr = zr2 - zi2 + cr
				zr2 = zr * zr
				zi2 = zi * zi
				it++
			}
			if zr2+zi2 <= 4.0 {
				count++
			}
		}
	}
	fmt.Println(count)
}
