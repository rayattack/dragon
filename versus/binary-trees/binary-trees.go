package main

import "fmt"

type Node struct {
	left, right *Node
}

func buildTree(depth int) *Node {
	n := &Node{}
	if depth > 0 {
		n.left = buildTree(depth - 1)
		n.right = buildTree(depth - 1)
	}
	return n
}

func check(n *Node) int64 {
	if n.left == nil {
		return 1
	}
	return 1 + check(n.left) + check(n.right)
}

func main() {
	minDepth, maxDepth := 4, 14
	var total int64 = 0
	total += check(buildTree(maxDepth + 1))
	for depth := minDepth; depth <= maxDepth; depth += 2 {
		iterations := int64(1) << uint(maxDepth-depth+minDepth)
		for i := int64(0); i < iterations; i++ {
			total += check(buildTree(depth))
		}
	}
	fmt.Println(total)
}
