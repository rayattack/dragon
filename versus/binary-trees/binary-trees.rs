struct Node {
    left: Option<Box<Node>>,
    right: Option<Box<Node>>,
}

fn make(depth: i32) -> Box<Node> {
    if depth == 0 {
        Box::new(Node { left: None, right: None })
    } else {
        Box::new(Node {
            left: Some(make(depth - 1)),
            right: Some(make(depth - 1)),
        })
    }
}

fn check(node: &Node) -> i64 {
    match &node.left {
        None => 1,
        Some(l) => 1 + check(l) + check(node.right.as_ref().unwrap()),
    }
}

fn main() {
    let min_depth = 4;
    let max_depth = 14;
    let mut total: i64 = 0;
    total += check(&make(max_depth + 1));
    let mut depth = min_depth;
    while depth <= max_depth {
        let iterations: i64 = 1i64 << (max_depth - depth + min_depth);
        for _ in 0..iterations {
            total += check(&make(depth));
        }
        depth += 2;
    }
    println!("{}", total);
}
