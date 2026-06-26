width = 1600
height = 1600
maxiter = 100
count = 0
py = 0
while py < height:
    ci = 2.0 * py / height - 1.0
    px = 0
    while px < width:
        cr = 2.0 * px / width - 1.5
        zr = 0.0
        zi = 0.0
        zr2 = 0.0
        zi2 = 0.0
        it = 0
        while it < maxiter and zr2 + zi2 <= 4.0:
            zi = 2.0 * zr * zi + ci
            zr = zr2 - zi2 + cr
            zr2 = zr * zr
            zi2 = zi * zi
            it += 1
        if zr2 + zi2 <= 4.0:
            count += 1
        px += 1
    py += 1
print(count)
