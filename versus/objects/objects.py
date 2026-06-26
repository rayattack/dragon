class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

total = 0
for i in range(1000000):
    p = Point(i, i + 1)
    total += p.x + p.y

print(total)
