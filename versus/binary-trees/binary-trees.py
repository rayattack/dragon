import sys
sys.setrecursionlimit(100000)


class Node:
    __slots__ = ('left', 'right')

    def __init__(self, left, right):
        self.left = left
        self.right = right


def make(depth):
    if depth == 0:
        return Node(None, None)
    return Node(make(depth - 1), make(depth - 1))


def check(node):
    if node.left is None:
        return 1
    return 1 + check(node.left) + check(node.right)


min_depth = 4
max_depth = 14
total = 0
total += check(make(max_depth + 1))
depth = min_depth
while depth <= max_depth:
    iterations = 1 << (max_depth - depth + min_depth)
    for _ in range(iterations):
        total += check(make(depth))
    depth += 2
print(total)
