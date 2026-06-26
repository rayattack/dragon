


print("hello")


def do_something(name: int) -> int:
    while(name < 10):
        print(f'before: {name}')
        name = name + 1
        print(f'after: {name}')
    return name



do_something(4)
