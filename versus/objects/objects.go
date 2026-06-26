package main

import "fmt"

type Point struct {
	x, y int64
}

func main() {
	var total int64 = 0
	for i := int64(0); i < 1000000; i++ {
		p := &Point{x: i, y: i + 1}
		total += p.x + p.y
		_ = p
	}
	fmt.Println(total)
}
