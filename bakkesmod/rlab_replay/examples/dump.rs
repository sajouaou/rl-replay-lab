fn main() {
    let args: Vec<String> = std::env::args().collect();
    let blob = rlab_replay::parse_to_blob(&args[1]).expect("parse");
    std::fs::write(&args[2], blob).expect("write");
}
